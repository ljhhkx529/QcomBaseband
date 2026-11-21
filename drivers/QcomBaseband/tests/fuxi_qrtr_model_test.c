/* Host model of the QRTR protocol layer over GLINK: the qrtr_hdr_v1 framing, the control packets and the name service.
   Built with -fsanitize=address,undefined by scripts/check.py. */
#include <stdio.h>
#include <string.h>
#include "qcom_qrtr.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* What the link saw: the packets the protocol layer wrote, kept as the bytes that went out. */
#define MAX_PACKETS 16
static uint8_t sent[MAX_PACKETS][256];
static uint32_t sent_len[MAX_PACKETS], sent_count;
static uint32_t last_server_service, last_server_node, last_server_port;
static uint32_t last_src_port, last_dst_node, last_dst_port;

static int link_send(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data, uint32_t length)
{
    (void)context;
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
    (void)context; (void)instance;
    last_server_service = service;
    last_server_node = node;
    last_server_port = port;
}

/* Builds the packet a peer would send us. */
static uint32_t build(uint8_t *out, uint32_t type, uint32_t src_node, uint32_t src_port, const void *body, uint32_t length)
{
    wr32(out + 0, QRTR_PROTO_VER_1);
    wr32(out + 4, type);
    wr32(out + 8, src_node);
    wr32(out + 12, src_port);
    wr32(out + 16, 0);
    wr32(out + 20, length);
    wr32(out + 24, 0);
    wr32(out + 28, QRTR_PORT_CTRL);
    if (length && body) memcpy(out + QRTR_HDR_LEN, body, length);
    return QRTR_HDR_LEN + length;
}

static void test_header(void)
{
    static struct qrtr_state q;
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    CHECK(qrtr_send(&q, 3, 0x22, QRTR_TYPE_DATA, "hi", 2) == -1);   /* no HELLO yet */
    CHECK(q.hello_dropped == 1);
    CHECK(sent_count == 0);
}

static void test_hello(void)
{
    static struct qrtr_state q;
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    CHECK(qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0) == 0);
    /* The control packet carries the command word the peer reads, in the body. The modem's own HELLO, the bytes
       the device sent on 2026-11-20, had a 20-byte body whose first word was the command. A control packet with
       no body has no command word for the peer's name service to read (Linux net/qrtr/ns.c switches on
       pkt->cmd), and upstream's receive path refuses a zero-size packet outright. */
    CHECK(sent_count == 1 && sent_len[0] == QRTR_HDR_LEN + QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + 0) == QRTR_PROTO_VER_1);
    CHECK(rd32(sent[0] + 4) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[0] + 8) == 7);
    CHECK(rd32(sent[0] + 28) == QRTR_PORT_CTRL);
    CHECK(rd32(sent[0] + 20) == QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 0) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 4) == 7);
    CHECK(q.hello_sent);
}

static void test_framing(void)
{
    static struct qrtr_state q;
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);
    sent_count = 0;

    /* The header and the payload go out as one buffer, the payload padded to four bytes. */
    const uint8_t body[] = { 0xde, 0xad, 0xbe, 0xef, 0x11 };
    CHECK(qrtr_send_data(&q, 0x4103, 3, 0x21, body, sizeof body) == 0);
    CHECK(sent_count == 1);
    CHECK(sent_len[0] == QRTR_HDR_LEN + 8);
    CHECK(rd32(sent[0] + 20) == sizeof body);
    CHECK(!memcmp(sent[0] + QRTR_HDR_LEN, body, sizeof body));
    CHECK(sent[0][QRTR_HDR_LEN + 5] == 0 && sent[0][QRTR_HDR_LEN + 6] == 0 && sent[0][QRTR_HDR_LEN + 7] == 0);
    /* A DATA packet carries the client's own local port as its source port: the modem routes the answer back by it. */
    CHECK(rd32(sent[0] + 12) == 0x4103);
    CHECK(rd32(sent[0] + 24) == 3);
    CHECK(rd32(sent[0] + 28) == 0x21);

    /* A payload that is already a multiple of four gets no padding. */
    sent_count = 0;
    CHECK(qrtr_send_data(&q, 0x4103, 3, 0x21, body, 4) == 0);
    CHECK(sent_count == 1 && sent_len[0] == QRTR_HDR_LEN + 4);

    /* A payload larger than the scratch is refused rather than truncated. */
    static uint8_t big[QRTR_MAX_PACKET];
    CHECK(qrtr_send_data(&q, 0x4103, 3, 0x21, big, sizeof big) == -1);
    CHECK(q.dropped == 1);
}

