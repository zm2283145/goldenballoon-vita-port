// app_launch_hold.h — "was the player holding the way back?" (issue #60).
//
// Sampled REPEATEDLY, not once. SDL's keyboard state is derived from the event
// stream, so a Shift already held down before the window existed produces no
// key-down for SDL to fold in -- on macOS the first thing SDL learns about that
// key is its RELEASE, and a single sample taken right after window creation
// reads 0. The pad is different (HID polling gives absolute button state
// immediately), but one sampler serves both.
//
// So the launcher re-samples every frame until the direct boot dispatches, and
// disarms the skip the first time it sees the hold. That makes the window a
// bounded, documentable thing -- "while the launcher is on screen", which is at
// least as long as it takes to hash the ROM -- instead of one instant nobody
// can aim at.
#ifndef MDKR64_APP_LAUNCH_HOLD_H
#define MDKR64_APP_LAUNCH_HOLD_H

#include "app_ui_policy.h"  // AppUiLauncherHold

// One sample of the live device state. `sampleIndex` counts samples within this
// launch: 0 is main()'s sample, taken before the first frame is built, and the
// launcher's per-frame samples continue from 1. It exists so the test seam can
// make a hold APPEAR partway through the window -- the case the one-shot
// sampler got wrong, and the one a scripted run cannot otherwise produce.
// Live shoulder input is a same-controller pair; separate pads cannot combine
// a gesture. Each sample also discovers newly connected controllers and releases
// detached borrows, without reopening the devices that remain connected.
//
// MDKR_APP_TEST_LAUNCH_HOLD (test-only) injects the RAW hold rather than the
// decision, so AppUi_launcherHoldOpensLauncher() still runs for real:
//
//   shift | shoulders | left-shoulder | right-shoulder   held from sample 0
//   <any of the above>@<n>                               held from sample n
AppUiLauncherHold AppLaunchHold_sample(unsigned sampleIndex);

// End the sampling window and give back the controllers it borrowed. Sampling
// opens each pad once and holds it for the window rather than reopening ~46
// times, so the window needs an end; call this at every exit from it -- the
// direct boot dispatching, a hold disarming it, a launch that never armed, any
// other boot, and the launcher's own teardown. Idempotent, and a later sample
// simply opens the pads again.
//
// Safe to call after SDL has been shut down: it closes only while
// SDL_INIT_GAMECONTROLLER is still up, and drops the handles either way. main()
// runs host.shutdown() before its scope ends, so ~Launcher's backstop call
// lands after SDL_Quit() -- the guard is what keeps that from being a
// use-after-free rather than an ordering nobody may ever change.
void AppLaunchHold_release();

#endif  // MDKR64_APP_LAUNCH_HOLD_H
