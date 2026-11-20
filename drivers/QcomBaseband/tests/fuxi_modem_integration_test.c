/* Host integration test of the fuxi modem adapter: the real fuxi_modem.c and qcom_qrtr.c, on a fake SMEM
   region and against a fake modem that speaks the GLINK handshake over the same FIFOs.
   Built with -fsanitize=address,undefined by scripts/check.py.

   Why it exists: the adapter used to send the first HELLO from fuxi_modem_start(), right after the GLINK link was
   created. At that moment the channel is not up, qcom_glink_send() returns QcomGlinkNotOpen, the packet is
   dropped, and qrtr_send_from() only sets hello_sent when the send succeeds, so the HELLO never went out and the
   QRTR link never came up. A model of that code proves nothing; this test drives the adapter itself and watches
   the HELLO arrive on the wire.

   The fake modem is the peer side of the GLINK protocol over the same shared memory: it answers the version
   handshake, opens the IPCRTR channel, grants a receive intent and sends its own QRTR HELLO.

   The GLINK descriptor's four words, as Linux reads them (drivers/rpmsg/qcom_glink_smem.c):
     descs[0] the transmit tail, descs[1] the transmit head, descs[2] the receive tail, descs[3] the receive head.
   The AP transmits on FIFO_0 and receives on FIFO_1, so the modem transmits on FIFO_1 and receives on FIFO_0, and
   both sides advance the same words. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "QcomSmem.c"
#include "QcomGlink.c"
#include "qcom_qrtr.c"
#include "fuxi_modem.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- the fake SMEM region ---- */

#define SMEM_BYTES 0x40000u
#define PART_BYTES 0x30000u
#define PART_OFFSET 0x1000u
#define TOC_MAGIC 0x434f5424u
#define PART_MAGIC 0x54525024u
#define CANARY 0xa5a5u

static uint8_t smem[SMEM_BYTES];

static void put(uint32_t at, uint32_t value)
{
    memcpy(smem + at, &value, 4);
}

/* The GLINK items: the descriptor, the AP's FIFO_0 and the modem's FIFO_1. */
static uint32_t desc_at, tx_at, rx_at;

/* Builds the SMEM table: one partition for host 0 and remote 1 (the modem), holding the three GLINK items. */
static void build_smem(void)
{
    memset(smem, 0, sizeof smem);

    /* The partition header: magic, the host pair, the size, the free offsets. */
    put(PART_OFFSET + 0, PART_MAGIC);
    put(PART_OFFSET + 4, (1u << 16) | 0u);   /* host0 = 0 (AP), host1 = 1 (modem) */
    put(PART_OFFSET + 8, PART_BYTES);
    put(PART_OFFSET + 12, 32u);              /* the uncached list is empty for now */
    put(PART_OFFSET + 16, PART_BYTES);       /* the cached list is empty too */

    /* The table of contents, at the end of the region. */
    const uint32_t toc = SMEM_BYTES - 0x1000;
    put(toc + 0, TOC_MAGIC);
    put(toc + 4, 1);                          /* one partition */
    put(toc + 8, 1);                          /* one entry */
    const uint32_t entry = toc + 32;
    put(entry + 0, PART_OFFSET);
    put(entry + 4, PART_BYTES);
    put(entry + 12, (1u << 16) | 0u);
    put(entry + 16, 8);                       /* the cacheline size */

    /* The three GLINK items, as uncached entries: 16 bytes of metadata, then the payload. */
    uint32_t at = 32;
    const uint16_t ids[3] = {478, 479, 480};
    const uint32_t sizes[3] = {32, 0x4000, 0x4000};
    uint32_t *where[3] = {&desc_at, &tx_at, &rx_at};
    for (int i = 0; i < 3; ++i) {
        put(PART_OFFSET + at + 0, CANARY | ((uint32_t)ids[i] << 16));
        put(PART_OFFSET + at + 4, sizes[i]);
        put(PART_OFFSET + at + 8, 0);
        put(PART_OFFSET + at + 12, 0);
        *where[i] = PART_OFFSET + at + 16;
        at += 16 + sizes[i];
    }
    put(PART_OFFSET + 12, at);
}

/* ---- the fake modem ---- */

#define FIFO_BYTES 0x4000u

/* The descriptor's four words. */
#define DESC_TX_TAIL 0u
#define DESC_TX_HEAD 1u
#define DESC_RX_TAIL 2u
#define DESC_RX_HEAD 3u

static uint32_t desc_get(uint32_t which) { return *(volatile uint32_t *)(smem + desc_at + which * 4); }
static void desc_set(uint32_t which, uint32_t value) { *(volatile uint32_t *)(smem + desc_at + which * 4) = value; }

