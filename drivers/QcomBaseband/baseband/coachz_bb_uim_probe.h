/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * One read-only request to the modem's UIM service: the status of the SIM slots.
 *
 * The eSIM control in coachz_bb_esim.h only speaks when a client asks it to, and a client here is an app that
 * wants an eUICC. Reading a physical card is not that: it is one request for the slot status, and its answer is
 * the state of the card in slot one.
 *
 * The request is the same one the eSIM control sends when it does not know the slots yet
 * (coachz_bb_esim.h, cbe_send_slot_status: CBE_UIM_GET_SLOT_STATUS, no TLVs), so the answer's layout is the one
 * that is already understood in this tree. The request asks for the state and nothing else: it does not switch
 * the slot, it does not authenticate, and it does not write anything to the card.
 *
 * The answer is not decoded here. The TLV layout of the slot status is not in this tree, so the bytes are what
 * settles it: the log prints the whole answer, and an answer that names a card and an answer that does not are
 * different in a way that can be read off it. The one thing that is decoded is the result, which is the same
 * result TLV every other service of this bridge reads.
 */
#ifndef COACHZ_BB_UIM_PROBE_H
#define COACHZ_BB_UIM_PROBE_H

#include <stdint.h>

#define CBP_SVC_UIM 0x0bu
/* Get Slot Status (Qualcomm's user_identity_module_v01.h). */
#define CBP_UIM_GET_SLOT_STATUS 0x0047u
/* The result TLV, the same one the SSCTL answer carries. */
#define CBP_TLV_RESULT 0x02u

/* The state of the one request. */
struct cbp_uim {
    uint8_t sent;        /* the request went out */
    uint8_t answered;    /* an answer came back */
    uint32_t error;      /* the result the modem reported */
    /* Every TLV the answer carried, whatever its type. The type of the slot status is not assumed here, so the
       types the answer actually carried are what settles it. */
    uint8_t tlv_types[16];
    uint8_t tlv_lens[16];
    uint32_t tlv_count;
};

/* Builds the request: a QMI request SDU with no TLVs. The Get Slot Status request takes none
 * (Qualcomm's user_identity_module_v01.h: the request message has no input). */
static inline uint32_t cbp_build_request(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t message)
{
    if (room < 7u) return 0;
    sdu[0] = 0x00u;             /* flags: a request */
    sdu[1] = (uint8_t)txn;     /* transaction */
    sdu[2] = 0x00u;
    sdu[3] = (uint8_t)(message & 0xffu);
    sdu[4] = (uint8_t)(message >> 8);
    sdu[5] = 0x00u;             /* TLV length: no TLVs */
    sdu[6] = 0x00u;
    return 7u;
}

/* Power On (0x30): TLV 0x01 {u8 slot}, the standard step that makes the card readable. */
#define CBP_UIM_POWER_ON 0x0030u
static inline uint32_t cbp_build_tlv(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t message,
                                     uint8_t type, const uint8_t *value, uint32_t value_len)
{
    if (value_len > 0xffu || room < 7u + 3u + value_len) return 0;
    sdu[0] = 0x00u; sdu[1] = (uint8_t)txn; sdu[2] = 0x00u;
    sdu[3] = (uint8_t)message; sdu[4] = (uint8_t)(message >> 8);
    const uint32_t tl = 3u + value_len;
    sdu[5] = (uint8_t)tl; sdu[6] = (uint8_t)(tl >> 8);
    sdu[7] = type; sdu[8] = (uint8_t)value_len; sdu[9] = 0x00u;
    for (uint32_t i = 0; i < value_len; ++i) sdu[10 + i] = value[i];
    return 10u + value_len;
}

/* Register Events with the card-status event mask the upper layer uses (TLV 0x01, four bytes, bit 0). */
static inline uint32_t cbp_build_register(uint8_t *sdu, uint32_t room, uint32_t txn)
{
    if (room < 14u) return 0;
    sdu[0] = 0x00u; sdu[1] = (uint8_t)txn; sdu[2] = 0x00u;
    sdu[3] = 0x2eu; sdu[4] = 0x00u; sdu[5] = 0x07u; sdu[6] = 0x00u;
    sdu[7] = 0x01u; sdu[8] = 0x04u; sdu[9] = 0x00u;
    sdu[10] = 0x01u; sdu[11] = 0x00u; sdu[12] = 0x00u; sdu[13] = 0x00u;
    return 14u;
}

