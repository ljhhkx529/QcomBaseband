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
