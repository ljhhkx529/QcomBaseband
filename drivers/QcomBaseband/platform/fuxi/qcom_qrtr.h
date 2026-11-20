/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * QRTR over the fuxi's GLINK-SMEM link: the wire protocol, the node and port bookkeeping and the name service.
 *
 * The fuxi's modem is a Qualcomm MPSS inside the SoC, not an Apple PCIe baseband. It is reached over GLINK (two SMEM
 * channels: IPCRTR for the QRTR traffic and DS for the data services), and the QRTR protocol runs over IPCRTR.
 *
 * Wire format (Linux net/qrtr/af_qrtr.c, qrtr_hdr_v1): a 32-byte little-endian header of
 *
 *   version(4) type(4) src_node(4) src_port(4) confirm_rx(4) size(4) dst_node(4) dst_port(4)
 *
 * followed by `size` bytes of payload, padded to a 4-byte boundary. The version is 1. The type is one of QRTR_TYPE_*.
 * The control port is 0xfffffffe; the broadcast node is 0xffffffff.
 *
 * The name service: the modem announces its services with NEW_SERVER (service, instance, node, port) and takes them
 * away with DEL_SERVER. A client opens a port on the modem's node and sends a DATA packet; the modem answers on the
 * same port.
 *
 * The node and port rules, from Linux net/qrtr/af_qrtr.c and ns.c and from the working Trogdor reference port:
 *
 *   - the AP's own node id is 1 (Linux qrtr_local_nid, Trogdor's WQ_NODE). It is what the modem addresses us by.
 *   - a control packet (HELLO, NEW_SERVER, DEL_SERVER, DEL_CLIENT) always carries the control port 0xfffffffe as its
 *     source port, and is 20 bytes: the command and four words.
 *   - the first HELLO goes to the broadcast node 0xffffffff, because the modem's node id is not known yet; the reply
 *     HELLO is addressed to the node id the modem's own HELLO carried.
 *   - a DATA packet carries the local port the client bound (the modem routes its answer back by it) as its source
 *     port, and the service's node and port as its destination.
 *   - a DEL_CLIENT tells the peer that a local port is gone; it goes to the broadcast node.
 *
 * The HELLO race: Linux flushes the delayed say_hello work when a reply arrives before our own HELLO went out
 * (net/qrtr/af_qrtr.c, the qcom fuxi patch 0005-qrtr.patch). Without that, every reply to the first packet is dropped
 * and no service is ever seen. This module sends HELLO as soon as the link is up and accepts replies at any time.
 */
#ifndef QCOM_QRTR_H
#define QCOM_QRTR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* qrtr_hdr_v1 */
#define QRTR_PROTO_VER_1 1u
#define QRTR_HDR_LEN 32u

#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_PORT_CTRL 0xfffffffeu

enum {
    QRTR_TYPE_DATA = 1,
    QRTR_TYPE_HELLO = 2,
    QRTR_TYPE_BYE = 3,
    QRTR_TYPE_NEW_SERVER = 4,
    QRTR_TYPE_DEL_SERVER = 5,
    QRTR_TYPE_DEL_CLIENT = 6,
    QRTR_TYPE_RESUME_TX = 7,
    QRTR_TYPE_EXIT = 8,
    QRTR_TYPE_PING = 9,
    QRTR_TYPE_NEW_LOOKUP = 10,
    QRTR_TYPE_DEL_LOOKUP = 11,
};

/* struct qrtr_ctrl_pkt: cmd then the union of server and client */
#define QRTR_CTRL_LEN 20u

/* The node id the AP claims. Linux qrtr_local_nid is 1 and the Trogdor reference port uses the same; the modem
   addresses the AP by it. */
#define QRTR_LOCAL_NODE 1u

#define QRTR_MAX_SERVERS 64u
/* The largest packet the layer assembles: the header plus a QMI SDU. */
#define QRTR_MAX_PACKET 8192u

