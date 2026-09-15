// Runs the production sampler against a deterministic SDL device boundary.
// No real devices, window, event loop, ROM, or SDL library are opened here.
#include "app_launch_hold.h"
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
struct Device {
    SDL_JoystickID id;
    bool attached = true;
    bool left = false;
    bool right = false;
    int opens = 0;
    int closes = 0;
};
std::vector<Device *> devices;
std::array<Uint8, SDL_NUM_SCANCODES> keys{};
bool initialized = true;
SDL_JoystickID openedIdentityOverride = -1;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL app_launch_hold: %s\n", message);
        std::abort();
    }
}
Device *device(SDL_GameController *pad) {
    return reinterpret_cast<Device *>(pad);
}
bool paired(AppUiLauncherHold hold) {
    return hold.leftShoulder && hold.rightShoulder;
}
}

extern "C" {
void SDLCALL SDL_PumpEvents(void) {}
const Uint8 *SDLCALL SDL_GetKeyboardState(int *count) {
    *count = static_cast<int>(keys.size());
    return keys.data();
}
int SDLCALL SDL_NumJoysticks(void) { return static_cast<int>(devices.size()); }
SDL_bool SDLCALL SDL_IsGameController(int index) {
    return index >= 0 && index < SDL_NumJoysticks() ? SDL_TRUE : SDL_FALSE;
}
SDL_JoystickID SDLCALL SDL_JoystickGetDeviceInstanceID(int index) {
    return devices.at(static_cast<size_t>(index))->id;
}
SDL_GameController *SDLCALL SDL_GameControllerOpen(int index) {
    Device *pad = devices.at(static_cast<size_t>(index));
    ++pad->opens;
    return reinterpret_cast<SDL_GameController *>(pad);
}
SDL_Joystick *SDLCALL SDL_GameControllerGetJoystick(SDL_GameController *pad) {
    return reinterpret_cast<SDL_Joystick *>(pad);
}
SDL_JoystickID SDLCALL SDL_JoystickInstanceID(SDL_Joystick *pad) {
    return openedIdentityOverride >= 0 ? openedIdentityOverride
                                     : reinterpret_cast<Device *>(pad)->id;
}
SDL_bool SDLCALL SDL_GameControllerGetAttached(SDL_GameController *pad) {
    return device(pad)->attached ? SDL_TRUE : SDL_FALSE;
}
void SDLCALL SDL_GameControllerUpdate(void) {}
Uint8 SDLCALL SDL_GameControllerGetButton(SDL_GameController *pad,
                                         SDL_GameControllerButton button) {
    return button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? device(pad)->left
         : button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? device(pad)->right
         : 0;
}
void SDLCALL SDL_GameControllerClose(SDL_GameController *pad) {
    expect(initialized, "no controller close after SDL shutdown");
    ++device(pad)->closes;
    expect(device(pad)->closes <= device(pad)->opens, "balanced device borrows");
}
Uint32 SDLCALL SDL_WasInit(Uint32 flags) { return initialized ? flags : 0; }
}

int main() {
    // Refuse a caller's scripted-hold override rather than silently exercising
    // the injection branch instead of these actual production device reads.
    expect(std::getenv("MDKR_APP_TEST_LAUNCH_HOLD") == nullptr,
           "unset MDKR_APP_TEST_LAUNCH_HOLD for the live sampler fixture");
    expect(!paired(AppLaunchHold_sample(0)), "empty initial device list");
    Device first{41}, second{72}, replacement{93};
    devices = {&first, &second};
    first.left = true;
    second.right = true;
    expect(!paired(AppLaunchHold_sample(1)), "different pads cannot pair shoulders");
    expect(first.opens == 1 && second.opens == 1, "late attached devices discovered");
    expect(!paired(AppLaunchHold_sample(2)), "unpaired holds stay unpaired");
    expect(first.opens == 1 && second.opens == 1, "connected borrows reused");
    second.left = true;
    expect(paired(AppLaunchHold_sample(3)), "same controller pair recognized");
    second.left = false;
    keys[SDL_SCANCODE_LSHIFT] = 1;
    expect(AppLaunchHold_sample(4).shift, "keyboard escape remains independent");
    keys.fill(0);

    first.attached = false;
    devices.erase(devices.begin()); // second's index changes; instance does not.
    expect(!paired(AppLaunchHold_sample(5)), "unplugged pad does not contribute");
    expect(first.closes == 1 && second.opens == 1,
           "detached borrow released; renumbered device not reopened");
    replacement.left = replacement.right = true;
    devices.push_back(&replacement);
    expect(paired(AppLaunchHold_sample(6)), "replacement pad discovered in same window");
    AppLaunchHold_release();
    AppLaunchHold_release();
    expect(first.opens == first.closes && second.opens == second.closes &&
               replacement.opens == replacement.closes,
           "release closes each borrowed handle exactly once");

    // Simulate an index changing identity between enumeration and open.
    openedIdentityOverride = 999;
    expect(!paired(AppLaunchHold_sample(7)), "raced device identity not sampled");
    expect(second.opens == second.closes && replacement.opens == replacement.closes,
           "mismatched identity borrows immediately released");
    openedIdentityOverride = -1;
    expect(paired(AppLaunchHold_sample(8)), "next sample retries stable device identity");
    AppLaunchHold_release();
    initialized = false;
    AppLaunchHold_release();
    std::puts("PASS app_launch_hold: same-device pairing, hotplug, identity, ownership");
    return 0;
}
