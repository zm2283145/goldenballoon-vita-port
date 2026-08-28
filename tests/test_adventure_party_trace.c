/* test_adventure_party_trace.c — AP-05 trace schema contract (C half).
 *
 * ROM-free: links only the trace module and the state module (whose
 * adventure_party_state_name() the session line uses). The load-bearing test
 * is the cross-language golden contract: this file formats one exemplar line
 * per fact class with fixed inputs and compares them BYTE-EXACT against
 * tests/data/adventure_party_trace_golden.txt. tests/adventure_party_trace.py
 * parses the SAME file, so a drift in either language fails one of the two.
 *
 * mdkr_trace_enabled()/mdkr_trace() are stubbed here (the house pattern, see
 * tests/test_ghost_bank.c): the formatters need no trace plumbing, and the
 * _emit wrappers — the only users of those symbols — are never exercised. */
#include "adventure_party/adventure_party_trace.h"

#include <stdio.h>
#include <string.h>

/* Trace plumbing stubs: present only so the _emit wrappers in the module link.
 * A formatter test never calls them. */
int mdkr_trace_enabled(void) { return 0; }
void mdkr_trace(const char *fmt, ...) { (void)fmt; }

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* Builds each exemplar into out[] and returns the count. The inputs here ARE
 * the golden file's inputs; changing one means regenerating the golden. */
#define EXEMPLAR_COUNT 9
#define LINE_CAP ADVENTURE_PARTY_TRACE_LINE_MAX

static void build_exemplars(char out[EXEMPLAR_COUNT][LINE_CAP],
                            int lens[EXEMPLAR_COUNT]) {
    AdventurePartySession session;
    AdventurePartyRoster roster;
    AdventurePartyTransitionRequest winner;
    AdventurePartyCompletionToken token;
    int i = 0;

    /* schema */
    lens[i] = adventure_party_trace_format_schema(out[i], LINE_CAP); i++;

    /* session: ACTIVE_LOBBY, session gen 1, level gen 3, host seat 0 */
    memset(&session, 0, sizeof session);
    session.state = ADVENTURE_PARTY_STATE_ACTIVE_LOBBY;
    session.session_generation = 1;
    session.level_generation = 3;
    session.host_seat = 0;
    lens[i] = adventure_party_trace_format_session(out[i], LINE_CAP, &session);
    i++;

    /* roster: three seats, characters 10/11/12 */
    memset(&roster, 0, sizeof roster);
    roster.participant_count = 3;
    roster.seat_mask = 0x7;
    roster.character_by_seat[0] = 10;
    roster.character_by_seat[1] = 11;
    roster.character_by_seat[2] = 12;
    lens[i] = adventure_party_trace_format_roster(out[i], LINE_CAP, &roster);
    i++;

    /* binding: seat 2 on controller port 3 */
    lens[i] = adventure_party_trace_format_binding(out[i], LINE_CAP, 2, 3); i++;

    /* transition: seat 1, tick 42, trigger 7, dest 9, level gen 3 */
    memset(&winner, 0, sizeof winner);
    winner.level_generation = 3;
    winner.simulation_tick = 42;
    winner.trigger_kind = 7;
    winner.destination = 9;
    winner.initiating_seat = 1;
    lens[i] = adventure_party_trace_format_transition(out[i], LINE_CAP, &winner);
    i++;

    /* interaction: seat 0, trigger-transition action, latched verdict */
    lens[i] = adventure_party_trace_format_interaction(
        out[i], LINE_CAP, 0, ADVENTURE_PARTY_ACTION_TRIGGER_TRANSITION,
        ADVENTURE_PARTY_ARBITRATE_LATCHED);
    i++;

    /* award: consume of a course token, key sgen 1 / lgen 3 / course 5 /
     * activity 2 / kind 0, result OK (0) */
    memset(&token, 0, sizeof token);
    token.session_generation = 1;
    token.level_generation = 3;
    token.course = 5;
    token.activity = 2;
    token.completion_kind = 0;
    lens[i] = adventure_party_trace_format_award(
        out[i], LINE_CAP, ADVENTURE_PARTY_TRACE_AWARD_CONSUME, &token, 0);
    i++;

    /* layout: three viewports, layout id 1 */
    lens[i] = adventure_party_trace_format_layout(out[i], LINE_CAP, 3, 1); i++;

    /* restore: suspend gen 2, restore gen 4, roster matched */
    lens[i] = adventure_party_trace_format_restore(out[i], LINE_CAP, 2, 4, 1);
    i++;
}

/* Reads non-empty lines from the golden file into golden[]; returns count or
 * -1 if the file could not be opened. */
static int read_golden(const char *path, char golden[EXEMPLAR_COUNT][LINE_CAP]) {
    FILE *f = fopen(path, "rb");
    char raw[512];
    int n = 0;
    if (!f) return -1;
    while (n < EXEMPLAR_COUNT && fgets(raw, sizeof raw, f)) {
        size_t len = strlen(raw);
        while (len && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
            raw[--len] = '\0';
        if (len == 0) continue;
        snprintf(golden[n], LINE_CAP, "%s", raw);
        n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *golden_path = (argc > 1)
        ? argv[1]
        : "tests/data/adventure_party_trace_golden.txt";
    char lines[EXEMPLAR_COUNT][LINE_CAP];
    int lens[EXEMPLAR_COUNT];
    char golden[EXEMPLAR_COUNT][LINE_CAP];
    int gcount, i;
    char small[4];
    int need;

    /* The header comment is the schema authority; the marker must agree. */
    expect(ADVENTURE_PARTY_TRACE_SCHEMA_VERSION == 1,
           "schema version constant is 1");

    build_exemplars(lines, lens);

    /* snprintf semantics: return value equals the written length. */
    for (i = 0; i < EXEMPLAR_COUNT; i++)
        expect(lens[i] == (int)strlen(lines[i]),
               "formatter returns snprintf-style length");

    /* Truncation is safe: a too-small buffer reports the full length and never
     * overflows (schema line is 5+ chars, cap is 4). */
    need = adventure_party_trace_format_schema(small, sizeof small);
    expect(need >= (int)sizeof small, "truncation reports full length");
    expect(small[sizeof small - 1] == '\0', "truncation stays NUL-terminated");

    gcount = read_golden(golden_path, golden);
    expect(gcount >= 0, "golden file opened");
    expect(gcount == EXEMPLAR_COUNT, "golden has one line per fact class");

    if (gcount == EXEMPLAR_COUNT) {
        for (i = 0; i < EXEMPLAR_COUNT; i++) {
            int ok = strcmp(lines[i], golden[i]) == 0;
            if (!ok)
                printf("     got:    %s\n     golden: %s\n", lines[i], golden[i]);
            expect(ok, "exemplar matches golden line byte-exact");
        }
    }

    if (failures) {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nall trace schema checks passed\n");
    return 0;
}
