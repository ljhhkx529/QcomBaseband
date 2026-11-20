/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The fuxi's IPCRTR transport: the QRTR protocol layer over the modem's GLINK channel in shared memory.
 *
 * The path is the one Linux builds for this board: remoteproc_mpss brings the modem up, its glink-edge gives the
 * AP a GLINK link over SMEM (remote pid 1), and the "IPCRTR" channel on that link carries QRTR packets whole. Linux
 * reaches the same place through rpmsg (drivers/rpmsg/qcom_glink_smem.c plus net/qrtr/smd.c); XNU has no rpmsg, so
 * this file speaks GLINK directly and hands the payloads to the QRTR layer.
 *
 * The GLINK, SMEM and SCM plumbing is the device repo's (package/QcomGlink.c, package/QcomSmem.c): this file only
 * knows how to find the modem's link, how to carry one QRTR packet over it, and how to hand what arrives to the
 * bridge.
 *
 * Every entry point runs on the modem's owner thread, never on an ifnet or IOKit caller's thread.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>

#include "qcom_qrtr.h"

#include "QcomGlink.h"
#include "QcomSmem.h"

/* The modem's SMEM partition and its GLINK items. The item ids are the ones Linux uses for every GLINK edge
   (drivers/rpmsg/qcom_glink_smem.c: SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR 478, FIFO_0 479, FIFO_1 480); the partition
   is the one shared with remote pid 1, the modem. The ADSP's edge is remote pid 2 and is a different partition. */
#define FUXI_GLINK_DESC_ITEM 478u
#define FUXI_GLINK_TX_ITEM 479u
#define FUXI_GLINK_RX_ITEM 480u
#define FUXI_MODEM_PID 1u
#define FUXI_GLINK_DESC_BYTES 32u

/* The channel the modem opens for IPCRTR, and the intent it grants on it. Linux matches the rpmsg device by the
   name the remote opens the channel with (net/qrtr/smd.c matches "IPCRTR"). */
#define FUXI_IPCRTR_CHANNEL "IPCRTR"
/* The modem's IPCRTR intents are 4 KiB on this class of SoC; Linux asks for what the remote offers and this is the
   size the fuxi's modem grants. It is a bound, not a proof: a smaller grant is handled by the intent handshake. */
#define FUXI_IPCRTR_INTENT_SIZE 4096u
#define FUXI_IPCRTR_INTENT_COUNT 8u

/* The QRTR node the AP claims. Linux leaves it to the endpoint registration (QRTR_EP_NID_AUTO) but its local node id
   is 1 and the Trogdor reference port uses the same; the modem addresses the AP by it. */
#define FUXI_QRTR_LOCAL_NODE QRTR_LOCAL_NODE

/* What the board must hand the adapter before it can start. */
typedef struct {
    /* The SMEM window, already mapped. */
    volatile uint8_t *smem_base;
    uint32_t smem_size;
    /* Rings the remote when the link has something to say. */
    void (*kick)(void *context);
    void *kick_context;
} fuxi_modem_link_config;

/* What the link has seen, for the board's own report. The board's probe owns the bring-up and reads these
   to say whether the data plane is up, so the numbers have to be reachable from outside. */
typedef struct {
    uint8_t ready;          /* the modem opened the channel */
    uint8_t hello_sent;     /* the first HELLO went out */
    uint8_t reserved[2];
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t tx_again;
    uint32_t open_failures;
    uint32_t qrtr_servers;
    /* Where the traffic stops. The link can be up while nothing arrives: the counters below say whether the
       modem's bytes are sitting in the receive FIFO, whether the GLINK layer dropped them, or whether the QRTR
       layer found them malformed. Without them there is no way to tell those three apart from the outside. */
    uint32_t glink_rx_messages;   /* the GLINK layer's messages handled */
    uint32_t glink_tx_messages;   /* the GLINK layer's messages written */
    uint32_t glink_rx_data;       /* the GLINK layer's data messages delivered */
    uint32_t glink_dropped;       /* the GLINK layer's data it could not deliver */
    uint32_t glink_unknown_opens; /* channel opens for a name we do not have */
    uint32_t rx_avail;            /* bytes waiting in the receive FIFO right now */
    uint32_t ch_up, ch_remote_open, ch_open_sent, ch_open_acked;
    uint32_t ch_local_advertised; /* our receive intents the modem knows about */
    uint32_t qrtr_rx, qrtr_malformed, qrtr_hello_received, qrtr_hello_dropped;
    /* The name service's state. The modem's node id is learned from its HELLO; the reply to that HELLO is
     * owed until the link has room for it, and the reply has to go back to the node and port the modem
     * carried. If the reply never lands, the modem never announces its services. */
    uint32_t qrtr_remote_node, qrtr_hello_reply_node, qrtr_hello_reply_port;
    uint8_t qrtr_hello_reply_owed, qrtr_remote_node_known;
    uint32_t glink_remote_intents;  /* the receive intents the modem granted us */
    char last_unknown[QCOM_GLINK_NAME];
} fuxi_modem_link_status;

