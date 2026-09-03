/** app_launch_hold.cpp — see app_launch_hold.h. */
#include "app_launch_hold.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

/*
 * The controllers borrowed for this sampling window, opened on the first
 * sample that needs them and closed by AppLaunchHold_release().
 *
 * Opening and closing every pad on every frame would have been ~46 open/close
 * pairs for one launch, and SDL's open path is not free: it re-reads the
 * mapping database and re-initialises the device. Holding the handles for the
 * window instead is both cheaper and more honest about what is going on -- we
 * are watching these buttons for a bounded period, not asking a fresh question
 * each frame. SDL2 refcounts the handle, so this remains a balanced borrow that
 * neither steals a controller from another owner nor closes one.
 */
std::vector<SDL_GameController *> g_pads;
bool                              g_padsOpen = false;

void openPads() {
    if (g_padsOpen) return;
    g_padsOpen = true;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (SDL_GameController *pad = SDL_GameControllerOpen(i)) {
            g_pads.push_back(pad);
        }
    }
}

/*
 * Parse the test seam: "<hold>" or "<hold>@<sample>". Every rejected spelling
 * is announced rather than silently treated as "nothing held" -- a typo there
 * would otherwise turn an arm that means to prove the hold works into an arm
 * that proves nothing at all, and pass.
 */
bool scriptedHold(const char *scripted, unsigned sampleIndex,
                  AppUiLauncherHold *out) {
    const char  *at = std::strchr(scripted, '@');
    unsigned     from = 0u;
    const size_t nameLength =
        at != nullptr ? static_cast<size_t>(at - scripted) : std::strlen(scripted);
    if (at != nullptr) {
        const char *digits = at + 1;
        char       *end = nullptr;
        /* strtoul accepts a leading '-' and wraps it, so "@-1" would otherwise
         * parse as ULONG_MAX and silently become "a hold that never appears" --
         * an arm that can only pass. Reject the sign before parsing. */
        if (*digits < '0' || *digits > '9') {
            std::fprintf(stderr,
                         "[app] MDKR_APP_TEST_LAUNCH_HOLD=%s has a sample "
                         "index that is not a non-negative number; nothing "
                         "is held\n",
                         scripted);
            return false;
        }
        const unsigned long parsed = std::strtoul(digits, &end, 10);
        if (end == digits || *end != '\0' || parsed > 0xFFFFFFFFul) {
            std::fprintf(stderr,
                         "[app] MDKR_APP_TEST_LAUNCH_HOLD=%s has an "
                         "unparseable sample index; nothing is held\n",
                         scripted);
            return false;
        }
        from = static_cast<unsigned>(parsed);
    }

    const auto named = [&](const char *name) {
        return std::strlen(name) == nameLength &&
               std::strncmp(scripted, name, nameLength) == 0;
    };
    const bool shift = named("shift");
    const bool shoulders = named("shoulders");
    const bool left = named("left-shoulder");
    const bool right = named("right-shoulder");
    if (!shift && !shoulders && !left && !right) {
        std::fprintf(stderr,
                     "[app] MDKR_APP_TEST_LAUNCH_HOLD=%s is not a hold this "
                     "build knows (shift, shoulders, left-shoulder, "
                     "right-shoulder, any of them with @<sample>); nothing "
                     "is held\n",
                     scripted);
        return false;
    }
    if (sampleIndex < from) return true;  // held, but not yet

    out->shift = shift;
    out->leftShoulder = shoulders || left;
    out->rightShoulder = shoulders || right;
    return true;
}

}  // namespace

AppUiLauncherHold AppLaunchHold_sample(unsigned sampleIndex) {
    AppUiLauncherHold hold;
    if (const char *scripted = std::getenv("MDKR_APP_TEST_LAUNCH_HOLD")) {
        if (scriptedHold(scripted, sampleIndex, &hold)) return hold;
        return AppUiLauncherHold{};
    }

    /* The keyboard half is why this is re-sampled at all: SDL's state is folded
     * from events, so a key already down before the window existed is invisible
     * here until something moves it. Reading it every frame is what turns "the
     * instant the window appeared" into "while the launcher is on screen". */
    SDL_PumpEvents();
    int          keyCount = 0;
    const Uint8 *keys = SDL_GetKeyboardState(&keyCount);
    if (keys != nullptr && keyCount > SDL_SCANCODE_RSHIFT) {
        hold.shift = keys[SDL_SCANCODE_LSHIFT] != 0 ||
                     keys[SDL_SCANCODE_RSHIFT] != 0;
    }
    /* The pad half needs no such care -- HID polling reports absolute button
     * state on the first read -- but it costs nothing to keep the two answers
     * on one clock. */
    openPads();
    SDL_GameControllerUpdate();
    for (SDL_GameController *pad : g_pads) {
        if (SDL_GameControllerGetButton(
                pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0) {
            hold.leftShoulder = true;
        }
        if (SDL_GameControllerGetButton(
                pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0) {
            hold.rightShoulder = true;
        }
    }
    return hold;
}

void AppLaunchHold_release() {
    for (SDL_GameController *pad : g_pads) SDL_GameControllerClose(pad);
    g_pads.clear();
    g_padsOpen = false;
}