/* The modem transmits on FIFO_1 (the AP's receive side) and receives on FIFO_0 (the AP's transmit side). */
#define MOD_TX_DATA (smem + rx_at)
#define MOD_RX_DATA (smem + tx_at)

/* The channel the modem opens: its local id is 7, the AP learns it as the remote id. */
#define MOD_LCID 7u
#define MOD_CHANNEL_NAME "IPCRTR"
#define MOD_INTENT_ID 42u
#define MOD_INTENT_SIZE 4096u

/* The QRTR packets the adapter wrote onto the GLINK, seen by the modem. */
#define MAX_PACKETS 32
static uint8_t packets[MAX_PACKETS][512];
static uint32_t packet_len[MAX_PACKETS], packet_count;

/* The state of the fake modem's own handshake. */
static int modem_version_sent, modem_open_sent, modem_open_ack_sent, modem_intent_granted, modem_hello_sent;
/* The AP's channel id and the id of the receive intent it advertised, both learned from what it wrote. */
static uint32_t adapter_lcid;
static uint32_t adapter_intent_id;

static void mod_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Writes into the modem's transmit FIFO, advancing the shared head word. */
static void mod_put(const uint8_t *header, uint32_t header_len, const uint8_t *payload, uint32_t payload_len)
{
    uint32_t head = desc_get(DESC_RX_HEAD);
    for (uint32_t i = 0; i < header_len; ++i) {MOD_TX_DATA[head] = header[i]; if (++head == FIFO_BYTES) head = 0;}
    for (uint32_t i = 0; i < payload_len; ++i) {MOD_TX_DATA[head] = payload[i]; if (++head == FIFO_BYTES) head = 0;}
    head = (head + 7u) & ~7u;
    if (head >= FIFO_BYTES) head -= FIFO_BYTES;
    desc_set(DESC_RX_HEAD, head);
}

static void mod_command(uint16_t cmd, uint16_t param1, uint32_t param2)
{
    uint8_t m[8];
    m[0] = (uint8_t)cmd; m[1] = (uint8_t)(cmd >> 8); m[2] = (uint8_t)param1; m[3] = (uint8_t)(param1 >> 8);
    mod_put32(m + 4, param2);
    mod_put(m, 8, 0, 0);
}

/* Reads what the adapter wrote into the modem's receive FIFO and collects the QRTR packets that ride on it.

   The GLINK messages have different shapes, and each one has to be consumed by its own length: the bare commands
   (version, open ack, intent request) are 8 bytes, an OPEN carries the channel name after its header, an INTENT
   carries its descriptor after a 16-byte header, and a TX_DATA carries the payload after a 16-byte header. */
static void mod_drain(void)
{
    for (;;) {
        const uint32_t tail = desc_get(DESC_TX_TAIL);
        const uint32_t head = desc_get(DESC_TX_HEAD);
        const uint32_t avail = head >= tail ? head - tail : FIFO_BYTES - tail + head;
        if (avail < 8) break;
        uint8_t m[16];
        const uint32_t take = avail < 16 ? avail : 16;
        for (uint32_t i = 0; i < take; ++i) {
            uint32_t at = tail + i;
            if (at >= FIFO_BYTES) at -= FIFO_BYTES;
            m[i] = MOD_RX_DATA[at];
        }
        const uint16_t cmd = (uint16_t)(m[0] | m[1] << 8);
        const uint16_t param1 = (uint16_t)(m[2] | m[3] << 8);
        const uint32_t param2 = (uint32_t)m[4] | ((uint32_t)m[5] << 8) | ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);

        if (cmd == QCOM_GLINK_OPEN) {
            /* The AP opened the channel: remember its id so the answer can name it. The name follows the header. */
            adapter_lcid = param1;
            const uint32_t name_bytes = param2 & 0xffff;
            if (avail < 8 + name_bytes) break;
            desc_set(DESC_TX_TAIL, (tail + ((8 + name_bytes + 7u) & ~7u)) % FIFO_BYTES);
            continue;
        }
        if (cmd == QCOM_GLINK_INTENT) {
            /* The AP advertised a receive intent: its id is the last word of the 16-byte header. */
            if (avail < 16) break;
            adapter_intent_id = (uint32_t)m[12] | ((uint32_t)m[13] << 8) | ((uint32_t)m[14] << 16) | ((uint32_t)m[15] << 24);
            desc_set(DESC_TX_TAIL, (tail + 16) % FIFO_BYTES);
            continue;
        }
        if (cmd == QCOM_GLINK_TX_DATA) {
            if (avail < 16) break;
            const uint32_t chunk = (uint32_t)m[8] | ((uint32_t)m[9] << 8) | ((uint32_t)m[10] << 16) | ((uint32_t)m[11] << 24);
            const uint32_t intent_id = (uint32_t)m[4] | ((uint32_t)m[5] << 8) | ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);
            if (packet_count < MAX_PACKETS && chunk <= sizeof packets[0]) {
                uint32_t at = (tail + 16) % FIFO_BYTES;
                for (uint32_t i = 0; i < chunk; ++i) {
                    packets[packet_count][i] = MOD_RX_DATA[at];
                    if (++at == FIFO_BYTES) at = 0;
                }
                packet_len[packet_count] = chunk;
                packet_count++;
            }
            desc_set(DESC_TX_TAIL, (tail + ((16 + chunk + 7u) & ~7u)) % FIFO_BYTES);
            /* The modem has read the data, so it frees its receive intent. The real modem does this too: without it
               the AP's intent stays busy and the next packet has nowhere to go. */
            mod_command(QCOM_GLINK_RX_DONE_W_REUSE, MOD_LCID, intent_id);
            continue;
        }
        /* The bare commands: the version exchange, the open ack, the intent request. */
        (void)param1;
        (void)param2;
        desc_set(DESC_TX_TAIL, (tail + 8) % FIFO_BYTES);
    }
}

