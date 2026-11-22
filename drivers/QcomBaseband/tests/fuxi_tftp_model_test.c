/* Host model of the TFTP service: the modem's carrier configuration, fetched over QRTR.
   Built with -fsanitize=address,undefined by scripts/check.py.

   The protocol is TFTP's over a QRTR data packet:
     byte 0     always zero
     byte 1     the opcode (RRQ 1, WRQ 2, DATA 3, ACK 4, ERROR 5, OACK 6)
     bytes 2-3  the block number, big-endian
     then       the path, NUL-terminated, the mode string, NUL-terminated, and the options as pairs of
                NUL-terminated name and value

   What is worth a test here is the reader and the writer. A request read from the wrong place would be
   answered for the wrong file, and an answer built with the wrong shape would be a transfer that never
   starts — and the whole point of this service is that the modem asks it for the carrier configuration
   it is missing. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include "fuxi_tftp.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The adapter the model serves with: a file of a known size and known bytes. */
#define FILE_SIZE 3000u
static uint8_t file_bytes[FILE_SIZE];
static char opened_path[256];
static int opened;
static int closed;
static int reads;
static uint8_t sent[64][FUXI_TFTP_PACKET_BLKSIZE + 16u];
static uint32_t sent_length[64];
static uint32_t sent_port[64];
static unsigned sent_count;

static int mock_open(void *context, const char *path, uint32_t path_length, void **handle)
{
    (void)context;
    if (path_length >= sizeof opened_path) return -1;
    memcpy(opened_path, path, path_length);
    opened_path[path_length] = 0;
    opened++;
    *handle = (void *)(uintptr_t)1u;
    return 0;
}

static int mock_read(void *context, void *handle, uint64_t offset, void *to, uint32_t length)
{
    (void)context; (void)handle;
    reads++;
    if (offset + length > FILE_SIZE) return -1;
    memcpy(to, file_bytes + offset, length);
    return 0;
}

static uint64_t mock_size(void *context, void *handle)
{
    (void)context; (void)handle;
    return FILE_SIZE;
}

static void mock_close(void *context, void *handle)
{
    (void)context; (void)handle;
    closed++;
}

static int mock_send(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const uint8_t *body,
                     uint32_t length)
{
    (void)context; (void)dst_node; (void)dst_port;
    if (sent_count >= 64u || length > sizeof sent[0]) return -1;
    memcpy(sent[sent_count], body, length);
    sent_length[sent_count] = length;
    sent_port[sent_count] = src_port;
    sent_count++;
    return 0;
}

static const struct fuxi_tftp_ops ops = {mock_open, mock_read, mock_size, mock_close, mock_send};

/* Builds a TFTP request: the header, the path, the mode, and the options.
   The options are given as pairs of names and values, and a null name ends them. */
static uint32_t build_request(uint8_t *out, uint32_t cap, uint8_t opcode, const char *path,
                              const char *const names[], const char *const values[], unsigned count)
{
    uint32_t at = 0;
    if (cap < 8u) return 0;
    out[at++] = 0;
    out[at++] = opcode;
    out[at++] = 0;
    out[at++] = 0;
    const size_t path_length = strlen(path);
    if (at + path_length + 1u > cap) return 0;
    memcpy(out + at, path, path_length);
    at += (uint32_t)path_length;
    out[at++] = 0;
    memcpy(out + at, "octet", 5);
    at += 5;
    out[at++] = 0;
    for (unsigned i = 0; i < count; ++i) {
        const size_t name_length = strlen(names[i]);
        const size_t value_length = strlen(values[i]);
        if (at + name_length + value_length + 2u > cap) return 0;
        memcpy(out + at, names[i], name_length);
        at += (uint32_t)name_length;
        out[at++] = 0;
        memcpy(out + at, values[i], value_length);
        at += (uint32_t)value_length;
        out[at++] = 0;
    }
    return at;
}

/* The bytes of an answer, as a string, so the test can look at them. */
static void answer_text(unsigned index, char *out, size_t cap)
{
    uint32_t at = 0;
    while (at < sent_length[index] && at + 1u < cap) {
        const uint8_t c = sent[index][at];
        out[at] = (c >= 32u && c < 127u) ? (char)c : '.';
        at++;
    }
    out[at] = 0;
}

