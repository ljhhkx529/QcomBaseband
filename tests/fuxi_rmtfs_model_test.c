/* Host model of RMTFS: the five messages the modem asks for, and the answers it is given.
   Built with -fsanitize=address,undefined by scripts/check.py.

   Why it exists: RMTFS answers a modem request in a QMI response whose layout is easy to get wrong in
   three places. The result TLV comes first and the answer's other TLV follows it, at byte 14. The path
   in OPEN is the TLV's value itself, with no length of its own in front of it. And the iovec's entries
   are the TLV's value with a count byte in front of them, each entry three 32-bit words, and the TLV's
   length has to be exactly a count byte and that many entries.

   The bounds matter too: a sector that would fall outside the file, or an address that would fall
   outside the buffer, has to be refused with the malformed-message error, and a message the server does
   not know has to be refused with it as well, so that a success is never answered to a request that was
   not understood.

   The wire format is the hardware-proven reference's (hakios.mpss.c, rmtfs_receive). */
#include <stdio.h>
#include <string.h>

#include "fuxi_rmtfs.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The TLV a request carries: a type byte, a two-byte length and the value. */
static uint32_t put_tlv(uint8_t *out, uint32_t at, uint8_t type, const void *value, uint32_t size)
{
    out[at++] = type;
    out[at++] = (uint8_t)size;
    out[at++] = (uint8_t)(size >> 8);
    memcpy(out + at, value, size);
    return at + size;
}

/* A request of the given message id, with its TLVs appended. */
static uint32_t build_request(uint8_t *out, uint16_t message, uint32_t at)
{
    memset(out, 0, 64);
    out[0] = 0x00;              /* a request */
    out[1] = 0x07;              /* the transaction */
    out[2] = 0x00;
    out[3] = (uint8_t)message;
    out[4] = (uint8_t)(message >> 8);
    return at;
}

/* ---- the adapter the test serves from ---- */

#define TEST_BUFFER 0x9c900000ull
#define TEST_BUFFER_SIZE 0x280000u

static int gOpened[4];
static uint32_t gSectorsRead, gSectorsWritten;

static int test_open(void *context, const uint8_t *path, uint32_t path_length)
{
    (void)context;
    for (unsigned i = 0; i < FUXI_RMTFS_FILES; ++i) {
        const uint32_t length = (uint32_t)strlen(fuxi_rmtfs_files[i].path);
        if (length != path_length || memcmp(fuxi_rmtfs_files[i].path, path, length)) continue;
        gOpened[i] = 1;
        return 0;
    }
    return -1;
}

static int test_io(void *context, uint32_t number, uint32_t sector, uint32_t buffer_phys)
{
    (void)context;
    if (!number || number > 4) return -1;
    if (sector >= 8) return -1;
    if (buffer_phys < TEST_BUFFER || buffer_phys - TEST_BUFFER + 512u > TEST_BUFFER_SIZE) return -1;
    gSectorsRead++;
    return 0;
}

static int test_write(void *context, uint32_t number, uint32_t sector, uint32_t buffer_phys)
{
    (void)context;
    if (!number || number > 4) return -1;
    if (sector >= 8) return -1;
    if (buffer_phys < TEST_BUFFER || buffer_phys - TEST_BUFFER + 512u > TEST_BUFFER_SIZE) return -1;
    gSectorsWritten++;
    return 0;
}

static int test_close(void *context, uint32_t number)
{
    (void)context;
    if (!number || number > 4) return -1;
    gOpened[number - 1] = 0;
    return 0;
}

static uint64_t test_buffer(void *context, uint32_t *size)
{
    (void)context;
    if (size) *size = TEST_BUFFER_SIZE;
    return TEST_BUFFER;
}

static uint64_t test_file_size(void *context, uint32_t number)
{
    (void)context;
    if (!number || number > 4) return 0;
    return 4096;
}

static const struct fuxi_rmtfs_ops test_ops = {
    test_open, test_io, test_write, test_close, test_buffer, test_file_size,
};

/* The result TLV of an answer: the result and the error, at bytes 10 and 12. */
static void result_of(const uint8_t *answer, uint32_t *result, uint32_t *error)
{
    *result = (uint32_t)answer[10] | (uint32_t)answer[11] << 8;
    *error = (uint32_t)answer[12] | (uint32_t)answer[13] << 8;
}

