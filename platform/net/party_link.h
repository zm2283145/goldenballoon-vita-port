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

/* "Unset" sentinels for the host-only session-config intent fields below. They
 * are deliberately NONZERO so a memset(0)'d intent reads as "host wants nothing"
 * (mode 0 is a REAL mode value, so a zero could not double as unset). A screen
 * that is not the host -- or a host screen that has not chosen yet -- publishes
 * these sentinels, and the dispatch planner then never wants the host kinds. */
#define MDKR_PARTY_LINK_MODE_UNSET 0xFFu   /* intent.mode: no SET_MODE wanted */
#define MDKR_PARTY_LINK_TRACK_UNSET 0xFFFFu /* intent.config_track: no SET_CONFIG_TRACK */
#define MDKR_PARTY_LINK_CUP_UNSET 0xFFu    /* intent.cup_id: no SET_CUP wanted */

/* REVERSE FEED record: the local player's latest in-menu intent. The native
 * screen is the source of truth for the pick, so it carries BOTH the racer and
 * the vehicle -- the reducer refuses READY until a vehicle is set, so a bridge
 * that could not express a vehicle could never legally ready a fresh seat.
 * vehicle_id == 0xFF (MDKR_ONLINE_NO_VEHICLE) means "unset" (no CHOOSE_VEHICLE).
 * start_requested is the host pressing Start on the native screen. The launcher
 * dispatches, per intent, CHOOSE_CHARACTER, then CHOOSE_VEHICLE (before READY,
 * so the vehicle lands first), then READY / START_RACE.
 *
 * The three session-config fields (mode / config_track / cup_id) are HOST-ONLY
 * (the native TRACK/CUP select screen, PD-T3): the host publishes them, a joiner
 * always leaves them at the UNSET sentinels above. config_track keeps the u16
 * width the lobby/snapshot use, but the dispatch ACTION.value is a u8 -- fine,
 * because every one of the 20 standard race track ids is <= 33 (a documented
 * ceiling; the reducer's known_race_track() rejects anything else). */