/* The adapter's state. */
typedef struct {
    fuxi_modem_link_config config;
    QcomSmem smem;
    QcomSmemPartition partition;
    QcomGlink glink;
    QcomGlinkChannel *channel;
    struct qrtr_state qrtr;
    /* The counters the report shows. */
    uint32_t rx_packets, tx_packets, tx_again, open_failures;
    char last_error[96];
    uint8_t ready;
    /* The first HELLO cannot go out until the GLINK channel is up: the modem opens the channel, and until it does
       the packet has nowhere to go. The flag says the HELLO is still owed. */
    uint8_t hello_pending;
    /* The HELLO went out. */
    uint8_t hello_sent;
} fuxi_modem_link;

static fuxi_modem_link fuxi_link;

/* The QRTR layer's send hook: one QRTR packet, header and payload already contiguous. The source port is the local
   port the packet comes from: the control port for a control packet, the client's own port for a DATA packet. */
static int fuxi_qrtr_send(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data,
                          uint32_t length)
{
    fuxi_modem_link *link = context;
    (void)src_port;
    (void)dst_node;
    (void)dst_port;
    if (!link->channel) return -1;
    const int result = qcom_glink_send(&link->glink, link->channel, data, length);
    if (result == QcomGlinkAgain) {
        /* No remote intent yet: the owner asks again on the next event. */
        link->tx_again++;
        return -1;
    }
    if (result != QcomGlinkOk) {
        snprintf(link->last_error, sizeof link->last_error, "glink send failed (%d)", result);
        return -1;
    }
    link->tx_packets++;
    return 0;
}

/* The QRTR layer's service hook: the modem announced a service on a port. */
static void fuxi_qrtr_new_server(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    (void)context;
    extern void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
    coachz_baseband_new_server(service, instance, node, port);
}

/* The QRTR layer's data hook: one payload for the bridge. The destination port is the bridge's local client port. */
static void fuxi_qrtr_data(void *context, uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body,
                           uint32_t length)
{
    fuxi_modem_link *link = context;
    link->rx_packets++;
    extern int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
    coachz_baseband_qrtr(dst_port, src_node, src_port, body, length);
}

/* The GLINK channel's receive hook: a whole QRTR packet. */
static void fuxi_glink_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)context; (void)channel;
    fuxi_modem_link *link = &fuxi_link;
    qrtr_receive(&link->qrtr, data, length);
}

/* The GLINK channel's state hook. */
static void fuxi_glink_state(void *context, QcomGlinkChannel *channel, int up)
{
    (void)context; (void)channel;
    if (up) fuxi_link.ready = 1;
}

/* Sends the first HELLO, once there is a channel to send it on. */
static void fuxi_send_hello(void)
{
    if (!fuxi_link.hello_pending) return;
    /* The first HELLO goes to the broadcast node: the modem's node id is not known until its own HELLO arrives. The
       reply HELLO, sent by the protocol layer, is addressed to the node id the modem carried. */
    const int result = qrtr_send(&fuxi_link.qrtr, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);
    if (result == 0) {
        fuxi_link.hello_pending = 0;
        fuxi_link.hello_sent = 1;
        return;
    }
    /* The channel is up but the remote has no intent yet: ask again on the next event. */
    fuxi_link.tx_again++;
}
/* Finds an item in the modem's partition and returns its bytes. */
static volatile uint8_t *fuxi_item(fuxi_modem_link *link, uint16_t id, uint32_t *size)
{
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&link->smem, &link->partition, id, &at, &bytes)) return 0;
    if (size) *size = bytes;
    return link->config.smem_base + at;
}

/* Brings the modem's GLINK link up: the descriptor's four words are the two FIFO heads and tails, the FIFO_0 item is
   the transmit side and FIFO_1 the receive side. */
