/* Host model of the read-only radio probes: the three requests that ask the modem whether the radio is on, whether
   it has registered, and how strong the signal is.
   Built with -fsanitize=address,undefined by scripts/check.py.

   The layouts are the ones the QMI service defines:
     DMS Get Operating Mode (0x2d): the mode in TLV 0x01.
     NAS Get Serving System (0x24): TLV 0x01 {registration state, CS attach, PS attach, selected network, radio
       interfaces}, and the network in TLV 0x12 {MCC, MNC, the name}.
     NAS Get Signal Info (0x4f): the LTE signal in TLV 0x14 {RSSI, RSRQ, RSRP, SNR}, the first two signed bytes
       and the last two signed 16-bit numbers little-endian.

   What is worth a test here is the reader. A field read from the wrong place would report a signal that is not
   there, or a registration that has not happened, and the whole point of the probes is to see the radio's state. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "coachz_bb_nas_probe.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Builds a QMI response: the header, then the TLVs, then the TLV length the header carries. */
static uint32_t build_answer(uint8_t *sdu, uint32_t room, uint32_t message, const uint8_t *tlvs, uint32_t tlv_len)
{
    if (room < 7u + tlv_len) return 0;
    sdu[0] = 0x02u;                                  /* a response */
    sdu[1] = 0x31u; sdu[2] = 0x00u;                 /* the transaction */
    sdu[3] = (uint8_t)(message & 0xffu);
    sdu[4] = (uint8_t)(message >> 8);
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
    /* The requests: a request SDU, the message id, no TLVs. */
    {
        uint8_t sdu[32];
        const uint32_t n = cbn_build_request(sdu, sizeof sdu, 0x31u, CBN_DMS_GET_OPERATING_MODE);
        CHECK(n == 7u);
        CHECK(sdu[0] == 0x00u);
        CHECK(sdu[1] == 0x31u);
        CHECK(sdu[3] == 0x2du && sdu[4] == 0x00u);
        CHECK(sdu[5] == 0x00u && sdu[6] == 0x00u);
        /* A request that does not fit is not built. */
        CHECK(cbn_build_request(sdu, 6u, 0x31u, CBN_NAS_GET_SIGNAL_INFO) == 0);
    }

    /* The result TLV: type 0x02, four bytes, the error code in the last two. */
    {
        struct cbn_probe st;
        memset(&st, 0, sizeof st);
        uint8_t a[64], t[64];
        const uint8_t result[4] = { 0x00, 0x00, 0x5eu, 0x00u };   /* error 94 */
        uint32_t w = add_tlv(t, 0, CBN_TLV_RESULT, result, 4);
        const uint32_t m = build_answer(a, sizeof a, CBN_NAS_GET_SIGNAL_INFO, t, w);
        CHECK(cbn_read_answer(&st, a, m) == 1);
        CHECK(st.error == 94u);
        CHECK(st.tlv_count == 1);
    }

    /* Something that is not a response is not an answer. */
    {
        struct cbn_probe st;
        memset(&st, 0, sizeof st);
        uint8_t a[64];
        memset(a, 0, sizeof a);
        a[0] = 0x00u;
        CHECK(cbn_read_answer(&st, a, 32u) == 0);
    }

    /* DMS Get Operating Mode: the mode in TLV 0x01. */
    {
        struct cbn_probe st;
        struct cbn_radio r;
        memset(&st, 0, sizeof st);
        memset(&r, 0, sizeof r);
        uint8_t a[64], t[64];
        const uint8_t mode[1] = { CBN_MODE_ONLINE };
        uint32_t w = add_tlv(t, 0, CBN_TLV_OPERATING_MODE, mode, 1);
        const uint32_t m = build_answer(a, sizeof a, CBN_DMS_GET_OPERATING_MODE, t, w);
        CHECK(cbn_read_answer(&st, a, m) == 1);
        CHECK(st.error == 0u);
        CHECK(cbn_take_operating_mode(&r, a, m) == 1);
        CHECK(r.have_operating_mode == 1);
        CHECK(r.operating_mode == CBN_MODE_ONLINE);
    }

    /* An operating mode answer that carries no mode: nothing is taken, and nothing is invented. */
    {
        struct cbn_radio r;
        memset(&r, 0, sizeof r);
        uint8_t a[64];
        const uint32_t m = build_answer(a, sizeof a, CBN_DMS_GET_OPERATING_MODE, NULL, 0);
        CHECK(cbn_take_operating_mode(&r, a, m) == 0);
        CHECK(r.have_operating_mode == 0);
    }

    /* NAS Get Serving System: the registration state, and the network.
       Registered on 460-01: the registration state 1, attached, and the MCC 460 and the MNC 1. */
    {
        struct cbn_probe st;
        struct cbn_radio r;
        memset(&st, 0, sizeof st);
        memset(&r, 0, sizeof r);
        uint8_t a[128], t[128];
        const uint8_t serving[5] = { CBN_REG_REGISTERED, 1, 1, 0, 0 };
        const uint8_t plmn[8] = { 0xcc, 0x01, 0x01, 0x00, 'C', 'M', 'C', 0 };
        uint32_t w = 0;
        w = add_tlv(t, w, CBN_TLV_SERVING_SYSTEM, serving, sizeof serving);
        w = add_tlv(t, w, CBN_TLV_CURRENT_PLMN, plmn, sizeof plmn);
        const uint32_t m = build_answer(a, sizeof a, CBN_NAS_GET_SERVING_SYSTEM, t, w);
        CHECK(cbn_read_answer(&st, a, m) == 1);
        CHECK(cbn_take_serving_system(&r, a, m) == 1);
        CHECK(r.have_serving == 1);
        CHECK(r.reg_state == CBN_REG_REGISTERED);
        CHECK(r.cs_attached == 1);
        CHECK(r.ps_attached == 1);
        CHECK(r.have_plmn == 1);
        CHECK(r.mcc == 460u);
        CHECK(r.mnc == 1u);
    }

    /* A serving system answer with no network: the registration state is still there, the network is not, and the
       network's fields keep the zero they were given rather than a leftover from another answer. */
    {
        struct cbn_radio r;
        memset(&r, 0, sizeof r);
        uint8_t a[64], t[64];
        const uint8_t serving[3] = { CBN_REG_NOT_REGISTERED_SEARCHING, 0, 0 };
        uint32_t w = add_tlv(t, 0, CBN_TLV_SERVING_SYSTEM, serving, sizeof serving);
        const uint32_t m = build_answer(a, sizeof a, CBN_NAS_GET_SERVING_SYSTEM, t, w);
        CHECK(cbn_take_serving_system(&r, a, m) == 1);
        CHECK(r.reg_state == CBN_REG_NOT_REGISTERED_SEARCHING);
        CHECK(r.have_plmn == 0);
        CHECK(r.mcc == 0u);
    }

    /* NAS Get Signal Info: the LTE signal. An RSSI of -71, an RSRQ of -9, an RSRP of -95 and an SNR of 13. */
    {
        struct cbn_probe st;
        struct cbn_radio r;
        memset(&st, 0, sizeof st);
        memset(&r, 0, sizeof r);
        uint8_t a[64], t[64];
        const uint8_t sig[6] = { 0xb9, 0xf7, 0xa1, 0xff, 0x0d, 0x00 };   /* -71, -9, -95, 13 */
        uint32_t w = add_tlv(t, 0, CBN_TLV_LTE_SIGNAL, sig, sizeof sig);
        const uint32_t m = build_answer(a, sizeof a, CBN_NAS_GET_SIGNAL_INFO, t, w);
        CHECK(cbn_read_answer(&st, a, m) == 1);
        CHECK(cbn_take_signal(&r, a, m) == 1);
        CHECK(r.have_signal == 1);
        CHECK(r.rssi == -71);
        CHECK(r.rsrq == -9);
        CHECK(r.rsrp == -95);
        CHECK(r.snr == 13);
    }

    /* A signal TLV that is too short is not read: the numbers would be read off the end of it. */
    {
        struct cbn_radio r;
        memset(&r, 0, sizeof r);
        uint8_t a[64], t[64];
        const uint8_t sig[4] = { 0xb9, 0xf7, 0xa1, 0xff };
        uint32_t w = add_tlv(t, 0, CBN_TLV_LTE_SIGNAL, sig, sizeof sig);
        const uint32_t m = build_answer(a, sizeof a, CBN_NAS_GET_SIGNAL_INFO, t, w);
        CHECK(cbn_take_signal(&r, a, m) == 0);
        CHECK(r.have_signal == 0);
    }

    /* The finder stops at the TLV it was asked for and does not run off the end of the answer. */
    {
        uint8_t a[64];
        memset(a, 0, sizeof a);
        a[0] = 0x02u;
        a[5] = 0xffu; a[6] = 0xffu;     /* the TLV area claims 65535 bytes */
        uint32_t len = 0;
        CHECK(cbn_find_tlv(a, 32u, CBN_TLV_RESULT, &len) == 0);
        CHECK(len == 0u);
    }

    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("nas probe model: ok\n");
    return 0;
}