int main(void)
{
    struct fuxi_rmtfs_state state = {};
    uint8_t request[128], answer[256];
    uint32_t answer_length = 0, message = 0;

    /* ---- OPEN: the path is the TLV's value itself ---- */
    uint32_t at = build_request(request, FUXI_RMTFS_OPEN, 7);
    at = put_tlv(request, at, 1, "/boot/modem_fs1", 15);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    CHECK(message == FUXI_RMTFS_OPEN);
    /* The answer is a response, and the request's transaction and message id come back. */
    CHECK(answer[0] == 0x02);
    CHECK(answer[1] == 0x07 && answer[3] == FUXI_RMTFS_OPEN);
    /* The result TLV comes first, at bytes 7 to 13, and the answer's other TLV follows it. */
    CHECK(answer[7] == 0x02 && answer[8] == 4);
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
        CHECK(error == FUXI_RMTFS_ERR_NONE);
    }
    /* The file's number is TLV 0x10, a 32-bit value, and it is the file's place in the table plus one. */
    CHECK(answer[14] == 0x10 && answer[15] == 4 && answer[16] == 0);
    CHECK(answer[17] == 1);
    /* The length in the header is the result TLV's seven bytes plus what followed it. */
    CHECK(answer[5] == 7 + 7);
    CHECK(answer_length == 14 + 7);
    CHECK(state.open[0] == 1);

    /* A path the server does not know is refused with the internal error, and nothing else. */
    at = build_request(request, FUXI_RMTFS_OPEN, 7);
    at = put_tlv(request, at, 1, "/boot/nonexistent", 16);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
        CHECK(error == FUXI_RMTFS_ERR_INTERNAL);
    }
    /* The refusal carries the result TLV and nothing else. */
    CHECK(answer_length == 14);

    /* ---- ALLOC_BUFF: the buffer's address ---- */
    at = build_request(request, FUXI_RMTFS_ALLOC_BUFF, 7);
    const uint32_t asked = 0x1000;
    at = put_tlv(request, at, 2, &asked, 4);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
    }
    /* TLV 0x10 carries the address as a 64-bit little-endian value. */
    CHECK(answer[14] == 0x10 && answer[15] == 8 && answer[16] == 0);
    {
        uint64_t address = 0;
        for (unsigned b = 0; b < 8; ++b) address |= (uint64_t)answer[17 + b] << (8 * b);
        CHECK(address == TEST_BUFFER);
    }
    CHECK(answer_length == 14 + 11);

    /* A request for more than the buffer holds is refused. */
    at = build_request(request, FUXI_RMTFS_ALLOC_BUFF, 7);
    const uint32_t too_big = TEST_BUFFER_SIZE + 1;
    at = put_tlv(request, at, 2, &too_big, 4);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
    }

    /* ---- RW_IOVEC: the entries ---- */
    /* A read of one sector of the open file, into the buffer. */
    at = build_request(request, FUXI_RMTFS_RW_IOVEC, 7);
    const uint32_t number = 1;
    at = put_tlv(request, at, 1, &number, 4);
    const uint8_t read_direction = 0;
    at = put_tlv(request, at, 2, &read_direction, 1);
    {
        /* The entries: a count byte and then, for each entry, the sector, the address and the count. */
        uint8_t entries[13];
        entries[0] = 1;
        entries[1] = 0; entries[2] = 0; entries[3] = 0; entries[4] = 0;          /* the sector */
        const uint32_t address = (uint32_t)TEST_BUFFER;
        memcpy(entries + 5, &address, 4);
        const uint32_t sectors = 1;
        memcpy(entries + 9, &sectors, 4);
        at = put_tlv(request, at, 3, entries, sizeof entries);
    }
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
        CHECK(gSectorsRead == 1);
    }
    /* The answer carries the result and nothing else. */
    CHECK(answer_length == 14);

    /* An address outside the buffer is refused with the malformed-message error. */
    at = build_request(request, FUXI_RMTFS_RW_IOVEC, 7);
    at = put_tlv(request, at, 1, &number, 4);
    at = put_tlv(request, at, 2, &read_direction, 1);
    {
        uint8_t entries[13];
        entries[0] = 1;
        entries[1] = 0; entries[2] = 0; entries[3] = 0; entries[4] = 0;
        const uint32_t address = (uint32_t)(TEST_BUFFER - 512);
        memcpy(entries + 5, &address, 4);
        const uint32_t sectors = 1;
        memcpy(entries + 9, &sectors, 4);
        at = put_tlv(request, at, 3, entries, sizeof entries);
    }
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
        CHECK(error == FUXI_RMTFS_ERR_MALFORMED);
    }

    /* A sector past the end of the file is refused with the same error. */
    at = build_request(request, FUXI_RMTFS_RW_IOVEC, 7);
    at = put_tlv(request, at, 1, &number, 4);
    at = put_tlv(request, at, 2, &read_direction, 1);
    {
        uint8_t entries[13];
        entries[0] = 1;
        const uint32_t sector = 4096;         /* past the 4096-byte file */
        memcpy(entries + 1, &sector, 4);
        const uint32_t address = (uint32_t)TEST_BUFFER;
        memcpy(entries + 5, &address, 4);
        const uint32_t sectors = 1;
        memcpy(entries + 9, &sectors, 4);
        at = put_tlv(request, at, 3, entries, sizeof entries);
    }
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
        CHECK(error == FUXI_RMTFS_ERR_MALFORMED);
    }

    /* A file that is not open is refused with the same error. */
    at = build_request(request, FUXI_RMTFS_RW_IOVEC, 7);
    const uint32_t not_open = 3;
    at = put_tlv(request, at, 1, &not_open, 4);
    at = put_tlv(request, at, 2, &read_direction, 1);
    {
        uint8_t entries[13];
        entries[0] = 1;
        entries[1] = 0; entries[2] = 0; entries[3] = 0; entries[4] = 0;
        const uint32_t address = (uint32_t)TEST_BUFFER;
        memcpy(entries + 5, &address, 4);
        const uint32_t sectors = 1;
        memcpy(entries + 9, &sectors, 4);
        at = put_tlv(request, at, 3, entries, sizeof entries);
    }
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
        CHECK(error == FUXI_RMTFS_ERR_MALFORMED);
    }

    /* A write goes through the same shape, and the direction is anything but zero. */
    at = build_request(request, FUXI_RMTFS_RW_IOVEC, 7);
    at = put_tlv(request, at, 1, &number, 4);
    const uint8_t write_direction = 1;
    at = put_tlv(request, at, 2, &write_direction, 1);
    {
        uint8_t entries[13];
        entries[0] = 1;
        entries[1] = 0; entries[2] = 0; entries[3] = 0; entries[4] = 0;
        const uint32_t address = (uint32_t)TEST_BUFFER;
        memcpy(entries + 5, &address, 4);
        const uint32_t sectors = 1;
        memcpy(entries + 9, &sectors, 4);
        at = put_tlv(request, at, 3, entries, sizeof entries);
    }
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
        CHECK(gSectorsWritten == 1);
    }

    /* ---- CLOSE ---- */
    at = build_request(request, FUXI_RMTFS_CLOSE, 7);
    at = put_tlv(request, at, 1, &number, 4);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
    }
    CHECK(state.open[0] == 0);

    /* ---- GET_DEV_ERROR ---- */
    at = build_request(request, FUXI_RMTFS_GET_DEV_ERROR, 7);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_SUCCESS);
    }
    /* TLV 0x10 carries the status, a single byte, and it is zero. */
    CHECK(answer[14] == 0x10 && answer[15] == 1 && answer[16] == 0 && answer[17] == 0);
    CHECK(answer_length == 14 + 4);

    /* ---- a message the server does not know ---- */
    at = build_request(request, 99, 7);
    CHECK(fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                           &message));
    CHECK(message == 99);
    {
        uint32_t result = 0, error = 0;
        result_of(answer, &result, &error);
        CHECK(result == FUXI_RMTFS_RESULT_FAILURE);
        CHECK(error == FUXI_RMTFS_ERR_MALFORMED);
    }
    CHECK(answer_length == 14);

    /* ---- a packet that is not a request ---- */
    at = build_request(request, FUXI_RMTFS_OPEN, 7);
    request[0] = 0x02;             /* a response, not a request */
    CHECK(!fuxi_rmtfs_serve(&state, request, at, answer, sizeof answer, &answer_length, &test_ops, NULL,
                            &message));

    /* ---- a request too short to hold a header ---- */
    CHECK(!fuxi_rmtfs_serve(&state, request, 3, answer, sizeof answer, &answer_length, &test_ops, NULL,
                            &message));

    if (failures) {
        printf("FAIL fuxi_rmtfs_model_test: %u failures\n", failures);
        return 1;
    }
    printf("PASS fuxi_rmtfs_model_test\n");
    return 0;
}
