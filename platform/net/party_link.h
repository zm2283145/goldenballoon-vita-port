/* Native online BETA live selection bridge.
 *
 * Two one-way process-global channels between the launcher (the online lobby
 * owner) and the visible engine (the native character/track screens):
 *
 *   FORWARD FEED  (launcher -> engine): a per-frame lobby SNAPSHOT the native
 *       screens render live. The launcher OWNS it (install / clear / publish);
 *       the engine READS the latest published snapshot.
 *   REVERSE FEED  (engine -> launcher): the local player's in-menu INTENT
 *       (hover / confirm / ready / back out / host-start). The engine PUBLISHES
 *       one intent per player action; the launcher POLLS it one-shot.
 *
 * Ownership + threading deliberately mirror net_roster_runtime.{h,c} (the
 * install-once launcher-owned publication) and online_race_results.{h,c} (the
 * one-shot engine->launcher poll). All calls are SINGLE-THREADED: the launcher
 * service callback and the engine loop alternate on ONE thread -- the launcher
 * interactive loop is suspended while the visible engine runs and vice versa --
 * so a plain process-global needs no lock, exactly the discipline
 * online_race_results documents. Install before the engine reads; clear only
 * after the engine session has returned.
 *
 * This header is dependency-free and safe to include ANYWHERE (engine or
 * launcher): the snapshot/intent records are pinned, fixed-size and
 * pointer-free, and the launcher-only projection helper below takes its lobby
 * inputs by forward-declared pointer so an engine TU that includes this header
 * pulls in no launcher headers. The translation unit (party_link.c) is compiled
 * ONLY under MDKR_ENABLE_ONLINE_BETA (the CMake gate), so the release engine
 * object never carries it.
 */
#ifndef MDKR_NET_PARTY_LINK_H
#define MDKR_NET_PARTY_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_PARTY_LINK_SEATS 4u
#define MDKR_PARTY_LINK_NAME_BYTES 24u

/* One racer seat in the forward feed. character_id / vehicle_id carry the
 * lobby's per-seat selection (0xFF == unselected, matching MDKR_ONLINE_NO_*). */
typedef struct MdkrPartyLinkSeat {
    uint8_t occupied;
    uint8_t is_local;    /* this endpoint owns the seat */
    uint8_t is_host;     /* the seat belongs to the room leader (crown) */
    uint8_t ready;
    uint8_t connected;
    uint8_t character_id;
    uint8_t vehicle_id;
    char name[MDKR_PARTY_LINK_NAME_BYTES]; /* NUL-terminated; empty when unknown */
} MdkrPartyLinkSeat;

/* Host's live cursor on the native screen (P2-T3 fills it; zero/invalid here). */
typedef struct MdkrPartyLinkHostCursor {
    uint8_t screen;
    int8_t x;
    int8_t y;
    int8_t row;
    uint8_t valid;
} MdkrPartyLinkHostCursor;

/* FORWARD FEED record: the launcher's per-frame lobby snapshot. Pinned,
 * fixed-size, pointer-free -- safe to copy byte-for-byte. `generation` is a
 * monotonic publish counter (see mdkr_party_link_publish); phase is the lobby
 * MdkrOnlinePhase cast to a byte; mode / configured_track / cup_id / race_index
 * / points / last_placements mirror the identically named MdkrOnlineLobby
 * fields (configured_track 0xFFFF == none, cup_id 0xFF == none). */
typedef struct MdkrPartyLinkSnapshot {
    uint32_t generation;
    uint8_t phase;
    MdkrPartyLinkSeat seats[MDKR_PARTY_LINK_SEATS];
    uint8_t mode;
    uint16_t configured_track;
    uint8_t cup_id;
    uint8_t race_index;
    uint16_t points[MDKR_PARTY_LINK_SEATS];
    uint8_t last_placements[MDKR_PARTY_LINK_SEATS];
    MdkrPartyLinkHostCursor host_cursor;
} MdkrPartyLinkSnapshot;

/* REVERSE FEED record: the local player's latest in-menu intent. start_requested
 * is the host pressing Start on the native screen. */
typedef struct MdkrPartyLinkLocalIntent {
    uint8_t hover_character;
    uint8_t confirmed;
    uint8_t ready;
    uint8_t backout;
    uint8_t start_requested;
} MdkrPartyLinkLocalIntent;

/* ---- Forward feed lifecycle (launcher only) ----------------------------- */

/* Install the bridge. Copy-based, install-once (mirrors
 * net_roster_runtime_install): returns false if already installed -- the owner
 * must clear() first. Resets the published snapshot and the reverse-feed poll
 * state. */
bool mdkr_party_link_install(void);
/* Tear the bridge down: drops the snapshot and the intent channel, and
 * mdkr_party_link_active() reports false afterwards. */
void mdkr_party_link_clear(void);
/* Engine-facing predicate: false when uninstalled. */
bool mdkr_party_link_active(void);

/* Launcher writes the latest snapshot. Monotonic generation: if the caller did
 * not advance snapshot->generation past the stored one, publish bumps it, so a
 * reader can always detect a new snapshot. No-op when uninstalled. */
void mdkr_party_link_publish(const MdkrPartyLinkSnapshot *snapshot);
/* Engine reads the latest published snapshot into *out. Returns false (leaving
 * *out untouched) when the bridge is inactive. */
bool mdkr_party_link_read(MdkrPartyLinkSnapshot *out);

/* ---- Reverse feed (engine publishes, launcher polls) -------------------- */

/* Engine publishes the local player's intent. Each publish re-arms the poll
 * below (a per-publish epoch, exactly like online_race_results). No-op when
 * uninstalled. Beta-gated callers arrive in P2-T2. */
void mdkr_party_link_intent_publish(const MdkrPartyLinkLocalIntent *intent);
/* Launcher one-shot poll: copies the recorded intent into *out and returns true
 * exactly once per publish (an already-read intent -- or none -- is never handed
 * out again), then clears the arm. Returns false when inactive. */
bool mdkr_party_link_intent_poll(MdkrPartyLinkLocalIntent *out);

/* ---- Launcher-side projection helper ------------------------------------ *
 *
 * Pure field mapping: project a launcher lobby snapshot + view model into the
 * pinned forward-feed record. It DOES NOT publish (the caller does) and DOES
 * NOT set out->generation (mdkr_party_link_publish owns monotonicity -- the
 * field is zeroed here). host_cursor is left zero/invalid (P2-T3 fills it).
 *
 * `local_endpoint_id` selects the local seat(s): pass the endpoint id when the
 * caller knows it, or 0 to fall back to view->local_member_is_leader (correct
 * for the fenced 2-endpoint beta -- the single occupied non-leader endpoint is
 * "local" when this endpoint is not the leader). `view` may be NULL (then only
 * an explicit nonzero local_endpoint_id marks local seats). Never dereferences
 * a pointer FIELD of the inputs, only the structs themselves.
 *
 * Declared with forward-declared struct tags so this header stays dependency-
 * free; party_link.c includes the launcher lobby headers to implement it, and
 * an engine TU that never calls it needs neither. */
struct MdkrOnlineViewModel;
struct MdkrOnlineLobby;
void mdkr_party_link_snapshot_from_lobby(
    MdkrPartyLinkSnapshot *out,
    const struct MdkrOnlineViewModel *view,
    const struct MdkrOnlineLobby *lobby,
    uint64_t local_endpoint_id);

#ifdef __cplusplus
}
#endif
#endif
