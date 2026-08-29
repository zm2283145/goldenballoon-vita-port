// ui_settings.h — settings panel generated from the video/gameplay schema.
//
// DKR ADAPTATION. mgb64's panel enumerates its own engine config registry
// (mgb_config_*) and opens an explicit staging session in the overlay, because
// that registry writes straight into live globals and needs Apply/Cancel to be
// safe. mdkr64 already has that transaction built into the config layer:
// mdkr_video_config_runtime_set() validates, persists, and then publishes only
// the LIVE-scope half, leaving RESTART-scope values staged in `desired` for the
// next launch. So this panel drives that API directly instead of layering a
// second staging model on top of it — every edit is already all-or-nothing.
#ifndef MDKR64_UI_SETTINGS_H
#define MDKR64_UI_SETTINGS_H

#include <string>

#include "engine_entry.h"
#include "modern_character_gameplay_profile.h"

struct SDL_Window;

struct SettingsCharacterPreviewRequest {
    std::string packageId;
    std::string sourceSha256;
    std::string fitSha256;
    std::string presentationSha256;
    MdkrCharacterPreviewContext context = MDKR_CHARACTER_PREVIEW_SELECT;
    MdkrCharacterPreviewScene scene =
        MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
    int players = 1;
    MdkrCharacterPreviewPose pose = MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned posePhaseMilli = 0u;
    MdkrCharacterPreviewPose transitionFromPose =
        MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned transitionFromPhaseMilli = 0u;
    int viewYawDegrees = 0;
    int viewPitchDegrees = 0;
    MdkrWorkshopPreviewLighting lighting =
        MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    std::string capturePng;
    MdkrCharacterPreviewCaptureKind captureKind =
        MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
    bool autoReturnAfterCapture = false;
    bool launcherOwnedCapture = false;
    bool portraitSourceHandoff = false;
    bool interactiveStudio = false;
    bool representativeMotionReview = false;
    bool donorReference = false;
};

struct SettingsCharacterPreviewDisposition {
    bool launcherOwnedCapture = false;
    bool portraitSourceHandoff = false;
    bool interactiveStudio = false;
    bool representativeMotionReview = false;
    bool donorReference = false;
    MdkrCharacterPreviewScene scene =
        MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
};

struct SettingsCharacterStudioFrame {
    bool fitReady = false;
    bool measurementReady = false;
    bool returnRequested = false;
};

// Draw the settings sections (one per MdkrVideoCategory) inside the current
// content region. Shared verbatim by the launcher and the in-game F1 overlay;
// the only difference is `compact`, which drops the per-key help text so the
// overlay panel stays readable over a running race.
//
// Returns true when any setting was changed this frame (the caller may want to
// re-read live state).
bool Settings_draw(SDL_Window *window, bool compact = false);

// Draw the complete Character Workshop in its launcher-owned destination.
// General Settings intentionally exposes only a shortcut and assignment
// summary; keeping authoring here prevents a growing inspector from consuming
// the ordinary settings hierarchy. The in-game overlay must not call this.
bool Settings_drawCharacterWorkshop(SDL_Window *window,
                                    bool compact = false);

// One-shot request raised by the Settings shortcut. The launcher owns panel
// routing, so the shared settings module never reaches into LauncherState.
bool Settings_takeCharacterWorkshopOpenRequest();

// Publish the immutable numeric donor summary extracted while the launcher
// validates the player's ROM. The settings UI never owns or re-reads ROM bytes.
// Passing null or an invalid/version-mismatched summary clears the comparison.
void Settings_setDonorGameplayProfiles(
    const MdkrDonorGameplayProfiles *profiles,
    const char *unavailableReason = nullptr);

// Validate a package or begin a resumable raw-GLB authoring draft. This is also
// the window-wide drag-and-drop entry point, so platforms without a native
// picker retain both flows. GLB intake creates a source package first; every
// resulting package still enters the ordinary mutation-free comparison and
// explicit commit boundary.
bool Settings_importCharacterPackage(const char *path);

// Open the same bounded native source picker used by the Workshop import
// rail and retain the selected path for ordinary explicit validation/review.
// This lets the launcher's contextual primary action lead directly into the
// ROM-free authoring journey without duplicating picker filters or bypassing
// any import gate. False means unavailable or cancelled.
bool Settings_chooseCharacterSource();

// Consume the one-shot exact-game preview requested by the launcher Workshop.
// The in-game compact Settings view never produces one: starting another engine
// inside a running engine would violate the host/session lifetime contract.
bool Settings_takeCharacterPreviewRequest(
    SettingsCharacterPreviewRequest &request);
// Focused editor drawn by the launcher-owned exact-preview overlay. It edits
// the same persisted profile as Offset Studio and live-publishes validated
// presentation tuning to player one; it never starts a nested engine session.
SettingsCharacterStudioFrame Settings_drawCharacterOffsetStudio(
    SDL_Window *window, const char *packageId,
    MdkrCharacterPreviewContext context, bool initializeRuntime);
