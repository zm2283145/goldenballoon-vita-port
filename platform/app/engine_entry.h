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
#include "../modern_character_semantics.h"
#include "../workshop_preview_runtime.h"

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

/* A pose inspector is a presentation-only exact-renderer request. LIVE keeps
 * ordinary game-driven animation and is the only mode eligible for durable
 * performance evidence. Every other value holds the chosen semantic at the
 * requested normalized phase without changing racer or vehicle logic. */
typedef enum {
    MDKR_CHARACTER_PREVIEW_POSE_LIVE = 0,
#define MDKR_CHARACTER_PREVIEW_POSE_ENUM(suffix, semantic, label) \
    MDKR_CHARACTER_PREVIEW_POSE_##suffix,
    MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
        MDKR_CHARACTER_PREVIEW_POSE_ENUM)
#undef MDKR_CHARACTER_PREVIEW_POSE_ENUM
    MDKR_CHARACTER_PREVIEW_POSE_COUNT,
} MdkrCharacterPreviewPose;

/* One-shot inspection captures are explicit render products. SCENE preserves
 * the ordinary composed gameplay frame. MODEL_ALPHA asks the modern-character
 * backend to replay only validated replacement draws into a transparent
 * target; it never hides world geometry in the visible frame. */
typedef enum {
    MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE = 0,
    MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA,
    MDKR_CHARACTER_PREVIEW_CAPTURE_COUNT,
} MdkrCharacterPreviewCaptureKind;

#define MDKR_CHARACTER_PREVIEW_CONTACTS 4u

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
    MdkrCharacterPreviewPose pose;
    unsigned pose_phase_milli;
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
    /* Latest successful post-solve vehicle draw. Stable contact order is
     * left hand, right hand, left foot, right foot. Every point is in the
     * donor target frame and every valid bit covers one complete witness. */
    unsigned contact_witness_mask;
    long long contact_chain_root_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_bend_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_target_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_end_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    unsigned long long contact_witness_error_micrometres
        [MDKR_CHARACTER_PREVIEW_CONTACTS];
    /* Latest successful replacement draw, expressed in its donor target
     * frame. Signed micrometres retain sub-millimetre fit evidence without
     * exposing host float representation across the C/C++ app boundary. */
    int fit_diagnostics_valid;
    long long fit_bounds_min_micrometres[3];
    long long fit_bounds_max_micrometres[3];
    long long fit_anchor_micrometres[3];
    int fit_forward_milli[3];
    unsigned long long inspection_pose_ticks;
    unsigned long long inspection_pose_fallback_ticks;
    int view_yaw_degrees;
    int view_pitch_degrees;
    MdkrWorkshopPreviewLighting lighting;
    unsigned long long camera_override_ticks;
    unsigned long long lighting_override_draws;
    int capture_requested;
    MdkrCharacterPreviewCaptureKind capture_kind;
    int capture_armed;
    unsigned long long capture_stable_frames;
    int capture_written;
    unsigned long long capture_png_bytes;
    /* Exact comparison environment captured inside the engine session. Text
     * comes from the bounded GPU diagnostic record; dimensions distinguish
     * output resolution from RenderScale's actual scene resolution. */
    char renderer_backend[32];
    char adapter[192];
    char driver[192];
    unsigned vendor_id;
    unsigned device_id;
    unsigned output_width;
    unsigned output_height;
    unsigned render_width;
    unsigned render_height;
} MdkrCharacterPreviewResult;

#define MDKR_CHARACTER_PREVIEW_RESULT_VERSION 10u
#define MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES 12u

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
    MdkrCharacterPreviewPose character_preview_pose;
    unsigned character_preview_pose_phase_milli;  // 0..1000
    int character_preview_view_yaw_degrees;       // -180..180
    int character_preview_view_pitch_degrees;     // -45..45
    MdkrWorkshopPreviewLighting character_preview_lighting;
    const char *character_preview_capture_png;    // optional, create-only
    MdkrCharacterPreviewCaptureKind character_preview_capture_kind;
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
