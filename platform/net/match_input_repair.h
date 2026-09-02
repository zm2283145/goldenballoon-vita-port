/* Explicit input-gap repair carried on the reliable-unordered authority lane.
 *
 * The realtime state channel is maxRetransmits 0 and each 64-byte bundle
 * carries three ticks of redundancy, so a loss burst shorter than three sends
 * heals itself. A longer burst leaves a contiguous hole the redundancy will
 * never cover again: the receiver names it (request) and the author answers it
 * from the input history it already keeps. See docs/ref/match-input-repair-v1.md.
 */
#ifndef MDKR_MATCH_INPUT_REPAIR_H
#define MDKR_MATCH_INPUT_REPAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rollback/rollback_limits.h"
#include "session/session_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_INPUT_REPAIR_VERSION 1u
#define MDKR_MATCH_INPUT_REPAIR_BYTES 64u

/* Cells one answer message carries: the 64-byte payload less its 14-byte
 * header and 2-byte CRC, at four bytes per pad sample. */
#define MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS 12u

/* The most ticks one request may name. A run older than the retained authored
 * depth cannot be reconciled even if it arrived, so asking for more would only
 * spend bandwidth on input the rollback window has already passed. */
#define MDKR_MATCH_INPUT_REPAIR_MAX_TICKS MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS

/* Answer messages one request costs, per canonical slot the author owns. */
#define MDKR_MATCH_INPUT_REPAIR_MAX_ANSWERS                                    \
    ((MDKR_MATCH_INPUT_REPAIR_MAX_TICKS +                                      \
      MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS - 1u) /                             \
     MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS)

typedef enum MdkrMatchInputRepairKind {
    MDKR_MATCH_INPUT_REPAIR_REQUEST = 0,
    MDKR_MATCH_INPUT_REPAIR_ANSWER = 1
} MdkrMatchInputRepairKind;

typedef struct MdkrMatchInputRepair {
    uint32_t match_epoch;
    /* Oldest tick of the contiguous run this message names. */
    uint32_t first_tick;
    uint8_t kind;
    /* Answer: the canonical slot the samples author. Zero in a request --
     * the responder answers for every slot it owns. */
    uint8_t slot;
    uint8_t count;
    MdkrPadSample samples[MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS];
} MdkrMatchInputRepair;

/* Both directions are total and structural: an encode refuses anything the
 * decode would reject, so the two are exact inverses over valid messages. */
bool mdkr_match_input_repair_encode(
    const MdkrMatchInputRepair *message, uint8_t *bytes, size_t length);
bool mdkr_match_input_repair_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputRepair *message);

#ifdef __cplusplus
}
#endif
#endif /* MDKR_MATCH_INPUT_REPAIR_H */
