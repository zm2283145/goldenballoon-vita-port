// engine_entry.h — the C entry points and seams the app shell delegates to.
//
// This is the ONLY header shared between the C++ app shell and the C engine, so
// the two never need hand-synced duplicate declarations. mdkr64_headless_main()
// is the original main() body from platform/main_pc.c (renamed under -DMDKR_APP);
// the shell calls it verbatim for every non-interactive invocation so that path
// stays byte-identical to the pre-shell binary.
#ifndef MDKR64_ENGINE_ENTRY_H
#define MDKR64_ENGINE_ENTRY_H

/* Canonical C handoff/recovery seam. Keep these declarations in one header so
 * the C engine and C++ shell cannot drift. */
#include "../host_window.h"

#ifdef __cplusplus
extern "C" {
#endif

// The engine's original entry point (platform/main_pc.c under MDKR_APP).
int mdkr64_headless_main(int argc, char **argv);

// --- Launcher -> game boot -------------------------------------------------
// DKR ADAPTATION: mgb64's MgbBootConfig carries level/difficulty/multiplayer/
// players/mp_stage — GoldenEye's mission-select surface. DKR's engine has no
// equivalent CLI (platform/main_pc.c models no level or difficulty flags; the
// player picks a world in the game's own adventure hub), so those fields are
// deliberately absent rather than present-and-ignored. What the launcher CAN
// meaningfully choose is the ROM and the presentation mode, which is what this
// carries.
#define MDKR_BOOT_MAX_OVERRIDES 16

typedef enum {
    MDKR_CHARACTER_PREVIEW_NONE = 0,
    MDKR_CHARACTER_PREVIEW_SELECT,
    MDKR_CHARACTER_PREVIEW_CAR,
    MDKR_CHARACTER_PREVIEW_HOVERCRAFT,
    MDKR_CHARACTER_PREVIEW_PLANE,
} MdkrCharacterPreviewContext;

// Measured evidence returned by an exact Character Workshop session. Interval
// values describe displayed wall cadence after a 120-authored-tick warm-up;
// they are not GPU timestamp queries. A short session can legitimately return
// warmup_complete=0 or fewer than 60 interval samples.
typedef struct MdkrCharacterPreviewResult {
    unsigned version;
    int started;
    int warmup_complete;
    int realtime;
    MdkrCharacterPreviewContext context;
    int players;
    unsigned long long warmup_ticks;
    unsigned long long interval_samples;
    unsigned long long displayed_frames;
    unsigned long long interval_p50_us;
    unsigned long long interval_p95_us;
    unsigned long long interval_p99_us;
    unsigned long long interval_mean_us;
    unsigned long long interval_max_us;
    unsigned long long tickwall_samples;
    unsigned long long tickwall_mean_ns;
    unsigned long long replacement_draws;
    unsigned long long replacement_primitives;
    unsigned long long hidden_donor_batches;
    unsigned long long contact_solves;
    unsigned long long contact_error_mean_micrometres;
    unsigned long long contact_error_max_micrometres;
} MdkrCharacterPreviewResult;

#define MDKR_CHARACTER_PREVIEW_RESULT_VERSION 2u

// Owned by the C engine entry module and non-NULL only during a launcher-owned
// preview boot. The game writes through it before engine teardown resets the
// underlying bounded counters.
extern MdkrCharacterPreviewResult *g_mdkrCharacterPreviewResult;

typedef struct {
    const char *rom_path;      // NULL/empty => engine default (baserom.us.v80.z64)
    int   video_mode;          // MdkrVideoMode, or -1 for "don't pass a preset"
    int   window_width;        // <= 0 => engine default
    int   window_height;
    int   automation_ticks;    // <= 0 => interactive; launcher regression seam
    int   automation_frames;   // mutually exclusive presentation-frame seam
    const char *input_script;  // automation-only deterministic controller fixture
    // One-shot Character Workshop route. The engine enters the exact authored
    // select or race context without menu-navigation scripts; zero disables it.
    const char *character_preview_package;
    MdkrCharacterPreviewContext character_preview_context;
    int character_preview_players;  // 1..4
    MdkrCharacterPreviewResult *character_preview_result;
    // Staged RESTART-scope settings, as "Video.Key=Value" strings. The settings
    // panel writes these when the player changes a restart-scope key before
    // pressing Play, so the choice takes effect on THIS boot rather than
    // silently waiting for the next one.
    const char *overrides[MDKR_BOOT_MAX_OVERRIDES];
    int   override_count;
} MdkrBootConfig;

// Boot the engine with the given config. Blocks until the game exits; returns
// the engine's exit code. Implemented by synthesizing a CLI invocation and
// delegating to mdkr64_headless_main(), so the launcher shares the exact engine
// boot path rather than a parallel one that could drift.
int mdkr64_engine_boot(const MdkrBootConfig *cfg);

// --- Fatal-crash write mirror (Windows) ------------------------------------
// DiagLog_install() tees stdout/stderr through a pipe drained by a reader
// thread. That thread dies with the process, so a crash-time write to stderr
// can be lost or block forever on a full pipe — exactly when the diagnostic
// matters most. The tee therefore publishes the PRE-tee console fd and the raw
// mdkr64.log fd here, and the engine's crash handler writes to them directly,
// bypassing the pipe. -1 means the tee is not installed (every CLI invocation),
// in which case stderr is already the real console and needs no bypass.
// Defined in platform/main_pc.c.
extern int g_diagLogRealErrFd;
extern int g_diagLogFileFd;

// --- Crash-screen presentation hook ----------------------------------------
// platform/main_pc.c's SIGSEGV/SIGBUS handler calls this AFTER it has written
// and flushed `[CRASH]` and the backtrace, and before it restores the default
// disposition and re-raises. The app shell registers
// platform/app/crash_screen.cpp here; NULL (every CLI invocation) means the
// handler behaves exactly as it did before the crash screen existed.
//
// Declared here rather than duplicated ad hoc for the reason this header
// exists: the engine defines it, the shell assigns it, and neither carries its
// own copy of the type. Defined in platform/main_pc.c.
extern void (*g_mdkrCrashScreenHook)(int signo);

// --- In-game overlay hooks (platform/app_overlay_hooks.c) ------------------
// The app registers these before boot; the engine's event pump and frame-end
// call them. Keeps the C engine free of any ImGui/C++ dependency.
//
// The pointer types are named, and named HERE, so that they pick up this
// header's C language linkage. The struct itself must stay inside the extern "C"
// block -- platform/app_overlay_hooks.c and tests/test_app_overlay_hooks.c are
// real C consumers -- and a C-linkage member can only legally be assigned a
// function that also has C linkage. Every implementation on the C++ side is
// therefore defined inside an `extern "C" { static ... }` block (internal
// linkage, C function type); see platform/app/ui_overlay.cpp.
typedef int  (*AppOverlayProcessEventFn)(const void *sdl_event);
typedef void (*AppOverlayServiceFn)(void);
typedef int  (*AppOverlayQueryFn)(void);
typedef int  (*AppOverlayRenderFn)(void);

typedef struct {
    /* Nonzero means the host shortcut consumed this event before game input. */
    AppOverlayProcessEventFn process_event;
    /* Called at the event-pump boundary before a new frame begins. */
    AppOverlayServiceFn      service;
    AppOverlayQueryFn        wants_input;
    /* Input capture and simulation pause are deliberately independent. An
     * online Party overlay consumes navigation input without stopping one
     * endpoint's authoritative clock. */
    AppOverlayQueryFn        wants_pause;
    AppOverlayQueryFn        wants_render;
    AppOverlayRenderFn       render;  // zero reports a fatal overlay-render failure
} AppOverlayHooks;
void platformSetOverlayHooks(const AppOverlayHooks *hooks);

// Apply the game's authored pause mix while the app-owned F1 overlay is open.
// The overlay layer composes with the game's latest authored volume mode; menu
// work may update that underlying mode while paused without removing the duck.
// Audio synthesis continues, so sequencing and the host sink remain continuous.
void mdkr64_engine_overlay_audio_pause(void);
void mdkr64_engine_overlay_audio_resume(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ENGINE_ENTRY_H