typedef struct MdkrPartyLinkLocalIntent {
    uint8_t hover_character;
    uint8_t vehicle_id;      /* chosen vehicle, or 0xFF (NO_VEHICLE) == unset */
    uint8_t confirmed;
    uint8_t ready;
    uint8_t backout;
    uint8_t start_requested;
    uint8_t mode;            /* host: single/tournament, or MODE_UNSET (0xFF) */
    uint16_t config_track;   /* host single-race: track id, or TRACK_UNSET */
    uint8_t cup_id;          /* host tournament: cup 0..4, or CUP_UNSET (0xFF) */
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

/* ---- Reverse-feed dispatch plan (pure, launcher-side) ------------------- *
 *
 * The dedupe/ordering logic is held OUT of the beta wiring so it is directly
 * unit-testable -- the same rationale as mdkr_net_roster_guard_decides_clear
 * (net_roster_runtime.h).
 *
 * DEDUPE IS AGAINST THE AUTHORITATIVE LOBBY SNAPSHOT, never a submit's return.
 * On the live adapter a submit's "accepted" is only the OPTIMISTIC transport
 * SEND result (the reduction, and any SELECTION_CONFLICT / ILLEGAL_VEHICLE
 * refusal, arrives later out-of-band); latching on it would swallow a command
 * that was sent, optimistically "accepted", then async-refused -- and never
 * re-send it once the conflict clears. So a command is DONE only once the local
 * seat's lobby value equals the intent (CONVERGENCE), which pumpPartyLink reads
 * from the very same snapshot surface each frame. Convergence per kind:
 *   CHOOSE_CHARACTER -> local character_id == hover_character
 *   CHOOSE_VEHICLE   -> local vehicle_id   == vehicle_id
 *   READY            -> local ready == 1;  CHANGE_SELECTION -> local ready == 0
 *   START_RACE       -> lobby phase left MDKR_ONLINE_LOBBY
 *   SET_MODE         -> lobby mode == intent.mode            (host only)
 *   SET_CONFIG_TRACK -> lobby configured_track == config_track (host only)
 *   SET_CUP          -> lobby cup_id == intent.cup_id        (host only)
 * The three session-config kinds are HOST-ONLY: the planner wants them only when
 * the local seat is the leader AND the intent carries a non-sentinel value, so a
 * joiner (which always publishes the UNSET sentinels) never plans them. They are
 * ordered BEFORE ready/start (see kOrder[]) because the reducer clears EVERY
 * member's ready on any mode/track/cup change; landing the config first and then
 * re-asserting ready in the SAME plan keeps the seat converged to all-ready.
 *
 * To avoid re-sending every frame while a command genuinely propagates, an
 * in-flight guard suppresses re-sending the SAME (kind,value) after a send,
 * UNTIL one of: the snapshot converges (done -- stop), a refusal is observed
 * for that kind (mdkr_party_link_dispatch_note_refusal -> re-fire promptly), or
 * a bounded number of pumps without convergence (a safety-net re-fire). The
 * synchronous fake adapter reduces immediately, so it converges on the next
 * pump and sends each command exactly once. These types name only party_link
 * values (no MdkrOnlineViewAction), so the header stays dependency-free; the
 * wiring maps each kind to the EXISTING view action, fills the START_RACE
 * vehicle mask (which needs the live lobby), submits IN ORDER, and marks each
 * in-flight only on a successful SEND. */
typedef enum MdkrPartyLinkDispatchKind {
    MDKR_PARTY_LINK_DISPATCH_NONE = 0,
    MDKR_PARTY_LINK_DISPATCH_CHOOSE_CHARACTER,
    MDKR_PARTY_LINK_DISPATCH_CHOOSE_VEHICLE,
    MDKR_PARTY_LINK_DISPATCH_CHANGE_SELECTION,
    MDKR_PARTY_LINK_DISPATCH_READY,
    MDKR_PARTY_LINK_DISPATCH_START_RACE,
    /* Host-only session config (PD-T3). Appended AFTER the existing kinds so the
     * pre-existing enum values are unchanged; the processing order is set by
     * kOrder[] in party_link.c (config before ready/start), not by this order. */
    MDKR_PARTY_LINK_DISPATCH_SET_MODE,
    MDKR_PARTY_LINK_DISPATCH_SET_CONFIG_TRACK,
    MDKR_PARTY_LINK_DISPATCH_SET_CUP,
    MDKR_PARTY_LINK_DISPATCH_KIND_COUNT
} MdkrPartyLinkDispatchKind;

typedef struct MdkrPartyLinkDispatchAction {
    uint8_t kind;  /* MdkrPartyLinkDispatchKind */
    uint8_t value; /* character/vehicle/mode/cup id or track id (all <= 33);
                    * START_RACE mask is filled by wiring */
} MdkrPartyLinkDispatchAction;

/* One plan may carry every dispatchable kind at once (KIND_COUNT - 1). */
#define MDKR_PARTY_LINK_MAX_DISPATCH 8u
typedef struct MdkrPartyLinkDispatchPlan {
    MdkrPartyLinkDispatchAction actions[MDKR_PARTY_LINK_MAX_DISPATCH];
    uint8_t count;
} MdkrPartyLinkDispatchPlan;

/* Current lobby values for the LOCAL seat -- the authoritative convergence
 * signal. Built by pumpPartyLink from the published snapshot. When no local
 * seat is resolvable (have_seat == 0) nothing is treated as converged, so the
 * command is (re-)sent under the in-flight guard. */
typedef struct MdkrPartyLinkLocalView {
    uint8_t have_seat;    /* a local seat was resolved in the snapshot */
    uint8_t is_host;      /* the local seat is the room leader (host-only kinds) */
    uint8_t character_id; /* local seat's current lobby character (0xFF none) */
    uint8_t vehicle_id;   /* local seat's current lobby vehicle (0xFF none) */
    uint8_t ready;        /* local seat's current lobby ready flag */
    uint8_t phase;        /* lobby phase (MdkrOnlinePhase) */
    uint8_t mode;         /* lobby mode (single/tournament) -- SET_MODE converge */
    uint16_t configured_track; /* lobby configured_track -- SET_CONFIG_TRACK converge */
    uint8_t cup_id;       /* lobby cup_id -- SET_CUP converge */
} MdkrPartyLinkLocalView;

/* Safety-net re-fire: pumps a still-un-converged in-flight command waits before
 * re-sending when no refusal is observed (~3 s at 30 Hz). Refusal re-fires
 * promptly; this only bounds a silently dropped command. */
#define MDKR_PARTY_LINK_INFLIGHT_MAX_PUMPS 90u

/* Per-kind in-flight guard, indexed by MdkrPartyLinkDispatchKind. */
typedef struct MdkrPartyLinkDispatchState {
    uint8_t inflight_active[MDKR_PARTY_LINK_DISPATCH_KIND_COUNT];
    uint8_t inflight_value[MDKR_PARTY_LINK_DISPATCH_KIND_COUNT];
    uint16_t inflight_age[MDKR_PARTY_LINK_DISPATCH_KIND_COUNT];
} MdkrPartyLinkDispatchState;

/* Reset the dedupe state (call on install/clear, before the first intent). */
void mdkr_party_link_dispatch_state_reset(MdkrPartyLinkDispatchState *state);

/* Plan the ORDERED reducer commands for one polled intent against the current
 * lobby-local values. Order is CHOOSE_CHARACTER, CHOOSE_VEHICLE,
 * CHANGE_SELECTION, READY, START_RACE -- vehicle BEFORE ready so a fresh seat's
 * vehicle lands first. A command is emitted only when the local seat is NOT yet
 * converged to the intent AND the in-flight guard permits (not already sent this
 * (kind,value), or the safety-net bound elapsed). Convergence auto-clears the
 * guard. `state` is mutated (guard aging / convergence clears). `local` may be
 * NULL (treated as no local seat). NULL state/intent/out -> empty plan. */
void mdkr_party_link_plan_dispatch(MdkrPartyLinkDispatchState *state,
                                   const MdkrPartyLinkLocalIntent *intent,
                                   const MdkrPartyLinkLocalView *local,
                                   MdkrPartyLinkDispatchPlan *out);

/* Mark an action in-flight after a SUCCESSFUL SEND (the transport accepted the
 * submit -- NOT a reduction). Suppresses re-sending the same (kind,value) until
 * convergence, a refusal, or the safety-net bound. No-op for NULL args. */
void mdkr_party_link_dispatch_mark_sent(MdkrPartyLinkDispatchState *state,
                                        const MdkrPartyLinkDispatchAction *action);

/* Clear the in-flight guard for `kind` after a refusal was observed on the
 * adapter's refusal surface, so the same intent re-fires on the next pump
 * (instead of waiting out the safety-net bound). No-op for NULL/out-of-range. */
void mdkr_party_link_dispatch_note_refusal(MdkrPartyLinkDispatchState *state,
                                           uint8_t kind);

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
