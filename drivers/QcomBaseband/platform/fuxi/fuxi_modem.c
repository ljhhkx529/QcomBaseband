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

#include "fuxi_servreg.h"
#include "fuxi_rmtfs.h"

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

/* How many milliseconds pass between the wildcard service lookups. A service the modem registers later is only
   found by asking again, because the name service does not announce what appears later. */
#define kWildcardEveryMs 2000u

/* The milliseconds since the boot, the way the bridge counts them (coachz_baseband.cpp's bb_now_ms). */
static uint32_t fuxi_modem_now_ms(void)
{
    uint64_t ticks, ns;
    clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks, &ns);
    return (uint32_t)(ns / 1000000u);
}

/* What the board must hand the adapter before it can start. */
typedef struct {
    /* The SMEM window, already mapped. */
    volatile uint8_t *smem_base;
    uint32_t smem_size;
    /* Rings the remote when the link has something to say. */
    void (*kick)(void *context);
    void *kick_context;
    /* RMTFS: the board's adapter for the modem's remote file system. The protocol half is here, the
       partition reads and the buffer are the board's, and the two are joined by these callbacks. */
    struct fuxi_rmtfs_ops rmtfs_ops;
    void *rmtfs_context;
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
    uint32_t glink_remote_busy;     /* the ones still held, so the credit left is the difference */
    /* The credit on the link and what it is spent on. */
    uint32_t glink_no_intent, glink_intent_too_small, glink_intent_requested, glink_rx_done;
    /* The service lookups that went out. The modem only publishes the services it is asked about, so a lookup
       that never went out is a service list that never arrives. */
    uint32_t qrtr_lookups;
    /* The lookups the modem answered. A lookup it ignored is not counted, so a count that stays behind the number
       that went out says the modem did not answer it. */
    uint32_t qrtr_lookups_answered;
    /* The QRTR flow control: the DATA packets the peer has not credited yet, and the RESUME_TX packets sent and
       received. A peer that runs out of credit stops answering, and the credit comes back with RESUME_TX. */
    uint32_t qrtr_tx_pending, qrtr_resumes_sent, qrtr_resumes_received;
    /* The service registry's locator: how often it was announced, how many requests it took and how many of
       them were answered. A request count that stays at zero says the modem never asked. */
    uint32_t locator_announces, locator_requests, locator_answered;
    /* RMTFS: the modem's remote file system. The counts say whether the modem asked for its files at all:
       the announcements, the requests that arrived, and the ones that were answered. */
    uint32_t rmtfs_announces, rmtfs_requests, rmtfs_answered;
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
    /* The bridge has been published. It is published once, when the channel comes up. */
    uint8_t published;
    /* The service lookup has been sent. The modem answers it with the services it has. */
    uint8_t lookup_sent;
    /* How many of the named lookups went out. */
    uint32_t wanted_done;
    /* How many lookups the link refused, so they are tried again. */
    uint32_t lookup_again;
    /* How many lookups went out. */
    uint32_t lookup_packets;
    /* When the wildcard lookup was last asked, in milliseconds. The poll runs far more often than that, so the
       re-ask is counted in time and not in polls. */
    uint32_t wildcard_at;
    /* The service registry's locator. It is announced once the link is up and again on every router HELLO, and
       it is served on its own local port. The counts say what the modem asked of it. */
    uint8_t locator_announced;
    uint32_t locator_announces, locator_requests, locator_answered;
    /* The number of the peer's HELLOs the last announcement answered. The announcement is sent again when the
       peer's count has moved past it, so a HELLO is always answered with one. */
    uint32_t locator_hello_at;
    /* RMTFS: the modem's remote file system. It is announced and served the same way as the locator, on its own
       local port. The counts say what the modem asked of it. */
    struct fuxi_rmtfs_state rmtfs;
    uint8_t rmtfs_announced;
    uint32_t rmtfs_announces, rmtfs_requests, rmtfs_answered;
    uint32_t rmtfs_hello_at;
    /* The board's adapter: the partition reads and the buffer. */
    struct fuxi_rmtfs_ops rmtfs_ops;
    void *rmtfs_context;

} fuxi_modem_link;

static fuxi_modem_link fuxi_link;

