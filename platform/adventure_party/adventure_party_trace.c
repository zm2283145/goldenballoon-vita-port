/* adventure_party_trace.c — see adventure_party_trace.h.
 *
 * Implementation notes:
 *
 *   - Every formatter is one straight-line snprintf (or a short run of them
 *     via append(), for the variable-length roster). The return value is
 *     snprintf's own would-be length, so truncation is detectable and never
 *     overflows the caller buffer.
 *   - NULL value-struct pointers are tolerated by substituting zeros/OFF: an
 *     _emit called on a torn-down session must produce a harmless line, never
 *     dereference garbage. This is the only "logic" in the file and it lives
 *     with the formatters, so the _emit wrappers stay pure plumbing.
 *   - Only `state` is named (adventure_party_state_name); every other enum is
 *     emitted as its integer so the schema owns no second name table.
 */
#include "adventure_party/adventure_party_trace.h"

#include <stdarg.h>
#include <stdio.h>

#include "mdkr_trace.h"

/* snprintf into buf at the running (snprintf-style) offset `used`, returning
 * the new running length. Safe once `used` has passed `cap`: it stops writing
 * but keeps counting, so the final return is the full length the line needs. */
static int append(char *buf, size_t cap, int used, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    if (used >= 0 && (size_t)used < cap) {
        n = vsnprintf(buf + used, cap - (size_t)used, fmt, ap);
    } else {
        char scratch[1];
        n = vsnprintf(scratch, 0, fmt, ap);
    }
    va_end(ap);
    if (n < 0)
        return n;
    return used + n;
}

int adventure_party_trace_format_schema(char *buf, size_t cap) {
    return snprintf(buf, cap, "aparty_schema: v=%d",
                    ADVENTURE_PARTY_TRACE_SCHEMA_VERSION);
}

int adventure_party_trace_format_session(char *buf, size_t cap,
                                         const AdventurePartySession *session) {
    AdventurePartySessionState state =
        session ? session->state : ADVENTURE_PARTY_STATE_OFF;
    uint32_t sgen = session ? session->session_generation : 0u;
    uint32_t lgen = session ? session->level_generation : 0u;
    unsigned host = session ? session->host_seat : 0u;
    return snprintf(buf, cap, "aparty_session: state=%s sgen=%u lgen=%u host=%u",
                    adventure_party_state_name(state),
                    (unsigned)sgen, (unsigned)lgen, host);
}

int adventure_party_trace_format_roster(char *buf, size_t cap,
                                        const AdventurePartyRoster *roster) {
    unsigned count = roster ? roster->participant_count : 0u;
    unsigned mask = roster ? roster->seat_mask : 0u;
    int used = append(buf, cap, 0, "aparty_roster: n=%u mask=0x%x", count, mask);
    if (roster) {
        for (int s = 0; s < ADVENTURE_PARTY_MAX_SEATS; s++) {
            if (roster->seat_mask & (unsigned)(1u << s))
                used = append(buf, cap, used, " c%d=%u", s,
                              (unsigned)roster->character_by_seat[s]);
        }
    }
    return used;
}

int adventure_party_trace_format_binding(char *buf, size_t cap, uint8_t seat,
                                         uint8_t controller_port) {
    return snprintf(buf, cap, "aparty_binding: seat=%u port=%u",
                    (unsigned)seat, (unsigned)controller_port);
}

int adventure_party_trace_format_transition(
    char *buf, size_t cap, const AdventurePartyTransitionRequest *winner) {
    unsigned seat = winner ? winner->initiating_seat : 0u;
    unsigned tick = winner ? winner->simulation_tick : 0u;
    unsigned trigger = winner ? winner->trigger_kind : 0u;
    unsigned dest = winner ? winner->destination : 0u;
    unsigned lgen = winner ? winner->level_generation : 0u;
    return snprintf(buf, cap,
                    "aparty_transition: seat=%u tick=%u trigger=%u dest=%u "
                    "lgen=%u",
                    seat, tick, trigger, dest, lgen);
}

int adventure_party_trace_format_interaction(char *buf, size_t cap,
                                             uint8_t seat,
                                             AdventurePartyActionKind action,
                                             AdventurePartyArbitration verdict) {
    return snprintf(buf, cap, "aparty_interaction: seat=%u action=%d verdict=%d",
                    (unsigned)seat, (int)action, (int)verdict);
}

int adventure_party_trace_format_award(char *buf, size_t cap,
                                       AdventurePartyTraceAwardOp op,
                                       const AdventurePartyCompletionToken *token,
                                       int result) {
    const char *op_name =
        (op == ADVENTURE_PARTY_TRACE_AWARD_ISSUE) ? "issue" : "consume";
    unsigned sgen = token ? token->session_generation : 0u;
    unsigned lgen = token ? token->level_generation : 0u;
    unsigned course = token ? token->course : 0u;
    unsigned activity = token ? token->activity : 0u;
    unsigned kind = token ? token->completion_kind : 0u;
    return snprintf(buf, cap,
                    "aparty_award: op=%s sgen=%u lgen=%u course=%u activity=%u "
                    "kind=%u result=%d",
                    op_name, sgen, lgen, course, activity, kind, result);
}

int adventure_party_trace_format_layout(char *buf, size_t cap,
                                        int viewport_count, int layout_id) {
    return snprintf(buf, cap, "aparty_layout: viewports=%d layout=%d",
                    viewport_count, layout_id);
}

int adventure_party_trace_format_restore(char *buf, size_t cap,
                                         uint32_t suspend_generation,
                                         uint32_t restore_generation,
                                         int roster_match) {
    return snprintf(buf, cap,
                    "aparty_restore: suspend_lgen=%u restore_lgen=%u match=%d",
                    (unsigned)suspend_generation, (unsigned)restore_generation,
                    roster_match ? 1 : 0);
}

void adventure_party_trace_emit_schema(void) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_schema(buf, sizeof buf);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_session(const AdventurePartySession *session) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_session(buf, sizeof buf, session);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_roster(const AdventurePartyRoster *roster) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_roster(buf, sizeof buf, roster);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_binding(uint8_t seat, uint8_t controller_port) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_binding(buf, sizeof buf, seat, controller_port);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_transition(
    const AdventurePartyTransitionRequest *winner) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_transition(buf, sizeof buf, winner);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_interaction(uint8_t seat,
                                            AdventurePartyActionKind action,
                                            AdventurePartyArbitration verdict) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_interaction(buf, sizeof buf, seat, action,
                                             verdict);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_award(AdventurePartyTraceAwardOp op,
                                      const AdventurePartyCompletionToken *token,
                                      int result) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_award(buf, sizeof buf, op, token, result);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_layout(int viewport_count, int layout_id) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_layout(buf, sizeof buf, viewport_count,
                                        layout_id);
    MDKR_TRACE("%s", buf);
}
void adventure_party_trace_emit_restore(uint32_t suspend_generation,
                                        uint32_t restore_generation,
                                        int roster_match) {
    char buf[ADVENTURE_PARTY_TRACE_LINE_MAX];
    if (!mdkr_trace_enabled()) return;
    adventure_party_trace_format_restore(buf, sizeof buf, suspend_generation,
                                         restore_generation, roster_match);
    MDKR_TRACE("%s", buf);
}