static void test_control_ports(void)
{
    static struct qrtr_state q;
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;

    /* A control packet carries the control port as its source port. */
    CHECK(qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0) == 0);
    CHECK(rd32(sent[0] + 12) == QRTR_PORT_CTRL);
    CHECK(rd32(sent[0] + 24) == 3);
    CHECK(rd32(sent[0] + 28) == QRTR_PORT_CTRL);

    /* A DEL_CLIENT goes to the broadcast node and names the local port that went away. */
    sent_count = 0;
    CHECK(qrtr_send_del_client(&q, 0x4107) == 0);
    CHECK(sent_count == 1 && sent_len[0] == QRTR_HDR_LEN + QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + 4) == QRTR_TYPE_DEL_CLIENT);
    CHECK(rd32(sent[0] + 12) == QRTR_PORT_CTRL);
    CHECK(rd32(sent[0] + 24) == QRTR_NODE_BCAST);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 0) == QRTR_TYPE_DEL_CLIENT);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 4) == 7);     /* our node */
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 8) == 0x4107); /* the port that went away */
}

static void test_new_server(void)
{
    static struct qrtr_state q;
    uint8_t body[QRTR_CTRL_LEN], packet[256];
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);
    last_server_service = 0;

    /* NEW_SERVER: cmd, service, instance, node, port */
    wr32(body + 0, QRTR_TYPE_NEW_SERVER);
    wr32(body + 4, 3);              /* NAS */
    wr32(body + 8, 1);              /* instance */
    wr32(body + 12, 0);             /* node: the modem's own */
    wr32(body + 16, 0x21);          /* port */
    uint32_t n = build(packet, QRTR_TYPE_NEW_SERVER, 0, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(last_server_service == 3 && last_server_port == 0x21);
    CHECK(q.servers == 1);
    CHECK(qrtr_lookup(&q, 3) == 0x21);

    /* A second instance of the same service: the lower instance wins. */
    wr32(body + 8, 2);
    wr32(body + 16, 0x22);
    n = build(packet, QRTR_TYPE_NEW_SERVER, 0, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(q.servers == 2);
    CHECK(qrtr_lookup(&q, 3) == 0x21);

    /* DEL_SERVER takes it away. */
    wr32(body + 0, QRTR_TYPE_DEL_SERVER);
    wr32(body + 8, 1);
    n = build(packet, QRTR_TYPE_DEL_SERVER, 0, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(q.servers == 1);
    CHECK(qrtr_lookup(&q, 3) == 0x22);
}

static uint32_t data_node, data_port, data_len, data_dst_port;
static uint8_t data_body[64];

static void link_data(void *context, uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t length)
{
    (void)context;
    data_node = src_node;
    data_port = src_port;
    data_dst_port = dst_port;
    data_len = length;
    if (length <= sizeof data_body) memcpy(data_body, body, length);
}

static void test_data(void)
{
    static struct qrtr_state q;
    uint8_t packet[256];
    qrtr_init(&q, 7, link_send, link_server, 0);
    qrtr_set_data_handler(&q, link_data);
    qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);

    const uint8_t body[] = { 0x00, 0x01, 0x02, 0x03, 0x04 };
    const uint32_t n = build(packet, QRTR_TYPE_DATA, 5, 0x21, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(data_node == 5 && data_port == 0x21 && data_len == sizeof body);
    /* The destination port is the bridge's local client port, which is what the reply is routed by. */
    CHECK(data_dst_port == QRTR_PORT_CTRL);
    CHECK(!memcmp(data_body, body, sizeof body));

    /* A truncated packet is counted, not acted on. */
    data_len = 0;
    qrtr_receive(&q, packet, QRTR_HDR_LEN - 1);
    CHECK(q.malformed == 1);
    CHECK(data_len == 0);
}

static void test_peer_hello(void)
{
    static struct qrtr_state q;
    uint8_t packet[256];
    uint8_t peer_body[QRTR_CTRL_LEN];
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    /* The peer's HELLO, built the way the modem builds it: the bytes the device sent on 2026-11-20 had a
       20-byte body whose first word was the command. */
    memset(peer_body, 0, sizeof peer_body);
    wr32(peer_body + 0, QRTR_TYPE_HELLO);
    wr32(peer_body + 4, 7907);
    wr32(peer_body + 8, 10);
    const uint32_t n = build(packet, QRTR_TYPE_HELLO, 9, QRTR_PORT_CTRL, peer_body, sizeof peer_body);
    qrtr_receive(&q, packet, n);
    CHECK(q.hello_received);
    CHECK(q.remote_node == 9 && q.remote_node_known);
    /* We answer with our own HELLO, addressed to the node the peer's HELLO carried. */
    CHECK(sent_count == 1 && rd32(sent[0] + 4) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[0] + 8) == 7);
    CHECK(rd32(sent[0] + 24) == 9);
    CHECK(rd32(sent[0] + 12) == QRTR_PORT_CTRL);
    /* And the answer carries the command word the peer reads, and our own node id where it looks for it. */
    CHECK(sent_len[0] == QRTR_HDR_LEN + QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + 20) == QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 0) == QRTR_TYPE_HELLO);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 4) == 7);
}

