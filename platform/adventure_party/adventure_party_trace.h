/* adventure_party_trace.h — Adventure Party read-only trace schema (AP-05).
 *
 * The one place the Adventure Party session's observable facts become text.
 * Every function here is a PURE FORMATTER over value structs the state and
 * policy modules already own (adventure_party_state.h / _policy.h): it writes
 * one line into a caller buffer and returns an snprintf-style length. Nothing
 * here mutates a session, opens a file, reads a controller, or touches the
 * renderer — the const pointers make "read-only" a compiler-checked property,
 * not a promise. The thin `_emit` wrappers add exactly one thing: they call
 * MDKR_TRACE (guarded by mdkr_trace_enabled()) so a shipping run evaluates
 * nothing. All logic lives in the formatters, which is why the whole schema is
 * unit-testable with no trace plumbing at all.
 *
 * ---------------------------------------------------------------------------
 * SCHEMA AUTHORITY (this comment is the contract; the golden file and both
 * language parsers must agree with it).
 *
 * Every line is `tag: k=v k=v ...`, the repo's MDKR_TRACE convention. The tag
 * is a stable `aparty_` prefix plus the fact class. Keys and their order are
 * part of the contract; a reader may key by name but the C formatter, the
 * golden file (tests/data/adventure_party_trace_golden.txt) and the stdlib
 * parser (tests/adventure_party_trace.py) all emit/expect the same order, so a
 * byte-for-byte golden is the drift alarm.
 *
 * Fact classes (one formatter, one line each):
 *
 *   aparty_schema      v                              schema version marker
 *   aparty_session     state sgen lgen host           state + generation snapshot
 *   aparty_roster      n mask c<seat>...              seat -> character identity set
 *   aparty_binding     seat port                      seat -> controller port
 *   aparty_transition  seat tick trigger dest lgen    the latched shared action
 *   aparty_interaction seat action verdict            shared-action arbitration result
 *   aparty_award       op sgen lgen course activity kind result   token issue/consume
 *   aparty_layout      viewports layout               viewport count / layout id
 *   aparty_restore     suspend_lgen restore_lgen match  suspend/restore + roster verdict
 *
 * Enum-valued keys (action, verdict, activity, kind, trigger, dest, result,
 * layout) are emitted as their raw integer values so the schema never depends
 * on a name table that could drift; `state` is the one exception and uses
 * adventure_party_state_name() because a session state has a canonical name
 * the state module already owns.
 *
 * SCHEMA BUMP RULES. ADVENTURE_PARTY_TRACE_SCHEMA_VERSION is the single source
 * of truth and the aparty_schema line reports it.
 *   - Bump the version for ANY breaking change: renaming/removing a key,
 *     reordering keys within a line, changing a value's meaning or encoding, or
 *     removing a fact class.
 *   - Adding a brand-new fact class (a new tag) that never appears on an
 *     existing line is additive and does NOT require a bump.
 *   - Every bump updates this comment, the golden file, and both parsers in
 *     the same commit, or one of the two golden tests fails — which is the
 *     point.
 * ---------------------------------------------------------------------------
 */
#ifndef MDKR64_ADVENTURE_PARTY_TRACE_H
#define MDKR64_ADVENTURE_PARTY_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "adventure_party/adventure_party_policy.h"
#include "adventure_party/adventure_party_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The schema version reported by aparty_schema. See the bump rules above. */
#define ADVENTURE_PARTY_TRACE_SCHEMA_VERSION 1

/* Big enough for the longest line a formatter can produce (a four-seat roster).
 * The _emit wrappers size their stack buffer with this. */
#define ADVENTURE_PARTY_TRACE_LINE_MAX 160

/* Which side of an exact-once token the award line is reporting. */
typedef enum AdventurePartyTraceAwardOp {
    ADVENTURE_PARTY_TRACE_AWARD_ISSUE = 0,
    ADVENTURE_PARTY_TRACE_AWARD_CONSUME = 1
} AdventurePartyTraceAwardOp;

/*
 * Formatters. Each writes one line (NUL-terminated when cap > 0) into buf and
 * returns the snprintf-style length: the number of characters the full line
 * needs, excluding the NUL. A return >= cap means the line was truncated. None
 * of them read past the value struct they are handed, and none mutate it.
 */
int adventure_party_trace_format_schema(char *buf, size_t cap);
int adventure_party_trace_format_session(char *buf, size_t cap,
                                         const AdventurePartySession *session);
int adventure_party_trace_format_roster(char *buf, size_t cap,
                                        const AdventurePartyRoster *roster);
int adventure_party_trace_format_binding(char *buf, size_t cap, uint8_t seat,
                                         uint8_t controller_port);
int adventure_party_trace_format_transition(
    char *buf, size_t cap, const AdventurePartyTransitionRequest *winner);
int adventure_party_trace_format_interaction(char *buf, size_t cap,
                                             uint8_t seat,
                                             AdventurePartyActionKind action,
                                             AdventurePartyArbitration verdict);
int adventure_party_trace_format_award(char *buf, size_t cap,
                                       AdventurePartyTraceAwardOp op,
                                       const AdventurePartyCompletionToken *token,
                                       int result);
int adventure_party_trace_format_layout(char *buf, size_t cap,
                                        int viewport_count, int layout_id);
int adventure_party_trace_format_restore(char *buf, size_t cap,
                                         uint32_t suspend_generation,
                                         uint32_t restore_generation,
                                         int roster_match);

/*
 * Emit wrappers. Each formats the matching line and, only when
 * mdkr_trace_enabled(), hands it to MDKR_TRACE. They contain no logic beyond
 * that — everything decidable lives in the formatters above, so these need no
 * test of their own.
 */
void adventure_party_trace_emit_schema(void);
void adventure_party_trace_emit_session(const AdventurePartySession *session);
void adventure_party_trace_emit_roster(const AdventurePartyRoster *roster);
void adventure_party_trace_emit_binding(uint8_t seat, uint8_t controller_port);
void adventure_party_trace_emit_transition(
    const AdventurePartyTransitionRequest *winner);
void adventure_party_trace_emit_interaction(uint8_t seat,
                                            AdventurePartyActionKind action,
                                            AdventurePartyArbitration verdict);
void adventure_party_trace_emit_award(AdventurePartyTraceAwardOp op,
                                      const AdventurePartyCompletionToken *token,
                                      int result);
void adventure_party_trace_emit_layout(int viewport_count, int layout_id);
void adventure_party_trace_emit_restore(uint32_t suspend_generation,
                                        uint32_t restore_generation,
                                        int roster_match);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADVENTURE_PARTY_TRACE_H */