static int fuxi_glink_start(fuxi_modem_link *link)
{
    uint32_t desc_size = 0, tx_size = 0, rx_size = 0;
    volatile uint8_t *desc = fuxi_item(link, FUXI_GLINK_DESC_ITEM, &desc_size);
    volatile uint8_t *tx = fuxi_item(link, FUXI_GLINK_TX_ITEM, &tx_size);
    volatile uint8_t *rx = fuxi_item(link, FUXI_GLINK_RX_ITEM, &rx_size);
    if (!desc || desc_size < FUXI_GLINK_DESC_BYTES || !tx || !tx_size || !rx || !rx_size) {
        snprintf(link->last_error, sizeof link->last_error,
                 "glink items missing (desc %u, tx %u, rx %u)", desc_size, tx_size, rx_size);
        return -1;
    }
    /* The descriptor holds, in order, the transmit tail, transmit head, receive tail and receive head. Linux reads
       them as descs[0..3] (drivers/rpmsg/qcom_glink_smem.c) and resets the two it owns to zero. */
    volatile uint32_t *words = (volatile uint32_t *)desc;
    const QcomGlinkFifo tx_fifo = {&words[1], &words[0], tx, tx_size};
    const QcomGlinkFifo rx_fifo = {&words[3], &words[2], rx, rx_size};
    qcom_glink_init(&link->glink, tx_fifo, rx_fifo, link->config.kick, link->config.kick_context);

    static uint8_t storage[FUXI_IPCRTR_INTENT_SIZE * FUXI_IPCRTR_INTENT_COUNT];
    const QcomGlinkClient client = {fuxi_glink_state, fuxi_glink_receive};
    /* The modem opens the channel, so the adapter answers rather than opening it. */
    link->channel = qcom_glink_channel(&link->glink, FUXI_IPCRTR_CHANNEL, FUXI_IPCRTR_INTENT_SIZE,
                                       FUXI_IPCRTR_INTENT_COUNT, storage, &client, link, 0);
    if (!link->channel) {
        snprintf(link->last_error, sizeof link->last_error, "glink channel %s refused", FUXI_IPCRTR_CHANNEL);
        link->open_failures++;
        return -1;
    }
    return 0;
}

/* Starts the modem's QRTR: finds the partition, brings up the link, and says HELLO. */
int fuxi_modem_start(const fuxi_modem_link_config *config)
{
    fuxi_modem_link *link = &fuxi_link;
    __builtin_memset(link, 0, sizeof *link);
    link->config = *config;
    link->smem.base = config->smem_base;
    link->smem.size = config->smem_size;
    if (qcom_smem_partition(&link->smem, QCOM_SMEM_HOST_APPS, FUXI_MODEM_PID, &link->partition)) {
        snprintf(link->last_error, sizeof link->last_error, "no SMEM partition with the modem (pid %u)", FUXI_MODEM_PID);
        return -1;
    }
    if (fuxi_glink_start(link)) return -1;

    qrtr_init(&link->qrtr, FUXI_QRTR_LOCAL_NODE, fuxi_qrtr_send, fuxi_qrtr_new_server, link);
    qrtr_set_data_handler(&link->qrtr, fuxi_qrtr_data);
    /* The HELLO is owed, not sent: the GLINK channel is only up once the modem opens it, and a HELLO sent before
       that is dropped by the link with nothing to show for it. The owner's poll sends it as soon as the channel is
       up. */
    link->hello_pending = 1;
    return 0;
}

/* The owner's poll: runs the GLINK, then answers what the modem asked for. */
void fuxi_modem_poll(void)
{
    fuxi_modem_link *link = &fuxi_link;
    if (!link->channel) return;
    qcom_glink_poll(&link->glink);
    /* The HELLO waits here: by the time the channel is up there is somewhere to send it. */
    if (link->hello_pending && link->ready) fuxi_send_hello();
    /* The answer to the modem's HELLO is owed until the link has room for it: the remote's receive intent is still
       held by the packet that carried the HELLO. */
    if (link->qrtr.hello_reply_owed) qrtr_flush_hello_reply(&link->qrtr);
    extern void coachz_baseband_poll(void);
    coachz_baseband_poll();
}

/* The bridge's send hook: one QMI message for a client. The local port is the client's own port, because the modem
   routes the answer back by it; the node and port are the service's, from the modem's NEW_SERVER. */
static void fuxi_bridge_send(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length)
{
    fuxi_modem_link *link = &fuxi_link;
    if (!link->channel) return;
    if (qrtr_send_data(&link->qrtr, local_port, node, port, message, length)) {
        snprintf(link->last_error, sizeof link->last_error, "QRTR data to %u:%u refused", node, port);
    }
}

