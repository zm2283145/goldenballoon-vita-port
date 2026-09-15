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
struct BorrowedPad {
    SDL_GameController *handle;
    SDL_JoystickID instance;
};
std::vector<BorrowedPad> g_pads;
bool                              g_padsOpen = false;

void openPads() {
    bool changed = !g_padsOpen;
    g_padsOpen = true;
    // Reconcile once per sample. Device indices move after unplugging, whereas
    // SDL instance IDs identify this connection. Keep connected handles borrowed
    // and discover late attachments without reopening every pad every frame.
    for (auto it = g_pads.begin(); it != g_pads.end();) {
        if (!SDL_GameControllerGetAttached(it->handle)) {
            SDL_GameControllerClose(it->handle);
            it = g_pads.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i)) continue;
        const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(i);
        if (instance < 0) continue;
        bool borrowed = false;
        for (const BorrowedPad &pad : g_pads) {
            if (pad.instance == instance) borrowed = true;
        }
        if (borrowed) continue;
        if (SDL_GameController *pad = SDL_GameControllerOpen(i)) {
            // A hotplug can change the index between enumeration and open.
            // Do not keep an incorrectly identified borrow; retry next sample.
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) !=
                instance) {
                SDL_GameControllerClose(pad);
                continue;
            }
            try {
                g_pads.push_back({pad, instance});
            } catch (...) {
                SDL_GameControllerClose(pad);
                throw;
            }
            changed = true;
        }
    }
    /* How many handles this window is holding. A test machine with no
     * controller attached borrows none, and then the release path below has
     * nothing to close -- so an arm that means to prove the teardown ordering
     * is safe has to be able to see that it actually had a handle to get
     * wrong. Without this line that arm would pass on an empty loop. */
    if (changed) {
        std::fprintf(stderr, "[app] skip-launcher watching pads=%zu\n",
                     g_pads.size());
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
    for (const BorrowedPad &pad : g_pads) {
        // The escape gesture belongs to ONE controller, never the union of
        // an incidental left shoulder on one pad and right on another.
        if (SDL_GameControllerGetAttached(pad.handle) &&
            SDL_GameControllerGetButton(
                pad.handle, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0 &&
            SDL_GameControllerGetButton(
                pad.handle, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0) {
            hold.leftShoulder = hold.rightShoulder = true;
        }
    }
    return hold;
}

void AppLaunchHold_release() {
    /*
     * Closing is conditional; forgetting is not.
     *
     * main() owns AppHost by value and calls host.shutdown() -- which reaches
     * SDL_Quit() -- BEFORE its scope ends, so anything running during the
     * subsequent unwind (~Launcher, which calls this as its backstop) is
     * running after SDL freed every device it owned. Handing those pointers
     * back to SDL_GameControllerClose() there is a use-after-free, and it is
     * reachable: skip armed, a remembered ROM that never dispatches, and the
     * player picks Quit.
     *
     * Every ordinary exit now releases while SDL is still up, so this guard
     * should never be the thing that saves us. It is here because "should
     * never" and "cannot" are different claims, and only one of them is worth
     * betting a crash on.
     */
    const bool sdlUp = SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0;
    /*
     * The witness for the ordering above, and the only deterministic one there
     * is. ASan does NOT catch the bad ordering on SDL 2.x: SDL_GameControllerClose
     * validates its argument against an internal list that SDL_Quit has already
     * emptied, so a stale handle is dropped without being dereferenced. That is
     * SDL's internal luck, not a contract -- it is not documented, and it is
     * exactly the kind of thing that differs between SDL versions and
     * platforms. So the gate asserts the ORDERING (a release carrying handles
     * must happen while the subsystem is still up) rather than waiting for a
     * sanitizer report that this SDL will never produce.
     */
    if (!g_pads.empty()) {
        std::fprintf(stderr,
                     "[app] skip-launcher released pads=%zu sdlUp=%d\n",
                     g_pads.size(), sdlUp ? 1 : 0);
    }
    if (sdlUp) {
        for (const BorrowedPad &pad : g_pads) {
            SDL_GameControllerClose(pad.handle);
        }
    }
    g_pads.clear();
    g_padsOpen = false;
}
