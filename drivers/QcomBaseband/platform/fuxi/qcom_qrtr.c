/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The QRTR protocol layer over the fuxi's GLINK-SMEM link: the 32-byte qrtr_hdr_v1 framing, the control packets and
 * the name service. Portable: no kernel types beyond the fixed-width integers, so the model test can run it on the host.
 */
#include "qcom_qrtr.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static struct qrtr_server *server_find(struct qrtr_state *q, uint32_t service, uint32_t instance)
{
    /* The whole array is walked, not the first `servers` entries: a removed server leaves a hole. */
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++)
        if (q->server[i].present && q->server[i].service == service && q->server[i].instance == instance)
            return &q->server[i];
    return 0;
}

static struct qrtr_server *server_add(struct qrtr_state *q, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    struct qrtr_server *s = server_find(q, service, instance);
    if (s) {
        s->node = node;
        s->port = port;
        return s;
    }
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++) {
        if (q->server[i].present) continue;
        q->server[i].service = service;
        q->server[i].instance = instance;
        q->server[i].node = node;
        q->server[i].port = port;
        q->server[i].present = 1;
        q->servers++;
        return &q->server[i];
    }
    return 0;
}

static void server_remove(struct qrtr_state *q, uint32_t service, uint32_t instance)
{
    struct qrtr_server *s = server_find(q, service, instance);
    if (!s) return;
    s->present = 0;
    q->servers--;
}

void qrtr_init(struct qrtr_state *q, uint32_t local_node, qrtr_send_fn send, qrtr_server_fn server, void *context)
{
    memset(q, 0, sizeof *q);
    q->local_node = local_node;
    q->send = send;
    q->server_fn = server;
    q->context = context;
}

void qrtr_set_data_handler(struct qrtr_state *q, qrtr_data_fn data)
{
    q->data_fn = data;
}

void qrtr_set_trace(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                    void *context)
{
    q->trace = trace;
    q->trace_context = context;
}

int qrtr_send(struct qrtr_state *q, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body, uint32_t length)
{
    return qrtr_send_from(q, QRTR_PORT_CTRL, dst_node, dst_port, type, body, length);
}

int qrtr_send_data(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *body, uint32_t length)
{
    return qrtr_send_from(q, src_port, dst_node, dst_port, QRTR_TYPE_DATA, body, length);
}

int qrtr_send_del_client(struct qrtr_state *q, uint32_t local_port)
{
    uint8_t body[QRTR_CTRL_LEN];
    wr32(body + 0, QRTR_TYPE_DEL_CLIENT);
    wr32(body + 4, q->local_node);
    wr32(body + 8, local_port);
    /* The peer is told over the broadcast node, as Linux's qrtr_port_remove does. */
    return qrtr_send_from(q, QRTR_PORT_CTRL, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_DEL_CLIENT, body, sizeof body);
}

/* Builds the control body a control packet carries.
 *
 * Upstream's name service does not read the header's type field: it reads the command word at the start of
 * the body (Linux net/qrtr/ns.c, the qrtr_ns_worker switch on pkt->cmd) and then the body is a whole
 * qrtr_ctrl_pkt: the command word and a 16-byte union (Linux include/uapi/linux/qrtr.h). The modem follows
 * that convention: its own HELLO carries a 20-byte body whose first word is the command, as the bytes the
 * device sent on 2026-11-20 showed (header type 2, body 20 bytes, body's first word 2).
 *
 * So every control packet this port sends has to carry that body. A control packet without one is
 * invisible to the peer: it has no command word to read, and upstream's own receive path rejects a
 * zero-size packet outright (af_qrtr.c: if (!size || size > len ...) goto err).
 */
static void build_ctrl_body(uint8_t *body, uint32_t cmd, uint32_t node, uint32_t port)
{
    memset(body, 0, QRTR_CTRL_LEN);
    wr32(body + 0, cmd);
    wr32(body + 4, node);
    wr32(body + 8, port);
}

int qrtr_flush_hello_reply(struct qrtr_state *q)
{
    if (!q->hello_reply_owed) return 0;
    const int result = qrtr_send(q, q->hello_reply_node, q->hello_reply_port, QRTR_TYPE_HELLO, q->hello_reply_body,
                                 QRTR_CTRL_LEN);
    if (result == 0) {
        q->hello_reply_owed = 0;
        return 1;
    }
    /* Still owed: the link had no room. */
    q->hello_dropped++;
    return 0;
}