/* The bridge's publish hook, defined below. The poll calls it once the channel is up. */
void fuxi_modem_publish(void);

/* The locator's receive handler, defined below. */
static void fuxi_servreg_receive(fuxi_modem_link *link, uint32_t src_node, uint32_t src_port, const void *body,
                                  uint32_t length);

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

/* The peer asked which of our services it may have. The answer is a NEW_SERVER for every service of ours that
   matches, and then the empty NEW_SERVER that ends the listing (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup and
   lookup_notify: "Empty notification, to indicate end of listing").
 *
   The match is upstream's: a lookup whose service is zero matches any service, and a lookup whose instance is
   zero matches any instance (Linux net/qrtr/ns.c, server_match). */
static void fuxi_qrtr_lookup(void *context, uint32_t service, uint32_t version_instance, uint32_t node)
{
    fuxi_modem_link *link = context;
    static const struct {
        uint32_t service, version, instance, port;
    } ours[] = {
        {FUXI_LOCATOR_SERVICE, FUXI_LOCATOR_VERSION, FUXI_LOCATOR_INSTANCE, FUXI_LOCATOR_PORT},
        {FUXI_RMTFS_SERVICE, FUXI_RMTFS_VERSION, FUXI_RMTFS_INSTANCE, FUXI_RMTFS_PORT},
    };
    /* The instance word: the version in the low byte, the instance in the high one. */
    const uint32_t wanted_instance = version_instance >> 8;
    const uint32_t wanted_version = version_instance & 0xffu;
    for (unsigned i = 0; i < sizeof ours / sizeof ours[0]; ++i) {
        if (service && service != ours[i].service) continue;
        if (wanted_instance && wanted_instance != ours[i].instance) continue;
        if (wanted_version && wanted_version != ours[i].version) continue;
        qrtr_send_new_server(&link->qrtr, node, ours[i].service, ours[i].version, ours[i].instance, ours[i].port);
        printf("FUXI_MODEM: the lookup was answered: service 0x%02x version %u instance %u, on port %u, to node %u\n",
               ours[i].service, ours[i].version, ours[i].instance, ours[i].port, node);
    }
    /* The empty NEW_SERVER ends the listing: it carries neither a service nor a port. */
    qrtr_send_new_server(&link->qrtr, node, 0, 0, 0, 0);
}

/* The modem's remote file system: answers what the modem asks it. Defined below. */
static void fuxi_rmtfs_receive(fuxi_modem_link *link, uint32_t src_node, uint32_t src_port, const void *body,
                               uint32_t length);