/* Finds the QRTR packet the adapter sent with this type and destination. */
static int find_packet(uint32_t type, uint32_t dst_node, uint32_t dst_port)
{
    for (uint32_t i = 0; i < packet_count; ++i) {
        if (packet_len[i] < QRTR_HDR_LEN) continue;
        if (rd32(packets[i] + 4) != type) continue;
        if (rd32(packets[i] + 24) != dst_node) continue;
        if (rd32(packets[i] + 28) != dst_port) continue;
        return (int)i;
    }
    return -1;
}

/* The fake modem's side of the handshake, one step per round.

   The AP is the responder on the channel: it waits for the modem's OPEN before it sends its own. So the modem
   opens first, then acknowledges the AP's OPEN, and only then is the channel up on both sides. */
static void modem_step(void)
{
    if (!modem_version_sent) {
        /* The modem starts the version exchange and acknowledges it, so the AP's link is agreed either way. */
        modem_version_sent = 1;
        mod_command(QCOM_GLINK_VERSION, 1, QCOM_GLINK_FEATURE_INTENT_REUSE);
        mod_command(QCOM_GLINK_VERSION_ACK, 1, QCOM_GLINK_FEATURE_INTENT_REUSE);
        return;
    }
    if (!modem_open_sent) {
        /* The modem opens the IPCRTR channel. The AP answers it with an OPEN_ACK and sends its own OPEN. */
        modem_open_sent = 1;
        uint8_t m[8];
        m[0] = QCOM_GLINK_OPEN; m[1] = 0; m[2] = (uint8_t)MOD_LCID; m[3] = 0;
        const uint32_t n = (uint32_t)strlen(MOD_CHANNEL_NAME) + 1;
        mod_put32(m + 4, n);
        mod_put(m, 8, (const uint8_t *)MOD_CHANNEL_NAME, n);
        return;
    }
    if (!modem_open_ack_sent && adapter_lcid) {
        /* The AP opened the channel in return: acknowledge it, which is what lets the AP mark the channel up. */
        modem_open_ack_sent = 1;
        mod_command(QCOM_GLINK_OPEN_ACK, (uint16_t)adapter_lcid, 0);
        return;
    }
    if (!modem_intent_granted && adapter_intent_id) {
        /* Grant one receive intent of the size the QRTR packets need, so the AP has somewhere to send them. */
        modem_intent_granted = 1;
        uint8_t m[16];
        m[0] = QCOM_GLINK_INTENT; m[1] = 0; m[2] = (uint8_t)MOD_LCID; m[3] = 0;
        mod_put32(m + 4, 1);
        mod_put32(m + 8, MOD_INTENT_SIZE);
        mod_put32(m + 12, MOD_INTENT_ID);
        mod_put(m, 16, 0, 0);
        return;
    }
    if (!modem_hello_sent && modem_intent_granted && adapter_intent_id) {
        /* The modem's own QRTR HELLO: node 5, on the control port, on the AP's advertised receive intent. */
        modem_hello_sent = 1;
        uint8_t q[QRTR_HDR_LEN];
        memset(q, 0, sizeof q);
        mod_put32(q + 0, QRTR_PROTO_VER_1);
        mod_put32(q + 4, QRTR_TYPE_HELLO);
        mod_put32(q + 8, 5);              /* the modem's node id */
        mod_put32(q + 12, QRTR_PORT_CTRL);
        mod_put32(q + 16, 0);
        mod_put32(q + 20, 0);
        mod_put32(q + 24, 1);            /* to us */
        mod_put32(q + 28, QRTR_PORT_CTRL);
        uint8_t h[16];
        h[0] = QCOM_GLINK_TX_DATA; h[1] = 0; h[2] = (uint8_t)MOD_LCID; h[3] = 0;
        mod_put32(h + 4, adapter_intent_id);
        mod_put32(h + 8, sizeof q);
        mod_put32(h + 12, 0);
        mod_put(h, 16, q, sizeof q);
    }
}

