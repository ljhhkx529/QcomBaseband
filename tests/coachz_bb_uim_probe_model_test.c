/* Host model of the read-only UIM probe: the one request that asks the modem for the state of the card slots.
   Built with -fsanitize=address,undefined by scripts/check.py.

   The request is a QMI request SDU with no TLVs, and the answer is a QMI response whose TLVs start at byte 7.
   The result TLV is type 0x02 and carries the error code in its last two bytes, the same layout every other
   service of this bridge reads.

   What is worth a test here is the reader: a TLV walk that runs off the end of the answer, or that mistakes the
   result for the card's state, would show a card that is not there, and the whole point of the probe is to see
   whether there is a card. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "coachz_bb_uim_probe.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Builds a QMI response: the header, then the TLVs, then the TLV length the header carries. */
static uint32_t build_answer(uint8_t *sdu, uint32_t room, const uint8_t *tlvs, uint32_t tlv_len)
{
    if (room < 7u + tlv_len) return 0;
    sdu[0] = 0x02u;                                  /* a response */
    sdu[1] = 0x23u; sdu[2] = 0x00u;                 /* the transaction the request used */
    sdu[3] = 0x47u; sdu[4] = 0x00u;                 /* Get Slot Status */
    sdu[5] = (uint8_t)(tlv_len & 0xffu);
    sdu[6] = (uint8_t)(tlv_len >> 8);
    memcpy(sdu + 7, tlvs, tlv_len);
    return 7u + tlv_len;
}

/* One TLV: the type, the length, then the value. */
static uint32_t add_tlv(uint8_t *out, uint32_t at, uint32_t type, const uint8_t *value, uint32_t len)
{
    out[at] = (uint8_t)type;
    out[at + 1] = (uint8_t)(len & 0xffu);
    out[at + 2] = (uint8_t)(len >> 8);
    memcpy(out + at + 3, value, len);
    return at + 3u + len;
}

int main(void)
{
    /* The request: a request SDU, the message id 0x0047, no TLVs. */
    uint8_t sdu[64];
    const uint32_t n = cbp_build_request(sdu, sizeof sdu, 0x23u, CBP_UIM_GET_SLOT_STATUS);
    CHECK(n == 7u);
    CHECK(sdu[0] == 0x00u);                    /* a request, not a response */
    CHECK(sdu[1] == 0x23u);                    /* the transaction */
    CHECK(sdu[3] == 0x47u && sdu[4] == 0x00u); /* Get Slot Status */
    CHECK(sdu[5] == 0x00u && sdu[6] == 0x00u); /* no TLVs */

    /* A request that does not fit is not built. */
    CHECK(cbp_build_request(sdu, 6u, 0x23u, CBP_UIM_GET_SLOT_STATUS) == 0);

    /* An answer with no TLVs at all: a response, no error, nothing carried. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[64];
        const uint32_t m = build_answer(a, sizeof a, NULL, 0);
        CHECK(cbp_read_answer(&st, a, m) == 1);
        CHECK(st.error == 0);
        CHECK(st.tlv_count == 0);
    }

    /* The result TLV: type 0x02, four bytes, the error code in the last two. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[64], t[64];
        const uint8_t result[4] = { 0x00, 0x00, 0x5eu, 0x00u };   /* error 94 */
        uint32_t w = 0;
        w = add_tlv(t, w, CBP_TLV_RESULT, result, 4);
        const uint32_t m = build_answer(a, sizeof a, t, w);
        CHECK(cbp_read_answer(&st, a, m) == 1);
        CHECK(st.error == 94u);
        CHECK(st.tlv_count == 1);
        CHECK(st.tlv_types[0] == CBP_TLV_RESULT);
        CHECK(st.tlv_lens[0] == 4);
    }

    /* An answer that carries a card's state as well: the result and one more TLV. The reader must see both and
       must not mistake the second for the result. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[128], t[128];
        const uint8_t result[4] = { 0x00, 0x00, 0x00, 0x00 };
        const uint8_t state[9] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
        uint32_t w = 0;
        w = add_tlv(t, w, CBP_TLV_RESULT, result, 4);
        w = add_tlv(t, w, 0x22u, state, 9);
        const uint32_t m = build_answer(a, sizeof a, t, w);
        CHECK(cbp_read_answer(&st, a, m) == 1);
        CHECK(st.error == 0);
        CHECK(st.tlv_count == 2);
        CHECK(st.tlv_types[0] == CBP_TLV_RESULT);
        CHECK(st.tlv_types[1] == 0x22u);
        CHECK(st.tlv_lens[1] == 9);
    }

    /* Something that is not a response is not an answer. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[64];
        memset(a, 0, sizeof a);
        a[0] = 0x00u;             /* a request, or an indication */
        CHECK(cbp_read_answer(&st, a, 32u) == 0);
        a[0] = 0x04u;             /* an indication */
        CHECK(cbp_read_answer(&st, a, 32u) == 0);
    }

    /* An answer whose TLV area runs past the end of the buffer is refused, not read off the end. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[64];
        memset(a, 0, sizeof a);
        a[0] = 0x02u;
        a[5] = 0xffu; a[6] = 0xffu;     /* the TLV area claims 65535 bytes */
        CHECK(cbp_read_answer(&st, a, 32u) == 0);
    }

    /* A TLV whose length runs past the end of the answer stops the walk: the TLVs before it are still counted. */
    {
        struct cbp_uim st;
        memset(&st, 0, sizeof st);
        uint8_t a[64], t[64];
        const uint8_t result[4] = { 0x00, 0x00, 0x2bu, 0x00u };   /* error 43 */
        uint32_t w = 0;
        w = add_tlv(t, w, CBP_TLV_RESULT, result, 4);
        t[w] = 0x22u; t[w + 1] = 0xffu; t[w + 2] = 0x00u;        /* a TLV of 255 bytes that is not there */
        w += 3u;
        const uint32_t m = build_answer(a, sizeof a, t, w);
        CHECK(cbp_read_answer(&st, a, m) == 1);
        CHECK(st.error == 43u);
        CHECK(st.tlv_count == 1);
    }

    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("uim probe model: ok\n");
    return 0;
}