/* The bridge's release hook: a client that goes away is announced to the modem, so it stops sending to that port. */
static void fuxi_bridge_del_client(uint32_t local_port)
{
    fuxi_modem_link *link = &fuxi_link;
    if (!link->channel) return;
    qrtr_send_del_client(&link->qrtr, local_port);
}

/* The bridge's publish hook, called once the QRTR link is up. The bridge publishes nothing unless
   /chosen/coachz-baseband says so. */
void fuxi_modem_publish(void)
{
    extern void coachz_baseband_configure(void (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t),
                                          void (*del_client)(uint32_t));
    coachz_baseband_configure(fuxi_bridge_send, fuxi_bridge_del_client);
}

/* What the link has seen, for the board's own probe to read. */
void fuxi_modem_status(fuxi_modem_link_status *status)
{
    if (!status) return;
    fuxi_modem_link *link = &fuxi_link;
    /* The caller's struct is not cleared: the counts below are read one at a time and the caller keeps its own
     * copy of the earlier ones. Only the fields this function does not fill are cleared. */
    status->ready = link->ready;
    status->hello_sent = link->hello_sent;
    status->rx_packets = link->rx_packets;
    status->tx_packets = link->tx_packets;
    status->tx_again = link->tx_again;
    status->open_failures = link->open_failures;
    status->qrtr_servers = link->qrtr.servers;
    status->glink_rx_messages = link->glink.rx_messages;
    status->glink_tx_messages = link->glink.tx_messages;
    status->glink_rx_data = link->glink.rx_data;
    status->glink_dropped = link->glink.dropped;
    status->glink_unknown_opens = link->glink.unknown_opens;
    status->rx_avail = qcom_glink_rx_avail(&link->glink.rx);
    status->ch_up = 0;
    status->ch_remote_open = 0;
    status->ch_open_sent = 0;
    status->ch_open_acked = 0;
    status->ch_local_advertised = 0;
    if (link->channel) {
        status->ch_up = link->channel->up;
        status->ch_remote_open = link->channel->remote_open;
        status->ch_open_sent = link->channel->open_sent;
        status->ch_open_acked = link->channel->open_acked;
        for (unsigned k = 0; k < link->channel->intent_count; ++k)
            if (link->channel->local[k].advertised) status->ch_local_advertised++;
    }
    status->qrtr_rx = link->qrtr.rx;
    status->qrtr_malformed = link->qrtr.malformed;
    status->qrtr_hello_received = link->qrtr.hello_received;
    status->qrtr_hello_dropped = link->qrtr.hello_dropped;
    status->qrtr_remote_node = link->qrtr.remote_node;
    status->qrtr_hello_reply_node = link->qrtr.hello_reply_node;
    status->qrtr_hello_reply_port = link->qrtr.hello_reply_port;
    status->qrtr_hello_reply_owed = link->qrtr.hello_reply_owed;
    status->qrtr_remote_node_known = link->qrtr.remote_node_known;
    status->glink_remote_intents = 0;
    if (link->channel) {
        for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
            if (link->channel->remote[k].valid) status->glink_remote_intents++;
    }
    for (unsigned b = 0; b < QCOM_GLINK_NAME; ++b) status->last_unknown[b] = link->glink.last_unknown[b];
}

/* What the link has seen, for the periodic report. */
void fuxi_modem_report(unsigned second)
{
    fuxi_modem_link *link = &fuxi_link;
    extern void coachz_baseband_report(unsigned second);
    coachz_baseband_report(second);
    printf("FUXI modem: %u s: link %s, QRTR tx %u rx %u again %u, servers %u, glink tx %u rx %u\n", second,
           link->ready ? "up" : "down", link->tx_packets, link->rx_packets, link->tx_again, link->qrtr.servers,
           link->glink.tx_messages, link->glink.rx_messages);
    if (link->last_error[0]) printf("FUXI modem: last error: %s\n", link->last_error);
}

/* The data path. The bridge's PDP layer hands packets here; the fuxi's has no IPA behind it yet, so nothing goes
   out and every attempt says so rather than dropping the packet silently. The control plane (SIM status, signal
   information) does not use this path: it rides the QRTR control channel, which is up. */
static void (*fuxi_data_receiver)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version);

void coachz_modem_data_receiver(void (*receiver)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version))
{
    fuxi_data_receiver = receiver;
}

int coachz_modem_data_send(uint32_t mux, const uint8_t *ip, uint32_t length)
{
    (void)mux; (void)ip; (void)length;
    return ENETDOWN;
}
