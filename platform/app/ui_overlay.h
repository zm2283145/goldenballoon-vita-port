// ui_overlay.h — the in-game overlay (F1): live settings, quit, FPS readout.
#ifndef MDKR64_UI_OVERLAY_H
#define MDKR64_UI_OVERLAY_H

#include "engine_entry.h"

struct SDL_Window;
class MdkrNativePartyHost;

enum class OverlayExitRequest {
    None,
    ReturnToLauncher,
    ReturnToLauncherWithoutMeasurement,
    RestartGame,
};

// Register the overlay hooks with the engine. Call before mdkr64_engine_boot().
void Overlay_install(SDL_Window *window);

// Focused, launcher-owned Offset Studio over the exact preview scene. Unlike
// the ordinary F1 menu it is always visible, captures all gameplay input, and
// keeps rendering/simulation alive so each validated tuning edit is visible on
// the following game frame.
void Overlay_installCharacterStudio(
    SDL_Window *window, const char *packageId,
    MdkrCharacterPreviewContext context);

// Local menus pause; online Party chrome captures navigation without pausing.
// The launcher-owned SessionRuntime updates this before an engine session.
void Overlay_setPauseAllowed(bool allowed);

/* Launcher-owned and process-lived; serviced even while overlay UI is hidden. */
void Overlay_setPhonePartyHost(MdkrNativePartyHost *host);

// Consume an orderly launcher/restart request made by the overlay. The overlay
// only asks the engine to quit; main_app performs renderer/audio/log teardown
// before it replaces the process.
OverlayExitRequest Overlay_consumeExitRequest();

// The gamepad button reserved for toggling the overlay, as an
// SDL_GameControllerButton value. Single source of truth so nothing else can
// bind it.
int Overlay_gamepadToggleButton();

#endif  // MDKR64_UI_OVERLAY_H
