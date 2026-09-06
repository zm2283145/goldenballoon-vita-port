/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_input_repair.h"

static MdkrPadSample sample(unsigned index) {
    const MdkrPadSample value = {
        (uint16_t)(0x2100u + index * 0x11u),
        (int8_t)(-80 + (int)index * 13),
        (int8_t)(80 - (int)index * 11), 1u};
    return value;
}

static void fill_answer(MdkrMatchInputRepair *answer, uint8_t count) {
    unsigned index;
    assert(count <= MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS);
    memset(answer, 0, sizeof(*answer));
    answer->kind = MDKR_MATCH_INPUT_REPAIR_ANSWER;
    answer->match_epoch = UINT32_C(0x0a0b0c0d);
    answer->first_tick = 4000u;
    answer->slot = 2u;
    answer->count = count;
    for (index = 0u; index < count; index++)
        answer->samples[index] = sample(index);
}

int main(void) {
    uint8_t bytes[MDKR_MATCH_INPUT_REPAIR_BYTES];
    uint8_t mutated[MDKR_MATCH_INPUT_REPAIR_BYTES];
    MdkrMatchInputRepair source;
    MdkrMatchInputRepair decoded;
    unsigned index;

    /* The batch cap is the rollback budget: a contiguous run longer than the
     * retained authored depth could not be reconciled even if it arrived. */
    assert(MDKR_MATCH_INPUT_REPAIR_MAX_TICKS ==
           MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS);
    /* One request is answered by a bounded number of fixed-size messages. */
    assert(MDKR_MATCH_INPUT_REPAIR_MAX_ANSWERS *
               MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS >=
           MDKR_MATCH_INPUT_REPAIR_MAX_TICKS);

    /* ---- request round trip ------------------------------------------- */
    memset(&source, 0, sizeof(source));
    source.kind = MDKR_MATCH_INPUT_REPAIR_REQUEST;
    source.match_epoch = UINT32_C(0x01020304);
    source.first_tick = 777u;
    source.count = MDKR_MATCH_INPUT_REPAIR_MAX_TICKS;
    assert(mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_input_repair_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);
    /* The epoch is on the wire, not implied by the connection: a repair for a
     * retired epoch must be recognizable as such by the receiver. */
    assert(bytes[6] == 1u && bytes[7] == 2u && bytes[8] == 3u &&
           bytes[9] == 4u);
    /* A request carries no samples. */
    for (index = 14u; index < 62u; index++) assert(bytes[index] == 0u);

    /* A request may not exceed the batch cap, and may not ask for nothing. */
    source.count = MDKR_MATCH_INPUT_REPAIR_MAX_TICKS + 1u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    source.count = 0u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    /* A request naming a canonical slot is malformed: the responder answers
     * for every slot it owns, and the requester does not choose. */
    source.count = 4u;
    source.slot = 1u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    source.slot = 0u;
    /* A sample in a request is malformed for the same reason. */
    source.samples[0] = sample(0u);
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));

    /* ---- answer round trip -------------------------------------------- */
    fill_answer(&source, MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS);
    assert(mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_input_repair_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);

    /* A short answer round trips and leaves the unused cells zeroed, so two
     * encodings of the same run are byte-identical. */
    fill_answer(&source, 5u);
    assert(mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    for (index = 14u + 5u * 4u; index < 62u; index++)
        assert(bytes[index] == 0u);
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(mdkr_match_input_repair_decode(bytes, sizeof(bytes), &decoded));
    assert(memcmp(&source, &decoded, sizeof(source)) == 0);

    /* Change only the rejected metadata; the fixture itself must stay within
     * its sample storage, even when exercising the encoder's count check. */
    fill_answer(&source, MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS);
    source.count = MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS + 1u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));

    /* Every carried sample must be a present pad inside the canonical stick
     * range: a repaired input is an ordinary input, held to the same shape the
     * bundle carrier holds. */
    fill_answer(&source, 3u);
    source.samples[1].present = 0u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    fill_answer(&source, 3u);
    source.samples[2].stick_x = 81;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    fill_answer(&source, 3u);
    source.samples[0].stick_y = -81;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));

    fill_answer(&source, 3u);
    source.slot = MDKR_SESSION_MAX_PLAYERS;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));

    /* A zero epoch is never a live match. */
    fill_answer(&source, 3u);
    source.match_epoch = 0u;
    assert(!mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));

    /* ---- decode refuses what encode would never produce ---------------- */
    fill_answer(&source, MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS);
    assert(mdkr_match_input_repair_encode(&source, bytes, sizeof(bytes)));
    assert(!mdkr_match_input_repair_decode(bytes, sizeof(bytes) - 1u, &decoded));
    for (index = 0u; index < sizeof(bytes); index++) {
        memcpy(mutated, bytes, sizeof(mutated));
        mutated[index] = (uint8_t)(mutated[index] ^ 0x40u);
        /* Every single-bit flip is caught: the trailing CRC covers the whole
         * message, so a corrupted repair can never be committed as input. */
        assert(!mdkr_match_input_repair_decode(mutated, sizeof(mutated),
                                              &decoded));
    }
    /* An unknown version is refused rather than parsed on its byte layout. */
    memcpy(mutated, bytes, sizeof(mutated));
    mutated[2] = MDKR_MATCH_INPUT_REPAIR_VERSION + 1u;
    assert(!mdkr_match_input_repair_decode(mutated, sizeof(mutated), &decoded));
    /* An unknown kind likewise. */
    memcpy(mutated, bytes, sizeof(mutated));
    mutated[3] = 2u;
    assert(!mdkr_match_input_repair_decode(mutated, sizeof(mutated), &decoded));

    /* ---- per-peer answer budget --------------------------------------- */
    {
        MdkrMatchInputRepairBudget budget;
        unsigned charged;
        memset(&budget, 0, sizeof(budget));
        /* One honest request's worth is always answerable in full. */
        for (charged = 0u; charged < MDKR_MATCH_INPUT_REPAIR_ANSWER_BUDGET;
             charged++) {
            assert(mdkr_match_input_repair_budget_charge(&budget, 500u));
        }
        /* A flood past it costs the responder nothing more, however many
         * different runs the requests name. */
        for (charged = 0u; charged < 1000u; charged++) {
            assert(!mdkr_match_input_repair_budget_charge(&budget, 500u));
        }
        assert(budget.spent == MDKR_MATCH_INPUT_REPAIR_ANSWER_BUDGET);
        /* The budget refills when the responder's OWN authored tick advances,
         * and a legitimate request in the next tick is answered in full. */
        for (charged = 0u; charged < MDKR_MATCH_INPUT_REPAIR_ANSWER_BUDGET;
             charged++) {
            assert(mdkr_match_input_repair_budget_charge(&budget, 501u));
        }
        assert(!mdkr_match_input_repair_budget_charge(&budget, 501u));
        /* A budget spent on a tick is not refilled by returning to it: the
         * charge tracks the last tick seen, never a set of them. */
        assert(mdkr_match_input_repair_budget_charge(&budget, 500u));
        assert(!mdkr_match_input_repair_budget_charge(NULL, 500u));
    }

    assert(!mdkr_match_input_repair_encode(NULL, bytes, sizeof(bytes)));
    assert(!mdkr_match_input_repair_encode(&source, NULL, sizeof(bytes)));
    assert(!mdkr_match_input_repair_decode(bytes, sizeof(bytes), NULL));
    assert(!mdkr_match_input_repair_decode(NULL, sizeof(bytes), &decoded));

    puts("test_match_input_repair: PASS");
    return 0;
}