/* The adapter's kick: the fake modem needs no doorbell, the test drives both sides by hand. */
static void host_kick(void *context)
{
    (void)context;
}

/* The bridge hooks the adapter calls: they are not what this test is about, so they only count. */
static int bridge_configured;
static uint32_t bridge_new_servers;
void coachz_baseband_configure(void (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t),
                               void (*del_client)(uint32_t))
{
    (void)send; (void)del_client;
    bridge_configured = 1;
}
void coachz_baseband_poll(void) {}
void coachz_baseband_report(unsigned second) {(void)second;}
void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
    (void)service; (void)version_instance; (void)node; (void)port;
    bridge_new_servers++;
}
int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size)
{
    (void)dst_port; (void)src_node; (void)src_port; (void)body; (void)size;
    return 0;
}

/* ---- the test ---- */

static void test_the_hello_reaches_the_modem(void)
{
    build_smem();

    /* The descriptor starts empty: the AP resets the two words it owns, the modem resets the other two. */
    desc_set(DESC_TX_TAIL, 0);
    desc_set(DESC_TX_HEAD, 0);
    desc_set(DESC_RX_TAIL, 0);
    desc_set(DESC_RX_HEAD, 0);

    packet_count = 0;
    modem_version_sent = modem_open_ack_sent = modem_intent_granted = modem_hello_sent = 0;
    adapter_lcid = 0;

    fuxi_modem_link_config config;
    config.smem_base = smem;
    config.smem_size = SMEM_BYTES;
    config.kick = host_kick;
    config.kick_context = 0;

    /* The adapter starts. It must succeed: the SMEM partition and the GLINK items are there. */
    const int rc = fuxi_modem_start(&config);
    CHECK(rc == 0);
    CHECK(fuxi_link.channel != 0);

    /* The GLINK link is not up yet, so the HELLO cannot have gone out. This is the state the defect started from. */
    CHECK(fuxi_link.ready == 0);
    CHECK(packet_count == 0);

    /* Run the two sides together until the channel is up and the HELLO has been sent. */
    int hello_index = -1;
    /* The rounds continue past the HELLO: the modem's own HELLO comes after it, and the answer to that is what
       proves the protocol layer is alive in both directions. */
    for (int round = 0; round < 60; ++round) {
        fuxi_modem_poll();
        modem_step();
        mod_drain();
        if (hello_index < 0) hello_index = find_packet(QRTR_TYPE_HELLO, QRTR_NODE_BCAST, QRTR_PORT_CTRL);
    }

    /* The HELLO reached the modem. Before the fix it never did: the packet was dropped because the channel was not
       up when fuxi_modem_start() sent it. */
    CHECK(hello_index >= 0);
    if (hello_index >= 0) {
        CHECK(rd32(packets[hello_index] + 8) == FUXI_QRTR_LOCAL_NODE);
        CHECK(rd32(packets[hello_index] + 12) == QRTR_PORT_CTRL);
        CHECK(rd32(packets[hello_index] + 20) == 0);
    }

    /* The link is up and the protocol layer knows it sent its HELLO. */
    CHECK(fuxi_link.ready == 1);
    CHECK(fuxi_link.qrtr.hello_sent == 1);

    /* The modem's own HELLO was answered, addressed to the node the modem carried. */
    const int reply = find_packet(QRTR_TYPE_HELLO, 5, QRTR_PORT_CTRL);
    CHECK(reply >= 0);
    if (reply >= 0) CHECK(rd32(packets[reply] + 8) == FUXI_QRTR_LOCAL_NODE);
    CHECK(fuxi_link.qrtr.remote_node == 5);
}

int main(void)
{
    test_the_hello_reaches_the_modem();
    if (failures) {
        printf("FAIL fuxi_modem_integration_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_modem_integration_test\n");
    return 0;
}
