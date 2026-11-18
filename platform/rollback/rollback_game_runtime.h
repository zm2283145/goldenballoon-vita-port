#ifndef MDKR_ROLLBACK_GAME_RUNTIME_H
#define MDKR_ROLLBACK_GAME_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "rollback_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Feature-off laboratory lifecycle. When MDKR_ROLLBACK_LAB=1, level_ready
 * builds and freezes the real-game registry, preallocates eight snapshots and
 * captures the tick-zero boundary. MDKR_ROLLBACK_LAB_RESIM adds an exact
 * four-tick replay proof; MDKR_ROLLBACK_LAB_MUTATION_CONTROL first requires a
 * changed P1 input replay to diverge. A requested lab returns false on any gap. */
bool mdkr_rollback_game_runtime_level_ready(void);
/* True only between a successful rollback level-ready boundary and level end.
 * Game-owned allocations that participate in authoritative object state use
 * this narrow query to select a snapshotted arena. */
bool mdkr_rollback_game_runtime_active(void);
/* True from launcher admission through level construction, before the registry
 * is frozen. Model/attachment allocations use it to enter the snapshotted
 * object subpool at birth instead of requiring permanent main-pool ranges. */
bool mdkr_rollback_game_runtime_network_active(void);
bool mdkr_rollback_game_runtime_requested(void);
/* Called once after host polling and before mode_game. In the delayed-input
 * laboratory it replaces polled pads with the canonical predicted input set. */
bool mdkr_rollback_game_runtime_prepare_tick(unsigned update_rate);
/* Latch a requested presentation-only level completion until the originating
 * authored tick's complete active input set has been received and reconciled. */
bool mdkr_rollback_game_runtime_transition_ready(bool requested);
/* Fail if an allocator-owned authority range was freed, resized or replaced.
 * Unrelated cache churn cannot move a live block and is intentionally ignored. */
bool mdkr_rollback_game_runtime_validate_boundary(unsigned update_rate);
/* Distinguish WHY the most recent validate_boundary() returned false. True only
 * when that false was a RECOVERABLE online-input starvation -- an online race
 * whose peer/bootstrap input for the boundary never arrived (the peer LOST at
 * race start or mid-race) -- and false for a genuine rollback INVARIANT violation
 * (allocation lifetime/coverage, snapshot capture, side-effect journal, tick
 * exhaustion) OR any offline runtime. Lets the engine tick loop route a peer
 * loss to a clean return-to-room while still aborting on real corruption. */
bool mdkr_rollback_game_runtime_online_input_recoverable(void);
/* Which recoverable class the verdict above was, so the session-end witness
 * can name it truthfully. True = the correction-replay belt's SIM-STATE
 * refusal (a paused / zero-rate / level-ending tick the replay cannot
 * lawfully re-run; the transport was healthy). False = peer/input starvation
 * at a boundary (the historical class). Meaningful only while
 * ..._online_input_recoverable() reports true. */
bool mdkr_rollback_game_runtime_online_refusal_was_sim_state(void);
/* Stable until the next prepare attempt or level end; intended for bounded
 * user-visible failure diagnostics, never gameplay policy. */
const char *mdkr_rollback_game_runtime_failure_reason(void);
/* The final host-I/O firewall. Ordinary play is unaffected. While a rollback
 * match is active, resimulation may never touch the host and progression data
 * may never be persisted from the match timeline. Call this at the platform
 * boundary, after validating arguments but before changing durable/external
 * state. A rejected write is intentionally reported to gameplay as consumed:
 * the online timeline must not retry it forever. */
bool mdkr_rollback_game_runtime_host_io_allowed(bool progression_write);
/* SFX presentation is host state, not rollback authority. A traced sound is
 * bound to its exact event identity here; the adapter starts it once, defers a
 * corrected preview until reconciliation, and cancels only the versioned
 * voice created by a vanished prediction. Returns true when rollback consumed
 * the request, including during resimulation. */
bool mdkr_rollback_game_runtime_sound_request(
    const MdkrRollbackAudioRequest *request);
/* Low-level SFX parameter/stop operations use this non-accounting firewall.
 * Replayed gameplay may rebuild authority but must not enqueue device work. */
bool mdkr_rollback_game_runtime_presentation_allowed(void);
uint8_t mdkr_rollback_game_runtime_prepared_confirmed_mask(void);
/* During resimulation, retain the corrected timeline's final SFX mutations
 * and apply them only after event reconciliation. Returns true when consumed. */
bool mdkr_rollback_game_runtime_defer_audio_command(
    const MdkrRollbackAudioCommand *command);
void mdkr_rollback_game_runtime_level_end(void);
/* Cumulative resimulated-tick count and wall time, for frame telemetry. */
void mdkr_rollback_game_runtime_resim_perf(uint64_t *samples,
                                           uint64_t *ns_total);

/* Peer state-digest witness (diagnostic only; never feeds the simulation). */
#define MDKR_ROLLBACK_DIGEST_RACERS 8u
#define MDKR_ROLLBACK_DIGEST_SLOTS 64u
/* No online correction can rewrite a tick once it is >= SNAPSHOT_SLOTS old. */
#define MDKR_ROLLBACK_DIGEST_FINAL_AGE 32u
typedef struct MdkrRollbackTickDigest {
    uint64_t semantic;
    uint32_t tick;
    uint32_t rng_seed;
    uint32_t rng_draws;
    uint32_t input; /* canonical inputs + update_rate applied this tick */
    uint32_t world;
    uint32_t racer[MDKR_ROLLBACK_DIGEST_RACERS];
    uint16_t object_count;
    uint8_t racer_count;
    uint8_t confirmed_mask; /* local diagnostic only; not compared */
} MdkrRollbackTickDigest;
/* True only for a tick that no rollback can rewrite any more. */
bool mdkr_rollback_game_runtime_final_digest(uint32_t tick,
                                             MdkrRollbackTickDigest *out);
uint32_t mdkr_rollback_game_runtime_validated_tick(void);

#ifdef __cplusplus
}
#endif
#endif