/* The lookup: the AP asks the modem which services it has. The modem answers with a NEW_SERVER per service and
   with an empty NEW_SERVER at the end of the list. */
static void test_lookup(void)
{
    static struct qrtr_state q;
    uint8_t body[QRTR_CTRL_LEN], packet[256], peer_body[QRTR_CTRL_LEN];
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);

    /* Before the peer's HELLO the modem's node is not known, so the lookup goes to the broadcast node. */
    CHECK(qrtr_send_lookup(&q, 0, 0, 0) == 0);
    CHECK(sent_count == 2);
    CHECK(last_dst_node == QRTR_NODE_BCAST);
    CHECK(last_dst_port == QRTR_PORT_CTRL);
    /* The body is a whole qrtr_ctrl_pkt: the command word, then the service and the instance out of the union. */
    CHECK(rd32(sent[1] + QRTR_HDR_LEN + 0) == QRTR_TYPE_NEW_LOOKUP);
    CHECK(rd32(sent[1] + QRTR_HDR_LEN + 4) == 0);
    CHECK(rd32(sent[1] + QRTR_HDR_LEN + 8) == 0);
    CHECK(sent_len[1] == QRTR_HDR_LEN + QRTR_CTRL_LEN);

    /* A lookup for one service carries the service id and the version in the low byte of the instance word
       (Linux drivers/soc/qcom/qmi_interface.c, qmi_send_new_lookup). */
    CHECK(qrtr_send_lookup(&q, 0x0b, 0x12, 0x03) == 0);
    CHECK(rd32(sent[2] + QRTR_HDR_LEN + 4) == 0x0b);
    CHECK(rd32(sent[2] + QRTR_HDR_LEN + 8) == 0x0312);

    /* The peer's HELLO body carries a node and a port of its own, next to the command word. The header names
       a different node. Both are kept: a service the peer holds on the body's node is not reachable from the
       header's node. */
    memset(peer_body, 0, sizeof peer_body);
    wr32(peer_body + 0, QRTR_TYPE_HELLO);
    wr32(peer_body + 4, 9);
    wr32(peer_body + 8, 10);
    uint32_t n = build(packet, QRTR_TYPE_HELLO, 9, QRTR_PORT_CTRL, peer_body, sizeof peer_body);
    qrtr_receive(&q, packet, n);
    CHECK(q.remote_node == 9 && q.remote_node_known);
    CHECK(q.peer_node == 9 && q.peer_node_known && q.peer_port == 10);
    CHECK(qrtr_send_lookup(&q, 0, 0, 0) == 0);
    CHECK(last_dst_node == 9);

    /* A lookup can be sent to a named node, which is how a service the peer holds on another node is asked for. */
    CHECK(qrtr_send_lookup_to(&q, 7907, 0x0b, 0, 0) == 0);
    CHECK(last_dst_node == 7907);
    CHECK(last_dst_port == QRTR_PORT_CTRL);
    CHECK(rd32(sent[sent_count - 1] + QRTR_HDR_LEN + 4) == 0x0b);

    /* The modem's answer: one NEW_SERVER per service, then the empty one that ends the list. */
    last_server_service = 0;
    wr32(body + 0, QRTR_TYPE_NEW_SERVER);
    wr32(body + 4, 0x0b);           /* UIM */
    wr32(body + 8, 0x02);
    wr32(body + 12, 0);             /* node */
    wr32(body + 16, 0x05);          /* port */
    n = build(packet, QRTR_TYPE_NEW_SERVER, 0, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(last_server_service == 0x0b);
    CHECK(q.servers == 1);
    CHECK(qrtr_lookup(&q, 0x0b) == 0x05);

    /* The empty NEW_SERVER: service 0 and port 0. It is the end of the listing, not a service
       (Linux net/qrtr/ns.c, lookup_notify: "Empty notification, to indicate end of listing"). */
    wr32(body + 4, 0);
    wr32(body + 16, 0);
    n = build(packet, QRTR_TYPE_NEW_SERVER, 0, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(q.servers == 1);
    /* Two answers: the service the modem published and the end of the listing. Both are answers, so a modem
       that answers with services and never ends its listing still moves the counter. */
    CHECK(q.lookups_answered == 2);
}

/* The flow control of upstream's net/qrtr/af_qrtr.c: the peer stops answering once its high watermark of
   outstanding DATA packets is reached, and the credit comes back with a RESUME_TX. Without this the link goes
   quiet in both directions after ten packets, with nothing dropped on our side. */
static void test_flow_control(void)
{
    static struct qrtr_state q;
    uint8_t packet[256], body[QRTR_CTRL_LEN];
    qrtr_init(&q, 7, link_send, link_server, 0);
    sent_count = 0;
    qrtr_send(&q, 3, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0);

    /* The confirm_rx flag goes on the packet that crosses the low watermark, and on no other. */
    for (uint32_t i = 1; i <= QRTR_TX_FLOW_LOW; i++) {
        CHECK(qrtr_send_data(&q, 0x4100, 9, 0x05, "hi", 2) == 0);
        CHECK(rd32(sent[sent_count - 1] + 16) == (i == QRTR_TX_FLOW_LOW ? 1u : 0u));
    }
    CHECK(q.flow[0].pending == QRTR_TX_FLOW_LOW);

    /* The high watermark refuses further packets rather than sending them into a peer that has no room. */
    for (uint32_t i = QRTR_TX_FLOW_LOW; i < QRTR_TX_FLOW_HIGH; i++)
        CHECK(qrtr_send_data(&q, 0x4100, 9, 0x05, "hi", 2) == 0);
    CHECK(q.flow[0].pending == QRTR_TX_FLOW_HIGH);
    CHECK(qrtr_send_data(&q, 0x4100, 9, 0x05, "hi", 2) == -1);
    CHECK(q.flow[0].pending == QRTR_TX_FLOW_HIGH);

    /* The peer's RESUME_TX frees the credit for the destination its body names. */
    memset(body, 0, sizeof body);
    wr32(body + 0, QRTR_TYPE_RESUME_TX);
    wr32(body + 4, 9);             /* the destination we addressed */
    wr32(body + 8, 0x05);          /* its port */
    uint32_t n = build(packet, QRTR_TYPE_RESUME_TX, 9, QRTR_PORT_CTRL, body, sizeof body);
    qrtr_receive(&q, packet, n);
    CHECK(q.resumes_received == 1);
    CHECK(q.flow[0].pending == 0);
    CHECK(qrtr_send_data(&q, 0x4100, 9, 0x05, "hi", 2) == 0);

    /* The peer's own credit: a DATA packet with the confirm_rx flag makes us answer with a RESUME_TX. The body
       names where the flagged packet was addressed, which is the destination the sender has to free (upstream's
       qrtr_send_resume_tx puts cb->dst_node and cb->dst_port in the body). The RESUME_TX itself goes back to where
       the packet came from. */
    memset(body, 0, sizeof body);
    n = build(packet, QRTR_TYPE_DATA, 9, 0x05, body, 0);
    wr32(packet + 16, 1);          /* confirm_rx */
    wr32(packet + 24, 7);          /* the node we addressed the packet to */
    wr32(packet + 28, 0x4100);     /* the port we addressed it to */
    qrtr_receive(&q, packet, n);
    CHECK(q.resumes_sent == 1);
    CHECK(rd32(sent[sent_count - 1] + QRTR_HDR_LEN + 0) == QRTR_TYPE_RESUME_TX);
    CHECK(rd32(sent[sent_count - 1] + QRTR_HDR_LEN + 4) == 7);
    CHECK(rd32(sent[sent_count - 1] + QRTR_HDR_LEN + 8) == 0x4100);
    CHECK(last_dst_node == 9);
    CHECK(last_dst_port == 0x05);
}

/* Announcing one of our own services: the NEW_SERVER the peer reads to learn that the service exists and
   where it is. The body carries the command word, the service, the instance word (the version in the low
   byte and the instance in the high one), our own node and the local port the service is served on. */
static void test_send_new_server(void)
{
    static struct qrtr_state q;
    qrtr_init(&q, 1, link_send, link_server, 0);
    sent_count = 0;
    /* The HELLO goes out first: a DATA or a control packet that is not a HELLO is refused before our own
       HELLO has gone out, because the peer's name service ignores it. */
    CHECK(qrtr_send(&q, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_HELLO, 0, 0) == 0);
    sent_count = 0;

    /* The service registry's locator: service 0x40, version 1, instance 1, on the local port 0x4001. */
    CHECK(qrtr_send_new_server(&q, 0, 0x40, 1, 1, 0x4001) == 0);
    CHECK(sent_count == 1);
    CHECK(sent_len[0] == QRTR_HDR_LEN + QRTR_CTRL_LEN);
    CHECK(rd32(sent[0] + 4) == QRTR_TYPE_NEW_SERVER);
    /* A control packet: the control port as its source and destination, and the node it is addressed to. */
    CHECK(rd32(sent[0] + 12) == QRTR_PORT_CTRL);
    CHECK(rd32(sent[0] + 24) == 0);
    CHECK(rd32(sent[0] + 28) == QRTR_PORT_CTRL);
    /* The body: the command word the peer switches on, the service, the instance word, our node and the port.
       The instance word of version 1 and instance 1 is 0x0101, which is the value 257. */
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 0) == QRTR_TYPE_NEW_SERVER);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 4) == 0x40);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 8) == 0x0101);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 12) == 1);
    CHECK(rd32(sent[0] + QRTR_HDR_LEN + 16) == 0x4001);
}

int main(void)
{
    test_header();
    test_hello();
    test_framing();
    test_control_ports();
    test_new_server();
    test_send_new_server();
    test_data();
    test_peer_hello();
    test_lookup();
    test_flow_control();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("qrtr model: ok\n");
    return 0;
}