/* The QRTR layer's data hook: one payload for the bridge. The destination port is the bridge's local client port. */
static void fuxi_qrtr_data(void *context, uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body,
                           uint32_t length)
{
    fuxi_modem_link *link = context;
    link->rx_packets++;
    /* The service registry's locator is served on its own port, and not by the bridge: it is the host service
       the modem asks where its own services are, and it is answered straight out of the modem's own table. */
    if (dst_port == FUXI_LOCATOR_PORT) {
        fuxi_servreg_receive(link, src_node, src_port, body, length);
        return;
    }
    /* RMTFS is served on its own port too, and not by the bridge: it is the host service the modem asks
       for its own files, and the bridge's client ports do not carry it. */
    if (dst_port == FUXI_RMTFS_PORT) {
        fuxi_rmtfs_receive(link, src_node, src_port, body, length);
        return;
    }
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

/* The service registry's locator: answers what the modem asks it.
 *
 * The answer goes back to the node and port the request came from, from the locator's own port, because the
 * modem routes the answer back by the port the request named. The request's bytes and the answer's bytes go
 * to the log, so the wire can be read off the capture: what the modem asked for, and what was told. */
static void fuxi_servreg_receive(fuxi_modem_link *link, uint32_t src_node, uint32_t src_port, const void *body,
                                  uint32_t length)
{
    uint8_t answer[256];
    uint32_t answer_length = 0;
    char wanted[64];
    uint32_t found = 0;
    link->locator_requests++;
    const int served = fuxi_servreg_serve((const uint8_t *)body, length, answer, sizeof answer, &answer_length,
                                          wanted, sizeof wanted, &found);
    if (!served) {
        printf("FUXI_MODEM: the locator was sent %u bytes it cannot answer\n", length);
        return;
    }
    printf("FUXI_MODEM: the locator: the modem asks where \"%s\" is, %u domain%s\n", wanted, found,
           found == 1u ? "" : "s");
    printf("FUXI_MODEM: the locator: the request is %u bytes:", length);
    for (uint32_t i = 0; i < length && i < 64u; ++i) printf(" %02x", ((const uint8_t *)body)[i]);
    printf("\n");
    printf("FUXI_MODEM: the locator: the answer is %u bytes:", answer_length);
    for (uint32_t i = 0; i < answer_length && i < 96u; ++i) printf(" %02x", answer[i]);
    printf("\n");
    if (qrtr_send_data(&link->qrtr, FUXI_LOCATOR_PORT, src_node, src_port, answer, answer_length)) {
        snprintf(link->last_error, sizeof link->last_error, "the locator's answer was refused");
        return;
    }
    link->locator_answered++;
}

/* Announces the locator to the peer: a NEW_SERVER naming the service, the version and instance it is
   published under, and the local port it is served on.
 *
   The announcement goes to the node the peer's HELLO named, on the peer's control port. The peer reads the
   command word out of the body and not out of the header's type field, the same way it reads the HELLO's.
 *
   Returns 0 when the announcement went out. */
static int fuxi_announce_locator(fuxi_modem_link *link, uint32_t node)
{
    if (qrtr_send_new_server(&link->qrtr, node, FUXI_LOCATOR_SERVICE, FUXI_LOCATOR_VERSION, FUXI_LOCATOR_INSTANCE,
                             FUXI_LOCATOR_PORT)) {
        snprintf(link->last_error, sizeof link->last_error, "the locator's announcement was refused");
        return -1;
    }
    link->locator_announces++;
    /* The peer's HELLO count this announcement answers. The next HELLO moves the count past it, and that is
       what makes the announcement go out again. */
    link->locator_hello_at = link->qrtr.hellos;
    printf("FUXI_MODEM: the locator was announced: service 0x%02x version %u instance %u, on port %u, to node %u\n",
           FUXI_LOCATOR_SERVICE, FUXI_LOCATOR_VERSION, FUXI_LOCATOR_INSTANCE, FUXI_LOCATOR_PORT, node);
    return 0;
}

/* The modem's remote file system: answers what the modem asks it.
 *
 * The answer goes back to the node and port the request came from, from RMTFS's own port, because the
 * modem routes the answer back by the port the request named. Every request's message id goes to the
 * log on one line, and the raw bytes of the first OPEN go to the log with it, so the wire can be read
 * off the capture: what the modem asked for, and what was told. */
static void fuxi_rmtfs_receive(fuxi_modem_link *link, uint32_t src_node, uint32_t src_port, const void *body,
                                uint32_t length)
{
    uint8_t answer[256];
    uint32_t answer_length = 0;
    uint32_t message = 0;
    link->rmtfs_requests++;
    const int served = fuxi_rmtfs_serve(&link->rmtfs, (const uint8_t *)body, length, answer, sizeof answer,
                                        &answer_length, &link->rmtfs_ops, link->rmtfs_context, &message);
    if (!served) {
        printf("FUXI_MODEM: the rmtfs was sent %u bytes it cannot answer\n", length);
        return;
    }
    /* The message id and its name on one line. The names are the reference's
       (hakios.mpss.c, RMTFS_OPEN and the four after it). */
    const char *name = "unknown";
    switch (message) {
    case FUXI_RMTFS_OPEN: name = "OPEN"; break;
    case FUXI_RMTFS_CLOSE: name = "CLOSE"; break;
    case FUXI_RMTFS_RW_IOVEC: name = "RW_IOVEC"; break;
    case FUXI_RMTFS_ALLOC_BUFF: name = "ALLOC_BUFF"; break;
    case FUXI_RMTFS_GET_DEV_ERROR: name = "GET_DEV_ERROR"; break;
    }
    printf("FUXI_MODEM: the rmtfs: the modem asks %s (message %u), %u bytes\n", name, message, length);
    /* The first OPEN's raw bytes: the path the modem asks for is in them, and the path is the whole
       question this round is about. */
    if (message == FUXI_RMTFS_OPEN && link->rmtfs_requests == 1u) {
        printf("FUXI_MODEM: the rmtfs: the first OPEN is %u bytes:", length);
        for (uint32_t i = 0; i < length && i < 96u; ++i) printf(" %02x", ((const uint8_t *)body)[i]);
        printf("\n");
    }
    if (qrtr_send_data(&link->qrtr, FUXI_RMTFS_PORT, src_node, src_port, answer, answer_length)) {
        snprintf(link->last_error, sizeof link->last_error, "the rmtfs's answer was refused");
        return;
    }
    link->rmtfs_answered++;
}

/* Announces RMTFS to the peer: a NEW_SERVER naming the service, the version and instance it is published
   under, and the local port it is served on. The instance word is 1: version 1, instance 0.
 *
   Returns 0 when the announcement went out. */
static int fuxi_announce_rmtfs(fuxi_modem_link *link, uint32_t node)
{
    if (qrtr_send_new_server(&link->qrtr, node, FUXI_RMTFS_SERVICE, FUXI_RMTFS_VERSION, FUXI_RMTFS_INSTANCE,
                             FUXI_RMTFS_PORT)) {
        snprintf(link->last_error, sizeof link->last_error, "the rmtfs's announcement was refused");
        return -1;
    }
    link->rmtfs_announces++;
    /* The peer's HELLO count this announcement answers. The next HELLO moves the count past it, and that is
       what makes the announcement go out again. */
    link->rmtfs_hello_at = link->qrtr.hellos;
    printf("FUXI_MODEM: the rmtfs was announced: service 0x%02x version %u instance %u, on port %u, to node %u\n",
           FUXI_RMTFS_SERVICE, FUXI_RMTFS_VERSION, FUXI_RMTFS_INSTANCE, FUXI_RMTFS_PORT, node);
    return 0;
}

/* Dumps one received QRTR packet, header and body, so the wire can be read off the log.
 *
 * The whole packet is printed, not the first 64 bytes: an RMTFS request carries its entries at the
 * end, and a dump that stops before them does not show what the modem asked for. */
static void fuxi_trace_qrtr(void *context, const uint8_t *bytes, uint32_t count)
{
    (void)context;
    printf("FUXI_MODEM: the qrtr packet is %u bytes:", count);
    for (uint32_t i = 0; i < count; ++i) printf(" %02x", bytes[i]);
    printf("\n");
    if (count < QRTR_HDR_LEN) return;
    const uint32_t head[8] = {
        (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24,
        (uint32_t)bytes[4] | (uint32_t)bytes[5] << 8 | (uint32_t)bytes[6] << 16 | (uint32_t)bytes[7] << 24,
        (uint32_t)bytes[8] | (uint32_t)bytes[9] << 8 | (uint32_t)bytes[10] << 16 | (uint32_t)bytes[11] << 24,
        (uint32_t)bytes[12] | (uint32_t)bytes[13] << 8 | (uint32_t)bytes[14] << 16 | (uint32_t)bytes[15] << 24,
        (uint32_t)bytes[16] | (uint32_t)bytes[17] << 8 | (uint32_t)bytes[18] << 16 | (uint32_t)bytes[19] << 24,
        (uint32_t)bytes[20] | (uint32_t)bytes[21] << 8 | (uint32_t)bytes[22] << 16 | (uint32_t)bytes[23] << 24,
        (uint32_t)bytes[24] | (uint32_t)bytes[25] << 8 | (uint32_t)bytes[26] << 16 | (uint32_t)bytes[27] << 24,
        (uint32_t)bytes[28] | (uint32_t)bytes[29] << 8 | (uint32_t)bytes[30] << 16 | (uint32_t)bytes[31] << 24,
    };
    printf("FUXI_MODEM: the qrtr header is version %u type %u src node %u src port %u, confirm %u, size %u, "
           "dst node %u dst port %u\n",
           head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
    if (count > QRTR_HDR_LEN) {
        const uint32_t body0 = (uint32_t)bytes[QRTR_HDR_LEN] | (uint32_t)bytes[QRTR_HDR_LEN + 1] << 8 |
            (uint32_t)bytes[QRTR_HDR_LEN + 2] << 16 | (uint32_t)bytes[QRTR_HDR_LEN + 3] << 24;
        printf("FUXI_MODEM: the qrtr body is %u bytes, its first word is %u\n", count - QRTR_HDR_LEN, body0);
    } else {
        printf("FUXI_MODEM: the qrtr packet has no body\n");
    }
}

/* The GLINK channel's state hook. */
static void fuxi_glink_state(void *context, QcomGlinkChannel *channel, int up)
{
    (void)context; (void)channel;
    if (up) fuxi_link.ready = 1;
}

/* The bytes that go out on the wire. The receive-side trace shows what the modem sent; this one shows what we sent,
   which is the only way to tell a lookup the modem ignored from a lookup that never left. */
static void fuxi_trace_qrtr_tx(void *context, const uint8_t *bytes, uint32_t count)
{
    (void)context;
    printf("FUXI_MODEM: the qrtr packet we sent is %u bytes:", count);
    for (uint32_t i = 0; i < count && i < 64; ++i) printf(" %02x", bytes[i]);
    printf("\n");
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
    /* The board's adapter for RMTFS: the partition reads and the buffer. */
    link->rmtfs_ops = config->rmtfs_ops;
    link->rmtfs_context = config->rmtfs_context;
    if (qcom_smem_partition(&link->smem, QCOM_SMEM_HOST_APPS, FUXI_MODEM_PID, &link->partition)) {
        snprintf(link->last_error, sizeof link->last_error, "no SMEM partition with the modem (pid %u)", FUXI_MODEM_PID);
        return -1;
    }
    if (fuxi_glink_start(link)) return -1;

    qrtr_init(&link->qrtr, FUXI_QRTR_LOCAL_NODE, fuxi_qrtr_send, fuxi_qrtr_new_server, link);
    qrtr_set_data_handler(&link->qrtr, fuxi_qrtr_data);
    qrtr_set_lookup_handler(&link->qrtr, fuxi_qrtr_lookup);
    qrtr_set_trace(&link->qrtr, fuxi_trace_qrtr, 0);
    qrtr_set_trace_tx(&link->qrtr, fuxi_trace_qrtr_tx, 0);
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
    /* The service registry's locator is announced to the modem, once the link is up and again on every
       router HELLO.
     *
     * The modem waits for two AP-side services before it brings its stack up, and the locator is one of them.
     * It learns of the service only from the announcement, so an announcement that is not repeated on the
     * peer's next HELLO leaves the peer without it: the peer sends HELLO again whenever it (re)opens its
     * link, and answers it with the services it wants. */
    if (link->ready && link->qrtr.remote_node_known) {
        if (!link->locator_announced) {
            /* A refused announcement is tried again on the next poll: the link had no room, and the flag is
               only set once the announcement has actually gone out. */
            if (fuxi_announce_locator(link, link->qrtr.remote_node) == 0) link->locator_announced = 1;
        } else if (link->qrtr.hellos > link->locator_hello_at) {
            fuxi_announce_locator(link, link->qrtr.remote_node);
        }
    }
    /* RMTFS is announced the same way, and for the same reason: the modem learns of it only from the
       announcement, and an announcement that is not repeated on the peer's next HELLO leaves the peer
       without it. */
    if (link->ready && link->qrtr.remote_node_known) {
        if (!link->rmtfs_announced) {
            if (fuxi_announce_rmtfs(link, link->qrtr.remote_node) == 0) link->rmtfs_announced = 1;
        } else if (link->qrtr.hellos > link->rmtfs_hello_at) {
            fuxi_announce_rmtfs(link, link->qrtr.remote_node);
        }
    }
    /* The bridge is published once the channel is up. It has to be published after, not before: the bridge's clients
       are bound to the services the modem announces, and it needs this side's send hook to reach them. Until the
       channel is up there is no route to the modem at all, so a bridge published earlier could only fail. */
    if (link->ready && !link->published) {
        link->published = 1;
        fuxi_modem_publish();
    }
    /* The services the bridge wants, asked for by name. The modem does not publish its whole list on its own: it
       answers a lookup with the services that match (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup and lookup_notify),
       and a client that never asks never learns. The lookup is asked for once the modem's node is known, because the
       answer is addressed to the node the lookup came from. */
    if (link->ready && link->qrtr.remote_node_known && !link->lookup_sent) {
        link->lookup_sent = 1;
        /* Service 0 and instance 0 match anything, so this one lookup asks for the whole list
           (Linux ns.c: `if (lookup->service && lookup->service != service) continue;`). */
        if (qrtr_send_lookup(&link->qrtr, 0, 0, 0)) {
            snprintf(link->last_error, sizeof link->last_error, "the service lookup was refused");
        } else {
            link->lookup_packets++;
        }
    }
    /* The wildcard lookup is asked again and again, on both of the nodes the peer named.
     *
     * Two facts from the wire make this necessary. The modem's answer to the wildcard lookup was two services
     * and then nothing: no empty NEW_SERVER, which upstream uses to end a listing. So the listing was cut short,
     * and a service that the modem registers a moment later is not in it. And the modem's own name service runs on
     * the node its HELLO body names, not the node its header names, so a lookup that goes only to the header's
     * node does not reach the whole list.
     *
     * The re-ask is the only way to learn a service that appears later: a service the bridge has not seen cannot
     * be asked for by name, and the name service does not announce what appears later. It is asked every
     * kWildcardEvery polls, which is a few seconds, so a service that shows up is bound within a few seconds of
     * its appearing. */
    if (link->ready && link->qrtr.remote_node_known && link->lookup_sent) {
        const uint32_t now = fuxi_modem_now_ms();
        if (!link->wildcard_at) link->wildcard_at = now;
        if (now - link->wildcard_at >= kWildcardEveryMs) {
            link->wildcard_at = now;
            const uint32_t nodes[2] = { link->qrtr.remote_node, link->qrtr.peer_node };
            for (unsigned k = 0; k < 2; ++k) {
                if (k == 1 && (!link->qrtr.peer_node_known || link->qrtr.peer_node == link->qrtr.remote_node))
                    continue;
                if (qrtr_send_lookup_to(&link->qrtr, nodes[k], 0, 0, 0) == 0) link->lookup_packets++;
                else link->lookup_again++;
            }
        }
    }
    /* The wildcard lookup only reaches what the modem publishes on its own initiative. The service the bridge
       needs is asked for by name: the lookup's service id and the version in the low byte of the instance word
       (Linux drivers/soc/qcom/qmi_interface.c, qmi_send_new_lookup). The names are swept one after another, and
       each is asked on both of the nodes the peer named: the one in its HELLO header and the one in its HELLO
       body, which are not the same. A service the peer holds on the body's node is not reachable from the
       header's node, so a lookup that goes only to the header's node finds nothing.

       The instance word is the other half of the question. Linux's name service matches it loosely
       (Linux net/qrtr/ns.c, server_match: a lookup with instance 0 matches any instance), but the modem's
       answer to a wildcard lookup shows its own instances, and a named lookup with instance 0 was answered
       with silence. So each name is asked twice: once with the instance the peer announced and once with
       instance 0. The two controls, 0x42 and 0x2b, are asked with the instance the peer announced, because
       they are the ones whose instances are known. */
    static const struct {
        uint32_t service, version, instance;
    } fuxi_wanted[] = {
        {0x40u, 0u, 0u},    /* SERVREG_LOC: the locator, the one service that lists the modem's services */
        {0x40u, 1u, 0u},
        {0x40u, 1u, 1u},
        {0x42u, 1u, 180u},  /* SERVREG_NOTIF, the instance the peer announced */
        {0x2bu, 2u, 18u},   /* SSCTL, the instance the peer announced */
        {0x42u, 0u, 0u},    /* the same name with instance 0 */
        {0x2bu, 0u, 0u},
        {0x0bu, 0u, 0u},    /* UIM: the card and its files */
        {0x0bu, 1u, 0u},
        {0x0bu, 1u, 2u},
        {0x03u, 0u, 0u},    /* NAS: the network and the signal */
        {0x03u, 1u, 0u},
        {0x01u, 0u, 0u},    /* WDS: the data service */
        {0x02u, 0u, 0u},    /* DMS */
        {0xe2u, 0u, 0u},    /* BSP */
    };
    if (link->ready && link->qrtr.remote_node_known && link->lookup_sent &&
        link->wanted_done < sizeof fuxi_wanted / sizeof fuxi_wanted[0]) {
        const uint32_t service = fuxi_wanted[link->wanted_done].service;
        const uint32_t version = fuxi_wanted[link->wanted_done].version;
        const uint32_t instance = fuxi_wanted[link->wanted_done].instance;
        /* Each lookup is asked on both nodes, one per poll, so the link has room and the log says which
           node was asked. */
        uint32_t node = link->qrtr.remote_node;
        if (link->qrtr.peer_node_known && link->qrtr.peer_node != link->qrtr.remote_node &&
            (link->wanted_done & 1u))
            node = link->qrtr.peer_node;
        if (qrtr_send_lookup_to(&link->qrtr, node, service, version, instance) == 0) {
            link->lookup_packets++;
            printf("FUXI_MODEM: the lookup for service 0x%02x version %u instance %u went out to node %u\n",
                   service, version, instance, node);
            link->wanted_done++;
        } else {
            /* The link had no room. The same lookup is tried again on the next poll. */
            link->lookup_again++;
        }
    }
    extern void coachz_baseband_poll(void);
    coachz_baseband_poll();
}

/* The bridge's send hook: one QMI message for a client. The local port is the client's own port, because the modem
   routes the answer back by it; the node and port are the service's, from the modem's NEW_SERVER.

   A refusal is reported back, so the bridge tries the message again. The QRTR layer refuses a DATA packet when
   the peer is out of credit, and the credit comes back with a RESUME_TX; a message dropped here would be lost
   with nothing on the log to say so. */
static int fuxi_bridge_send(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length)
{
    fuxi_modem_link *link = &fuxi_link;
    if (!link->channel) return -1;
    if (qrtr_send_data(&link->qrtr, local_port, node, port, message, length)) {
        snprintf(link->last_error, sizeof link->last_error, "QRTR data to %u:%u refused", node, port);
        return -1;
    }
    return 0;
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
    extern void coachz_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t),
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
    status->glink_remote_busy = 0;
    status->glink_no_intent = link->glink.no_intent;
    status->glink_intent_too_small = link->glink.intent_too_small;
    status->glink_intent_requested = link->glink.intent_requested;
    status->glink_rx_done = link->glink.rx_done;
    status->qrtr_lookups = link->lookup_packets;
    status->qrtr_lookups_answered = link->qrtr.lookups_answered;
    /* The flow control. A peer that is out of credit stops answering, so the count of outstanding packets and the
       number of RESUME_TX packets sent and received say whether the link is waiting for a credit. */
    status->qrtr_tx_pending = link->qrtr.flow[0].pending;
    status->qrtr_resumes_sent = link->qrtr.resumes_sent;
    status->qrtr_resumes_received = link->qrtr.resumes_received;
    status->locator_announces = link->locator_announces;
    status->locator_requests = link->locator_requests;
    status->locator_answered = link->locator_answered;
    status->rmtfs_announces = link->rmtfs_announces;
    status->rmtfs_requests = link->rmtfs_requests;
    status->rmtfs_answered = link->rmtfs_answered;
    if (link->channel) {
        for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k) {
            if (!link->channel->remote[k].valid) continue;
            status->glink_remote_intents++;
            /* An intent the remote has not told us it is done with is still held: it cannot be sent into again.
               The count of held intents is the credit the remote has left, which is what says whether the link is
               out of room or the modem has stopped answering. */
            if (link->channel->remote[k].busy) status->glink_remote_busy++;
        }
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
    /* The locator's own counts: how often it was announced, and what the modem asked of it. */
    printf("FUXI modem: %u s: the locator was announced %u times, took %u requests and answered %u\n", second,
           link->locator_announces, link->locator_requests, link->locator_answered);
    /* RMTFS's own counts: a request count that stays at zero says the modem never asked for its files. */
    printf("FUXI modem: %u s: the rmtfs was announced %u times, took %u requests and answered %u\n", second,
           link->rmtfs_announces, link->rmtfs_requests, link->rmtfs_answered);
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