int qrtr_send_from(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, uint32_t type,
                   const void *body, uint32_t length)
{
    if (!q->send) return -1;
    /* A DATA packet may not go out before our own HELLO: the modem's name service ignores it. A reply that arrives
       before our HELLO did is the race Linux fixes by flushing the say_hello work (the fuxi patch 0005-qrtr.patch);
       here the HELLO is sent as soon as the link is up, so only the ordering matters. */
    if (type != QRTR_TYPE_HELLO && !q->hello_sent) {
        q->hello_dropped++;
        return -1;
    }
    /* A control packet carries the command word the peer reads, in the body. A caller that hands over no body
       gets one built here, so a control packet cannot go out without the word the peer switches on. */
    uint8_t built[QRTR_CTRL_LEN];
    if (!body && type != QRTR_TYPE_DATA) {
        build_ctrl_body(built, type, q->local_node, src_port);
        body = built;
        length = QRTR_CTRL_LEN;
    }
    /* The header and the payload travel in one buffer, as Linux does: skb_push() puts the header in front of the
       payload the skb already holds, and skb_put_padto() rounds the payload up to four bytes. */
    const uint32_t padded = (length + 3u) & ~3u;
    if (QRTR_HDR_LEN + padded > sizeof q->scratch) {
        q->dropped++;
        return -1;
    }
    uint8_t *p = q->scratch;
    wr32(p + 0, QRTR_PROTO_VER_1);
    wr32(p + 4, type);
    wr32(p + 8, q->local_node);
    wr32(p + 12, src_port);
    wr32(p + 16, 0);             /* confirm_rx */
    wr32(p + 20, length);
    wr32(p + 24, dst_node);
    wr32(p + 28, dst_port);
    if (length && body) memcpy(p + QRTR_HDR_LEN, body, length);
    for (uint32_t i = length; i < padded; i++) p[QRTR_HDR_LEN + i] = 0;
    const int result = q->send(q->context, src_port, dst_node, dst_port, p, QRTR_HDR_LEN + padded);
    if (result) return result;
    q->tx++;
    if (type == QRTR_TYPE_HELLO) q->hello_sent = 1;
    return 0;
}

void qrtr_receive(struct qrtr_state *q, const void *data, uint32_t length)
{
    const uint8_t *p = data;
    if (!p || length < QRTR_HDR_LEN) {
        q->malformed++;
        return;
    }
    q->rx++;
    /* The first packet is dumped in full: the port has to learn which of the two command conventions the
       peer uses, the one in the header's type field or the one in the body's first word (Linux net/qrtr/ns.c
       reads the body's word), and the bytes say which. The hook is the board's, so it is not there on the
       host; the model test does not need it. */
    if (q->rx <= 2 && q->trace) q->trace(q->trace_context, p, length);
    if (rd32(p + 0) != QRTR_PROTO_VER_1) {
        q->malformed++;
        return;
    }
    const uint32_t type = rd32(p + 4);
    const uint32_t src_node = rd32(p + 8);
    const uint32_t src_port = rd32(p + 12);
    const uint32_t size = rd32(p + 20);
    const uint32_t dst_node = rd32(p + 24);
    const uint32_t dst_port = rd32(p + 28);
    if (size > length - QRTR_HDR_LEN) {
        q->malformed++;
        return;
    }
    const uint8_t *body = p + QRTR_HDR_LEN;

    if (type == QRTR_TYPE_HELLO) {
        q->hello_received = 1;
        q->remote_node = src_node;
        q->remote_node_known = 1;
        /* The modem may announce its services in the same breath as its HELLO; answer and carry on. The answer is
           owed until the link takes it: the remote's receive intent is still held by the very packet that carried
           the HELLO, so the first attempt is usually refused. */
        q->hello_reply_owed = 1;
        q->hello_reply_node = src_node;
        q->hello_reply_port = src_port;
        /* The answer carries the command word the peer reads, and the node and port it looks for: our own node id
           in the union's node field, and the control port. The modem's own HELLO carried the same shape. */
        build_ctrl_body(q->hello_reply_body, QRTR_TYPE_HELLO, q->local_node, QRTR_PORT_CTRL);
        qrtr_flush_hello_reply(q);
        return;
    }
    if (type == QRTR_TYPE_NEW_SERVER) {
        if (size < QRTR_CTRL_LEN) {
            q->malformed++;
            return;
        }
        const uint32_t service = rd32(body + 4);
        const uint32_t instance = rd32(body + 8);
        const uint32_t node = rd32(body + 12);
        const uint32_t port = rd32(body + 16);
        if (node) {
            q->remote_node = node;
            q->remote_node_known = 1;
        }
        if (!server_add(q, service, instance, node, port)) {
            q->dropped++;
            return;
        }
        if (q->server_fn) q->server_fn(q->context, service, instance, node, port);
        return;
    }
    if (type == QRTR_TYPE_DEL_SERVER) {
        if (size < QRTR_CTRL_LEN) {
            q->malformed++;
            return;
        }
        server_remove(q, rd32(body + 4), rd32(body + 8));
        return;
    }
    if (type == QRTR_TYPE_DATA) {
        /* The bridge's own hook takes the payload. The destination port is the local client port the answer belongs
           to; the source node and port say who sent it. */
        if (q->data_fn) q->data_fn(q->context, dst_port, src_node, src_port, body, size);
        else q->dropped++;
        return;
    }
    /* BYE, DEL_CLIENT, PING, RESUME_TX and the lookups need no answer here. */
    (void)dst_node;
}

uint32_t qrtr_lookup(struct qrtr_state *q, uint32_t service)
{
    /* The lowest instance of a service is the one a client takes, as qmicli's QRTR backend does. The whole array is
       walked, not the first `servers` entries: a removed server leaves a hole. */
    uint32_t best = 0xffffffffu, found = 0;
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++) {
        if (!q->server[i].present || q->server[i].service != service) continue;
        if (q->server[i].instance < best) {
            best = q->server[i].instance;
            found = q->server[i].port;
        }
    }
    return found;
}
