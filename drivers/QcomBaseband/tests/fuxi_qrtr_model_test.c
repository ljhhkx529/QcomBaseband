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

int main(void)
{
    test_header();
    test_hello();
    test_framing();
    test_control_ports();
    test_new_server();
    test_data();
    test_peer_hello();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("qrtr model: ok\n");
    return 0;
}