/* Read Transparent (0x20): TLV 0x02 {u16 file, u8 path length, path} (coachz_bb_data.h, cbd_uim_read_file). */
static inline uint32_t cbp_build_read(uint8_t *sdu, uint32_t room, uint32_t txn, uint16_t file,
                                      const uint8_t *path, uint8_t path_len)
{
    const uint32_t tl = 2u + 1u + path_len;
    if (path_len > 8u || room < 7u + 3u + tl) return 0;
    sdu[0] = 0x00u; sdu[1] = (uint8_t)txn; sdu[2] = 0x00u;
    sdu[3] = 0x20u; sdu[4] = 0x00u; sdu[5] = (uint8_t)(3u + tl); sdu[6] = (uint8_t)((3u + tl) >> 8);
    uint32_t at = 7u;
    sdu[at++] = 0x02u; sdu[at++] = (uint8_t)tl; sdu[at++] = 0x00u;
    sdu[at++] = (uint8_t)file; sdu[at++] = (uint8_t)(file >> 8);
    sdu[at++] = path_len;
    for (uint32_t i = 0; i < path_len; ++i) sdu[at++] = path[i];
    return at;
}

/* Change Provisioning Session (0x38): TLV 0x01 {u8 session_type, u16 session_id} and
   TLV 0x10 {u8 slot, u8 aid_len, u8 aid[]} (libqmi uim-service: Change Provisioning Session). */
#define CBP_UIM_CHANGE_SESSION 0x0038u
#define CBP_UIM_SESSION_PRIMARY_GW 0u
static inline uint32_t cbp_build_session(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t message,
                                         uint8_t session_type, uint16_t session_id,
                                         uint8_t slot, const uint8_t *aid, uint8_t aid_len)
{
    const uint32_t t1 = 3u + 3u;                 /* session change */
    const uint32_t t10 = 3u + 2u + aid_len;      /* application information */
    if (aid_len == 0u || room < 7u + t1 + t10) return 0;
    sdu[0] = 0x00u; sdu[1] = (uint8_t)txn; sdu[2] = 0x00u;
    sdu[3] = (uint8_t)message; sdu[4] = (uint8_t)(message >> 8);
    const uint32_t tl = t1 + t10;
    sdu[5] = (uint8_t)tl; sdu[6] = (uint8_t)(tl >> 8);
    uint32_t at = 7u;
    sdu[at++] = 0x01u; sdu[at++] = 0x03u; sdu[at++] = 0x00u;
    sdu[at++] = session_type; sdu[at++] = (uint8_t)session_id; sdu[at++] = (uint8_t)(session_id >> 8);
    sdu[at++] = 0x10u; sdu[at++] = (uint8_t)(2u + aid_len); sdu[at++] = 0x00u;
    sdu[at++] = slot; sdu[at++] = aid_len;
    for (uint32_t i = 0; i < aid_len; ++i) sdu[at++] = aid[i];
    return at;
}

/* Reads the answer: the result and the list of TLVs the answer carried. Returns 1 when the answer was a response.
 *
 * The result TLV is type 0x02 and carries the error code in the last two bytes of its value (the same layout the
 * bridge's own TLV writer uses, cbq_tlv_result, and the same place the SSCTL reader takes it from). The TLVs of
 * a QMI answer start at byte 7, each one a type byte, a 16-bit little-endian length and then the value.
 *
 * A TLV whose length runs past the end of the answer ends the walk rather than failing the whole answer: the
 * TLVs before it were read and they are worth having, and the raw bytes are on the log either way. */
static inline int cbp_read_answer(struct cbp_uim *st, const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || sdu[0] != 0x02u) return 0;   /* 0x02 is a response */
    const uint32_t end = 7u + (uint32_t)(sdu[5] | (sdu[6] << 8));
    if (end > n) return 0;
    st->error = 0;
    for (uint32_t at = 7u; at + 3u <= end;) {
        const uint32_t type = sdu[at];
        const uint32_t len = sdu[at + 1] | (sdu[at + 2] << 8);
        if (at + 3u + len > end) break;
        if (type == CBP_TLV_RESULT && len >= 2u) {
            const uint8_t *value = sdu + at + 3;
            st->error = (uint32_t)(value[len - 2] | value[len - 1] << 8);
        }
        if (st->tlv_count < 16u) {
            st->tlv_types[st->tlv_count] = (uint8_t)type;
            st->tlv_lens[st->tlv_count] = (uint8_t)(len > 255u ? 255u : len);
            st->tlv_count++;
        }
        at += 3u + len;
    }
    return 1;
}

#endif
