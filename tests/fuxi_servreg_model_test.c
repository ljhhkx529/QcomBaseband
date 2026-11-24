/* Host model of the service registry's locator: the table the answers come from and the answer's own
   layout. Built with -fsanitize=address,undefined by scripts/check.py.

   Why it exists: the locator answers a modem request in a QMI response whose TLVs have a fixed shape, and
   three things about that shape are easy to get wrong. The request's wanted name is TLV 1, a plain string
   and not an element of a list. The table's revision in TLV 0x11 is the constant 1 and not the version the
   .jsn file carries. And the domain record inside TLV 0x12 is a byte of name length, the name without a
   trailing NUL, the instance as four little-endian bytes, the validity byte and the service data as four
   more little-endian bytes.

   A message the locator does not know is answered with a failure and the invalid-message-id error, and with
   nothing else, so a test checks that no success is ever answered to an unknown id. */
#include <stdio.h>
#include <string.h>

#include "fuxi_servreg.h"

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

/* A GET_DOMAIN_LIST request that asks for one name. */
static uint32_t build_request(uint8_t *out, uint16_t message, const char *name)
{
    memset(out, 0, 32);
    out[0] = 0x00;              /* a request */
    out[1] = 0x07;              /* the transaction */
    out[2] = 0x00;
    out[3] = (uint8_t)message;
    out[4] = (uint8_t)(message >> 8);
    uint32_t at = 7;
    if (name) at = put_tlv(out, at, 1, name, (uint32_t)strlen(name));
    out[5] = (uint8_t)(at - 7);
    out[6] = (uint8_t)((at - 7) >> 8);
    return at;
}

/* The value of the TLV of the given type in the answer, and its length. */
static const uint8_t *answer_tlv(const uint8_t *out, uint32_t length, uint8_t type, uint32_t *size)
{
    for (uint32_t at = 7u; at + 3u <= length;) {
        const uint32_t n = (uint32_t)out[at + 1] | (uint32_t)out[at + 2] << 8;
        if (n > length - at - 3u) return 0;
        if (out[at] == type) {
            if (size) *size = n;
            return out + at + 3u;
        }
        at += 3u + n;
    }
    return 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(void)
{
    uint8_t request[64], answer[256];
    uint32_t answer_length = 0;
    char wanted[64];
    uint32_t found = 0;

    /* The name the modem asks for is answered out of the table. */
    const uint32_t request_length = build_request(request, FUXI_SERVREG_GET_DOMAIN_LIST, "tms/servreg");
    CHECK(fuxi_servreg_serve(request, request_length, answer, sizeof answer, &answer_length, wanted, sizeof wanted,
                              &found));
    CHECK(strcmp(wanted, "tms/servreg") == 0);
    /* One domain answers: the table names one domain for tms/servreg. */
    CHECK(found == 1);
    /* The answer is a response and echoes the request's transaction and message id. */
    CHECK(answer[0] == 0x02);
    CHECK(answer[1] == 0x07 && answer[2] == 0x00);
    CHECK(le16(answer + 3) == FUXI_SERVREG_GET_DOMAIN_LIST);
    /* The result is success, in TLV 0x02. */
    uint32_t size = 0;
    const uint8_t *value = answer_tlv(answer, answer_length, 0x02, &size);
    CHECK(value != 0);
    CHECK(size == 4);
    CHECK(le16(value) == 0 && le16(value + 2) == 0);
    /* The number of domains, in TLV 0x10. */
    value = answer_tlv(answer, answer_length, 0x10, &size);
    CHECK(value != 0);
    CHECK(size == 2);
    CHECK(le16(value) == 1);
    /* The table's revision, in TLV 0x11: the constant 1, not the version the .jsn file carries. */
    value = answer_tlv(answer, answer_length, 0x11, &size);
    CHECK(value != 0);
    CHECK(size == 2);
    CHECK(le16(value) == 1);
    /* The list, in TLV 0x12: a byte of count, then the domain record. */
    value = answer_tlv(answer, answer_length, 0x12, &size);
    CHECK(value != 0);
    CHECK(value[0] == 1);
    CHECK(value[1] == 17);                            /* the domain's length */
    CHECK(memcmp(value + 2, "msm/modem/root_pd", 17) == 0);
    /* The instance, four bytes little endian, then the validity byte and the service data. The instance
       follows the name, so its place depends on the name's length. */
    CHECK(le32(value + 19) == 180);
    CHECK(value[23] == 0);
    CHECK(le32(value + 24) == 0);

    /* The other service of the table is answered the same way. */
    const uint32_t gps_length = build_request(request, FUXI_SERVREG_GET_DOMAIN_LIST, "gps/gps_service");
    CHECK(fuxi_servreg_serve(request, gps_length, answer, sizeof answer, &answer_length, wanted, sizeof wanted, &found));
    CHECK(strcmp(wanted, "gps/gps_service") == 0);
    CHECK(found == 1);

    /* A name the table does not hold is answered with no domains, and with no list at all. */
    const uint32_t other_length = build_request(request, FUXI_SERVREG_GET_DOMAIN_LIST, "nas/anything");
    CHECK(fuxi_servreg_serve(request, other_length, answer, sizeof answer, &answer_length, wanted, sizeof wanted,
                             &found));
    CHECK(found == 0);
    value = answer_tlv(answer, answer_length, 0x10, &size);
    CHECK(value != 0 && le16(value) == 0);
    CHECK(answer_tlv(answer, answer_length, 0x12, &size) == 0);

    /* A message the locator does not know is answered with a failure and the invalid-message-id error, and
       with nothing else: no list, and no success. */
    const uint32_t unknown_length = build_request(request, 0x0099, "tms/servreg");
    CHECK(fuxi_servreg_serve(request, unknown_length, answer, sizeof answer, &answer_length, wanted, sizeof wanted,
                             &found));
    value = answer_tlv(answer, answer_length, 0x02, &size);
    CHECK(value != 0);
    CHECK(size == 4);
    CHECK(le16(value) == FUXI_QMI_RESULT_FAILURE);
    CHECK(le16(value + 2) == FUXI_QMI_ERR_INVALID_MESSAGE_ID);
    /* Nothing else is in the answer. */
    CHECK(answer_tlv(answer, answer_length, 0x10, &size) == 0);
    CHECK(answer_tlv(answer, answer_length, 0x12, &size) == 0);
    CHECK(answer_length == 14);

    /* A packet that is not a request is not answered at all. */
    uint8_t response[8] = {2, 7, 0, 0x21, 0, 0, 0, 0};
    CHECK(fuxi_servreg_serve(response, sizeof response, answer, sizeof answer, &answer_length, wanted, sizeof wanted,
                             &found) == 0);
    /* A packet too short to be a QMI message is not answered either. */
    CHECK(fuxi_servreg_serve(request, 4, answer, sizeof answer, &answer_length, wanted, sizeof wanted, &found) == 0);

    /* A request that names no service at all is answered with no domains rather than with every domain. */
    const uint32_t nameless_length = build_request(request, FUXI_SERVREG_GET_DOMAIN_LIST, 0);
    CHECK(fuxi_servreg_serve(request, nameless_length, answer, sizeof answer, &answer_length, wanted, sizeof wanted,
                             &found));
    CHECK(found == 0);

    if (failures) {
        printf("FAIL %d checks\n", failures);
        return 1;
    }
    printf("the servreg locator model passes\n");
    return 0;
}