/* One service the modem announced. */
struct qrtr_server {
    uint32_t service, instance, node, port;
    uint8_t present;
};

/* The link the protocol layer writes through. Returns 0 when the bytes are on their way. The source port says which
   local port the packet comes from: the control port for a control packet, the client's own port for a DATA packet,
   because the modem routes its answer back by it. */
typedef int (*qrtr_send_fn)(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data,
                            uint32_t length);
typedef void (*qrtr_server_fn)(void *context, uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
/* Hands a DATA packet's payload to the bridge. The destination port is the local client port the answer belongs to,
   the source node and port say who sent it. Implemented by the board adapter. */
typedef void (*qrtr_data_fn)(void *context, uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body,
                             uint32_t length);

/* The transport's state. The board fills in the link hooks; the protocol layer keeps the rest. */
struct qrtr_state {
    uint32_t local_node;              /* the node id the AP claims; the modem's is learned from its HELLO */
    uint32_t remote_node;             /* the modem's node id, from its HELLO or its NEW_SERVER */
    uint8_t remote_node_known, hello_sent, hello_received;
    /* The reply to the peer's HELLO, owed when the link could not take it the first time. The link is busy more
       often than not right after the handshake: the remote's intent is still held by the packet that carried the
       HELLO. Without this the answer is lost and the peer never learns our node id. */
    uint8_t hello_reply_owed;
    uint32_t hello_reply_node, hello_reply_port;
    /* The reply's control body: the command word the peer reads and the node and port it looks for. */
    uint8_t hello_reply_body[QRTR_CTRL_LEN];
    struct qrtr_server server[QRTR_MAX_SERVERS];
    uint32_t servers;
    /* The link hooks. */
    qrtr_send_fn send;
    qrtr_server_fn server_fn;
    qrtr_data_fn data_fn;
    void *context;
    /* An optional hook that sees the control packets as they arrive, so the wire can be read off the log. */
    void (*trace)(void *context, const uint8_t *bytes, uint32_t length);
    void *trace_context;
    /* How many packets the trace hook has seen. The dump is limited, so a busy link cannot flood the log. */
    uint32_t traced;
    /* Counters for the log. */
    uint32_t tx, rx, dropped, malformed, hello_dropped;
    char last_error[96];
    /* The scratch the header and its payload are assembled in, so they go out as one buffer. */
    uint8_t scratch[QRTR_MAX_PACKET];
};

/* Resets the protocol state and sends HELLO. */
void qrtr_init(struct qrtr_state *q, uint32_t local_node, qrtr_send_fn send, qrtr_server_fn server,
               void *context);
/* Sets the handler for DATA packets. */
void qrtr_set_data_handler(struct qrtr_state *q, qrtr_data_fn data);
/* Sets the hook that sees the first packets as they arrive. */
void qrtr_set_trace(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                    void *context);
/* Sends one QRTR packet: the 32-byte header and the payload. The source port is the control port. */
int qrtr_send(struct qrtr_state *q, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body, uint32_t length);
/* Sends one QRTR packet from a given source port. */
int qrtr_send_from(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body,
                   uint32_t length);
/* Sends a DATA packet to a node and port, from the local port the client bound. */
int qrtr_send_data(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *body, uint32_t length);
/* Tells the peer that a local client port is gone. */
int qrtr_send_del_client(struct qrtr_state *q, uint32_t local_port);
/* Sends the reply to the peer's HELLO if it is still owed. The owner calls this whenever the link may have room
   again. Returns 1 when the reply went out. */
int qrtr_flush_hello_reply(struct qrtr_state *q);
/* Hands one received QRTR packet (header and payload) to the protocol layer. */
void qrtr_receive(struct qrtr_state *q, const void *data, uint32_t length);
/* The service the modem announced with the lowest instance, or 0. */
uint32_t qrtr_lookup(struct qrtr_state *q, uint32_t service);

#ifdef __cplusplus
}
#endif
#endif