int main(void)
{
    for (unsigned i = 0; i < FILE_SIZE; ++i) file_bytes[i] = (uint8_t)(i & 0xffu);

    /* The request: a read of the modem's own path. */
    {
        struct fuxi_tftp_request r;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/modem_pr/mcfg.mbn", 0, 0, 0);
        CHECK(n > 0);
        CHECK(fuxi_tftp_parse(req, n, &r) == 1);
        CHECK(r.opcode == FUXI_TFTP_RRQ);
        CHECK(r.wide == 0);
        CHECK(r.options == 0);
        CHECK(strcmp(r.path, "/readonly/firmware/image/modem_pr/mcfg.mbn") == 0);
        CHECK(r.blksize == FUXI_TFTP_DEFAULT_BLKSIZE);
        CHECK(r.wsize == 1);
        CHECK(r.seek == 0);
    }

    /* A request with options: the ones the protocol knows are read, and an unknown one is ignored. */
    {
        struct fuxi_tftp_request r;
        uint8_t req[256];
        static const char *const names[] = { "blksize", "timeoutms", "tsize", "wsize", "rsize", "seek", "nosuch" };
        static const char *const values[] = { "4096", "2000", "0", "4", "200", "100", "1" };
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", names,
                                         values, 7u);
        CHECK(n > 0);
        CHECK(fuxi_tftp_parse(req, n, &r) == 1);
        CHECK(r.options == 1);
        /* The block size is capped at what fits in one QRTR packet, and the answer agrees to the capped
           value, not to the one that was asked for. */
        CHECK(r.blksize == FUXI_TFTP_PACKET_BLKSIZE);
        CHECK(r.timeoutms == 2000);
        CHECK(r.have_tsize == 1);
        CHECK(r.wsize == 4);
        CHECK(r.rsize == 200);
        CHECK(r.seek == 100);
    }

    /* The other shape: the opcode in byte 0 and byte 1 zero. The two references disagree about where the
       opcode sits, so both are read, and which one it was is said. */
    {
        struct fuxi_tftp_request r;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", 0, 0, 0);
        req[0] = FUXI_TFTP_RRQ;
        req[1] = 0;
        CHECK(fuxi_tftp_parse(req, n, &r) == 1);
        CHECK(r.opcode == FUXI_TFTP_RRQ);
        CHECK(r.wide == 1);
    }

    /* A request that is not one: no path, a mode that is not octet, a string that runs off the end. */
    {
        struct fuxi_tftp_request r;
        uint8_t req[256];
        CHECK(fuxi_tftp_parse(req, 3u, &r) == 0);
        CHECK(fuxi_tftp_parse(req, 0u, &r) == 0);
        uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", 0, 0, 0);
        req[n - 1] = 'X';   /* the mode is no longer "octet" */
        CHECK(fuxi_tftp_parse(req, n, &r) == 0);
        n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", 0, 0, 0);
        req[n - 1] = 0;     /* the mode string is not terminated inside the packet */
        CHECK(fuxi_tftp_parse(req, n - 1u, &r) == 0);
    }

    /* The service: a read is answered with the file's first block, and the file is opened by its own
       path. */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        opened = 0; closed = 0; reads = 0; sent_count = 0;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/modem_pr/mcfg.mbn", 0, 0, 0);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &ops, 0, &opcode, &wide) == 1);
        CHECK(opcode == FUXI_TFTP_RRQ);
        CHECK(opened == 1);
        CHECK(strcmp(opened_path, "/readonly/firmware/image/modem_pr/mcfg.mbn") == 0);
        CHECK(state.requests == 1);
        CHECK(state.reads == 1);
        /* No options were asked for, so the first block goes straight out. */
        CHECK(sent_count == 1);
        CHECK(sent[0][1] == FUXI_TFTP_DATA);
        CHECK(sent[0][2] == 0 && sent[0][3] == 1);
        CHECK(sent_length[0] == 4u + FUXI_TFTP_DEFAULT_BLKSIZE);
        /* And the block is the file's own bytes. */
        CHECK(memcmp(sent[0] + 4, file_bytes, FUXI_TFTP_DEFAULT_BLKSIZE) == 0);
    }

    /* A read with options: the answer agrees to the options, and the file's size is in it. */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        opened = 0; sent_count = 0;
        uint8_t req[256];
        static const char *const names[] = { "blksize", "tsize", "wsize" };
        static const char *const values[] = { "4096", "0", "4" };
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", names,
                                         values, 3u);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &ops, 0, &opcode, &wide) == 1);
        CHECK(sent_count == 1);
        CHECK(sent[0][1] == FUXI_TFTP_OACK);
        char text[256];
        answer_text(0, text, sizeof text);
        CHECK(strstr(text, "blksize") != 0);
        CHECK(strstr(text, "4000") != 0);
        CHECK(strstr(text, "tsize") != 0);
        CHECK(strstr(text, "3000") != 0);   /* the file's own size */
        CHECK(strstr(text, "wsize") != 0);
        CHECK(strstr(text, "4") != 0);
    }

    /* A write is acknowledged and discarded: nothing is stored, and the file is never opened. */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        opened = 0; sent_count = 0;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_WRQ, "/readwrite/modem_pr/mcfg.mbn", 0, 0, 0);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &ops, 0, &opcode, &wide) == 1);
        CHECK(opcode == FUXI_TFTP_WRQ);
        CHECK(state.writes == 1);
        CHECK(opened == 0);
        CHECK(sent_count == 1);
        CHECK(sent[0][1] == FUXI_TFTP_ACK);
    }

    /* A request for a file that is not there is answered with an error, not with a success. */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        sent_count = 0;
        /* The adapter refuses every open. */
        struct fuxi_tftp_ops refusing = ops;
        refusing.open = 0;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", 0, 0, 0);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &refusing, 0, &opcode, &wide) == 1);
        CHECK(state.missing == 1);
        CHECK(sent_count == 1);
        CHECK(sent[0][1] == FUXI_TFTP_ERROR);
    }

    /* The transfer: an acknowledgement opens the window, and the file's last block is the short one. */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        opened = 0; sent_count = 0; reads = 0;
        uint8_t req[256];
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", 0, 0, 0);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &ops, 0, &opcode, &wide) == 1);
        /* The first block went out on the service's port; the transfer's own port carries the rest. */
        CHECK(sent_port[0] == FUXI_TFTP_TRANSFER_PORT_BASE);

        /* The file is 3000 bytes and the block is 512, so there are six blocks: 3000 / 512 + 1. */
        CHECK(state.transfer[0].blocks == 6u);
        /* An acknowledgement of block 1 sends block 2, and so on to the end of the file. The window is one
           block wide here, because the modem asked for no wsize. */
        uint8_t ack[4] = { 0, FUXI_TFTP_ACK, 0, 1 };
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_TRANSFER_PORT_BASE, 0, 40, ack, 4u, &ops, 0, &opcode, &wide) == 1);
        CHECK(opcode == FUXI_TFTP_ACK);
        CHECK(sent_count == 2u);
        CHECK(sent[1][1] == FUXI_TFTP_DATA);
        CHECK(sent[1][2] == 0 && sent[1][3] == 2);
        /* The rest of the file, one block per acknowledgement. */
        for (unsigned block = 2; block <= 5u; ++block) {
            ack[2] = 0;
            ack[3] = (uint8_t)block;
            CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_TRANSFER_PORT_BASE, 0, 40, ack, 4u, &ops, 0, &opcode, &wide) == 1);
        }
        CHECK(sent_count == 6u);
        /* The last block is the short one: 3000 - 5*512 = 440 bytes. */
        CHECK(sent_length[5] == 4u + 440u);
        CHECK(sent[5][2] == 0 && sent[5][3] == 6);
        CHECK(sent[5][1] == FUXI_TFTP_DATA);
    }

    /* The window: an acknowledgement of n sends the next wsize blocks, and the reference's rule is the
       same (umi_modem_mhi.inc:838-840). */
    {
        struct fuxi_tftp_state state;
        memset(&state, 0, sizeof state);
        sent_count = 0;
        uint8_t req[256];
        static const char *const names[] = { "wsize" };
        static const char *const values[] = { "4" };
        const uint32_t n = build_request(req, sizeof req, FUXI_TFTP_RRQ, "/readonly/firmware/image/x", names,
                                         values, 1u);
        uint32_t opcode = 0, wide = 0;
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_PORT, 0, 40, req, n, &ops, 0, &opcode, &wide) == 1);
        CHECK(state.transfer[0].wsize == 4u);
        /* An acknowledgement of block 0 sends blocks 1 to 4. The first block went out on the request
           itself, so it is the second of the four that the window sends. */
        uint8_t ack[4] = { 0, FUXI_TFTP_ACK, 0, 0 };
        CHECK(fuxi_tftp_serve(&state, FUXI_TFTP_TRANSFER_PORT_BASE, 0, 40, ack, 4u, &ops, 0, &opcode, &wide) == 1);
        CHECK(sent_count == 5u);
        CHECK(sent[1][3] == 1);
        CHECK(sent[4][3] == 4);
    }

    /* The block length: a file of exactly one block's worth has one block that is full and one that is
       empty, and the empty one ends the transfer. */
    {
        CHECK(fuxi_tftp_blocks(0, 512) == 1u);
        CHECK(fuxi_tftp_blocks(512, 512) == 2u);
        CHECK(fuxi_tftp_blocks(3000, 512) == 6u);
        CHECK(fuxi_tftp_block_length(3000, 512, 1) == 512u);
        CHECK(fuxi_tftp_block_length(3000, 512, 5) == 512u);
        CHECK(fuxi_tftp_block_length(3000, 512, 6) == 440u);
        CHECK(fuxi_tftp_block_length(512, 512, 2) == 0u);
    }

    /* The answers' shapes. */
    {
        uint8_t out[64];
        uint32_t n = fuxi_tftp_build_ack(out, sizeof out, 0x1234);
        CHECK(n == 4u);
        CHECK(out[0] == 0 && out[1] == FUXI_TFTP_ACK);
        CHECK(out[2] == 0x12 && out[3] == 0x34);
        n = fuxi_tftp_build_error(out, sizeof out, FUXI_TFTP_ENOENT, "file not found");
        CHECK(n == 4u + 15u);
        CHECK(out[1] == FUXI_TFTP_ERROR);
        CHECK(out[2] == 0 && out[3] == FUXI_TFTP_ENOENT);
        CHECK(memcmp(out + 4, "file not found", 15) == 0);
        n = fuxi_tftp_build_data(out, sizeof out, 7, file_bytes, 16);
        CHECK(n == 20u);
        CHECK(out[1] == FUXI_TFTP_DATA);
        CHECK(out[2] == 0 && out[3] == 7);
        CHECK(memcmp(out + 4, file_bytes, 16) == 0);
        /* An answer that does not fit is not built. */
        CHECK(fuxi_tftp_build_data(out, 8u, 7, file_bytes, 16) == 0);
    }

    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("tftp model: ok\n");
    return 0;
}