// Final save gate shared by the visible button and Escape/controller-back
// confirmation. Returns false after restoring the last durable fit.
bool Settings_commitCharacterOffsetStudio(
    const char *packageId, MdkrCharacterPreviewContext context);
// Rebind an interactive studio result to the profile that actually left the
// exact renderer. Empty means the package/context is no longer available.
std::string Settings_characterPreviewCurrentFitSignature(
    const std::string &packageId, MdkrCharacterPreviewContext context);
void Settings_publishCharacterPreviewResult(
    const std::string &packageId,
    const std::string &sourceSha256,
    const std::string &fitSha256,
    const std::string &presentationSha256,
    const std::string &capturePng,
    const SettingsCharacterPreviewDisposition &disposition,
    const MdkrCharacterPreviewResult &result,
    const MdkrCharacterMotionReviewResult *motionReview = nullptr);

// Discard any in-progress audible Audio slider preview. Used when navigation
// removes the settings panel before ImGui can emit a normal deactivation.
void Settings_cancelAudioPreview();

// Apply the persisted app-shell scale after AppConfig::load() and after ImGui
// setup. Invalid/out-of-range preference text safely resolves to 1.0.
void Settings_loadUiScalePreference();

// Center of the rendered Frame limit combo in logical window coordinates.
// Returns false until the widget has been drawn. Smoke-only observation; it
// never mutates UI state or bypasses ImGui input handling.
bool Settings_smokeFrameLimitCenter(int *x, int *y);

// Observe the real Frame limit popup after it has rendered. Returns false when
// it is closed; while open, `focusedIndex` is the focused public option or -1
// until ImGui establishes a navigation anchor.
bool Settings_smokeFrameLimitPopup(int *focusedIndex);

// Number of downward selections between two public Frame limit values, or -1
// when either value is absent/backwards. Keeps input smoke navigation tied to
// the rendered option table instead of a duplicated index count.
int Settings_smokeFrameLimitDownSteps(const char *from, const char *to);

// Center of the save-failure Retry control for Frame limit. This is exposed
// only so the shell smoke can route a real SDL click through ImGui after write
// access is restored in the same process.
bool Settings_smokeFrameLimitRetryCenter(int *x, int *y);

// Center of a rendered Presentation pace radio button ("original" or "smooth")
// in logical window coordinates. Returns false until that control has been
// drawn, and for "custom", which is a reading of the two underlying keys and
// has no widget. Smoke-only observation; it never mutates UI state.
bool Settings_smokePresentationPaceCenter(const char *pace, int *x, int *y);

// Bounds of the rendered UI-scale slider in logical window coordinates.
// Smoke-only observation used to drive a real held-pointer drag and prove the
// widget does not move underneath that pointer before the edit is committed.
bool Settings_smokeUiScaleRect(int *minX, int *minY, int *maxX, int *maxY);

// Publish completed Character Workshop jobs on the UI thread. The launcher
// calls this on every destination, not only while the Workshop is visible, so
// navigation cannot strand a ready result or make safe shutdown wait in a
// static destructor with no visible progress.
void Settings_serviceCharacterWork();

// True while any Character Workshop compiler, validation, or install result
// still needs to finish or publish on the UI thread. This is product lifecycle
// state; Play and shutdown use it as well as the shell smoke.
bool Settings_characterWorkPending();

// Backward-compatible name for the existing smoke harness.
bool Settings_smokeCharacterWorkPending();

// Collect the settings the player has staged but that the running/next engine
// has not picked up yet, as "Key=Value" strings, so the launcher can pass them
// straight into this boot via --video-set instead of making the player relaunch
// twice. Writes at most `cap` entries; returns how many were written.
int Settings_collectStagedOverrides(const char **out, int cap);

// True when at least one RESTART-scope setting differs from what is live.
bool Settings_restartPending();

// The player-facing label for what the NEXT launch will actually use for a
// setting -- the staged value if the player has edited it, otherwise the
// persisted one -- so the Play panel can summarize what pressing Play does
// without the player opening Settings. Returns a stable string owned by the
// settings module (option-table labels); never null. `key` is an MdkrVideoKey
// widened to int so this header need not include the engine enum.
const char *Settings_effectiveLabel(int videoKey);

// The player-facing name of what the next launch will use for the frame rate,
// in the merged control's own vocabulary: "Original", "Smooth", or "Custom".
// The frame rate is one control now, not the raw Frame limit key, so the Play
// summary names it the way the player set it. Stable string, never null.
const char *Settings_effectivePaceLabel();

// Opens and scrolls the launcher Settings panel to Controller on its next
// draw. Used by recovery routes; one-shot and process-local.
void Settings_requestControllerSection();

// Print the stable player-facing frame-limit labels used by the schema and
// rendered-settings smoke tests. This keeps the conservative release wording
// an executable contract rather than documentation that can silently drift.
void Settings_dumpSchemaContract();

#endif  // MDKR64_UI_SETTINGS_H
