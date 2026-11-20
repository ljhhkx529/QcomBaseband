/* Host model of the fuxi modem adapter's HELLO: the first HELLO cannot go out until the GLINK channel is up,
   because until the modem opens the channel the packet has nowhere to go.
   Built with -fsanitize=address,undefined by scripts/check.py.

   The defect this covers: the adapter used to send the HELLO from fuxi_modem_start(), right after the GLINK link
   was created. At that point the channel is not up, qcom_glink_send() returns QcomGlinkNotOpen, the packet is
   dropped, and qrtr_send_from() only sets hello_sent when the send succeeds, so the HELLO was never sent and the
   QRTR link never came up. The HELLO is now owed (hello_pending) and sent from the owner's poll, where the channel
   state is known. */
#include <stdio.h>
#include <string.h>
#include "qcom_qrtr.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* What the link saw. */
#define MAX_PACKETS 16
static uint8_t sent[MAX_PACKETS][256];
static uint32_t sent_len[MAX_PACKETS], sent_count;
static uint32_t last_src_port, last_dst_node, last_dst_port;

/* The link's own state, which the adapter does not see: whether the channel is up, and whether the remote has an
   intent to take the packet. */
static int channel_up;
static int remote_has_intent;

/* The send hook: the same shape as fuxi_qrtr_send(). A send on a channel that is not up is refused, which is what
   dropped the HELLO. */
static int link_send(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data,
                     uint32_t length)
{
    (void)context;
    if (!channel_up) return -1;
    if (!remote_has_intent) return -1;
    if (sent_count >= MAX_PACKETS || length > sizeof sent[0]) return -1;
    memcpy(sent[sent_count], data, length);
    sent_len[sent_count] = length;
    last_src_port = src_port;
    last_dst_node = dst_node;
    last_dst_port = dst_port;
    sent_count++;
    return 0;
}

static void link_server(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    (void)context; (void)service; (void)instance; (void)node; (void)port;
}

/* The adapter's HELLO handling, mirroring fuxi_modem.c: the HELLO is owed, and the owner's poll sends it once the
   channel is up. */
struct adapter {
    struct qrtr_state qrtr;
    uint8_t hello_pending;
    uint32_t tx_again;
};

static void adapter_start(struct adapter *a)
{
    qrtr_init(&a->qrtr, 1, link_send, link_server, 0);
    a->hello_pending = 1;
}

static void adapter_poll(struct adapter *a)
{
    if (!a->hello_pending || !channel_up) return;
    const int result = qrtr_send(&a->qrtr, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);
    if (result == 0) {
        a->hello_pending = 0;
        return;
    }
    a->tx_again++;
}

/* The 32-bit little-endian accessors come from qcom_qrtr.c. */

/* The HELLO is owed, not lost: it goes out once the channel is up. */
static void test_hello_waits_for_the_channel(void)
{
    struct adapter a;
    channel_up = 0;
    remote_has_intent = 1;
    sent_count = 0;
    adapter_start(&a);

    /* The poll before the channel is up sends nothing, and the HELLO is still owed. */
    adapter_poll(&a);
    CHECK(sent_count == 0);
    CHECK(a.hello_pending == 1);
    CHECK(a.qrtr.hello_sent == 0);

    /* The channel comes up. Now the HELLO goes out. */
    channel_up = 1;
    adapter_poll(&a);
    CHECK(sent_count == 1);
    CHECK(a.hello_pending == 0);
    CHECK(a.qrtr.hello_sent == 1);

    /* It is a HELLO to the broadcast node on the control port, with no body. */
    CHECK(rd32(sent[0] + 4) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[0] + 8) == 1);              /* our node id */
    CHECK(rd32(sent[0] + 12) == QRTR_PORT_CTRL); /* the control port */
    CHECK(rd32(sent[0] + 24) == QRTR_NODE_BCAST);
    CHECK(rd32(sent[0] + 28) == QRTR_PORT_CTRL);
    CHECK(rd32(sent[0] + 20) == 0);              /* no body */
}

/* The old behaviour: sending the HELLO from the start call, before the channel is up, loses it. The protocol layer
   does not set hello_sent, so the link never comes up. This is the defect, kept as the test that proves the fix
   changed the outcome. */
static void test_the_defect_would_have_lost_the_hello(void)
{
    struct qrtr_state q;
    channel_up = 0;
    remote_has_intent = 1;
    sent_count = 0;
    qrtr_init(&q, 1, link_send, link_server, 0);

    /* What fuxi_modem_start() used to do. */
    CHECK(qrtr_send(&q, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0) != 0);
    CHECK(sent_count == 0);
    CHECK(q.hello_sent == 0);

    /* And because hello_sent is not set, the data plane is refused as well. */
    CHECK(qrtr_send_data(&q, 0x10, 2, 3, "hi", 2) != 0);
    CHECK(q.hello_dropped == 1);
}

/* The remote has no intent yet: the HELLO stays owed and is asked for again, not lost. */
static void test_hello_retries_until_the_remote_has_an_intent(void)
{
    struct adapter a;
    channel_up = 1;
    remote_has_intent = 0;
    sent_count = 0;
    adapter_start(&a);

    adapter_poll(&a);
    CHECK(sent_count == 0);
    CHECK(a.hello_pending == 1);
    CHECK(a.tx_again == 1);

    adapter_poll(&a);
    CHECK(a.tx_again == 2);
    CHECK(a.hello_pending == 1);

    /* The intent arrives. Now it goes out. */
    remote_has_intent = 1;
    adapter_poll(&a);
    CHECK(sent_count == 1);
    CHECK(a.hello_pending == 0);
    CHECK(a.qrtr.hello_sent == 1);
}

/* The modem's own HELLO is answered, and the answer is addressed to the node the modem carried. */
static void test_the_hello_is_answered(void)
{
    struct adapter a;
    channel_up = 1;
    remote_has_intent = 1;
    sent_count = 0;
    adapter_start(&a);
    adapter_poll(&a);
    CHECK(sent_count == 1);

    /* The modem's HELLO: node 5, control port. */
    uint8_t packet[64];
    memset(packet, 0, sizeof packet);
    wr32(packet + 0, QRTR_PROTO_VER_1);
    wr32(packet + 4, QRTR_TYPE_HELLO);
    wr32(packet + 8, 5);               /* src node */
    wr32(packet + 12, QRTR_PORT_CTRL);  /* src port */
    wr32(packet + 16, 0);
    wr32(packet + 20, 0);              /* no body */
    wr32(packet + 24, 1);              /* dst node: us */
    wr32(packet + 28, QRTR_PORT_CTRL);
    qrtr_receive(&a.qrtr, packet, QRTR_HDR_LEN);

    /* The answer goes to the modem's node, on the control port. */
    CHECK(sent_count == 2);
    CHECK(rd32(sent[1] + 4) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[1] + 24) == 5);
    CHECK(rd32(sent[1] + 28) == QRTR_PORT_CTRL);
    CHECK(a.qrtr.remote_node == 5);
}

int main(void)
{
    test_hello_waits_for_the_channel();
    test_the_defect_would_have_lost_the_hello();
    test_hello_retries_until_the_remote_has_an_intent();
    test_the_hello_is_answered();
    if (failures) {
        printf("FAIL fuxi_modem_hello_model_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_modem_hello_model_test\n");
    return 0;
}
