#!/usr/bin/env python3
"""Behavioural proof for the endpoint-local R-C minimap toggle (Vita LAN).

The production helpers (hud_minimap_toggle_is_local/_slot/_owned and the
HUD_MINIMAP_TOGGLE accessor) and the production R-C mutation statement from
hud_player_tick are sliced verbatim out of game/src/game_ui.c and compiled
against small host stubs for the roster/rollback/input seams. Nothing is
reimplemented: if game_ui.c changes the helpers or the R-C block, this fixture
compiles and executes the new text.

Scenarios (two endpoints, both running every canonical seat like hud_tick):
  * each endpoint responds only to its own seat's press;
  * a remote seat's press leaves the local value untouched;
  * a replayed local press (resimulation, presentation disallowed) cannot
    flip the value a second time;
  * canonical gHudToggleSettings stays at its defaults while online;
  * offline the original gHudToggleSettings slot is toggled by any seat.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_UI = ROOT / "game/src/game_ui.c"

HELPER_START = "static s8 sHudLocalToggleSettings[4]"
HELPER_END = "#define HUD_MINIMAP_TOGGLE (*hud_minimap_toggle_slot())"
RC_START = "if (input_pressed(gHudController) & R_CBUTTONS"

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int8_t s8;
typedef int32_t s32;
#define TRUE 1
#define FALSE 0
#define R_CBUTTONS 0x0001
#define SOUND_TING_LOW 1
#define SOUND_TING_HIGH 2
typedef struct { s32 playerIndex; s32 raceFinished; } Object_Racer;

/* Canonical ROM defaults (game_ui.c: gHudToggleSettings). */
static s8 gHudToggleSettings[4] = { 1, 1, 1, 1 };
static s32 gHUDNumPlayers = 1; /* 2-player layout index */
static s32 gHudController;
static s32 gHudRaceStart = 1;
static s32 gMinimapOpacity = 0;

static s32 stubRosterActive;
static s32 stubLocalSeat;        /* canonical seat owned by this endpoint */
static s32 stubPresentationAllowed = 1;
static unsigned stubPressedMask[4];
static s32 stubSounds;

static s32 mdkr_net_roster_runtime_active(void) { return stubRosterActive; }
static int mdkr_net_roster_runtime_canonical_to_local(unsigned canonical,
                                                      uint8_t *local) {
    if ((s32) canonical != stubLocalSeat) {
        return 0;
    }
    *local = 0;
    return 1;
}
static int mdkr_rollback_game_runtime_presentation_allowed(void) {
    return stubPresentationAllowed;
}
static unsigned input_pressed(s32 controller) {
    return stubPressedMask[controller & 3];
}
static void sound_play(s32 id, void *mask) { (void) id; (void) mask; stubSounds++; }
"""

DRIVER = r"""
static void rc_tick_seat(Object_Racer *racer) {
    gHudController = racer->playerIndex;
    __RC_BLOCK__
}

/* One hud_tick: every canonical seat runs, as on every LAN endpoint. */
static void hud_tick_all(Object_Racer *racers, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        rc_tick_seat(&racers[i]);
    }
}

typedef struct { s8 local[4]; s8 prev; } EndpointState;

static void endpoint_enter(const EndpointState *e, s32 seat) {
    memcpy(sHudLocalToggleSettings, e->local, sizeof(e->local));
    sHudLocalPrevToggleSetting = e->prev;
    stubLocalSeat = seat;
}
static void endpoint_leave(EndpointState *e) {
    memcpy(e->local, sHudLocalToggleSettings, sizeof(e->local));
    e->prev = sHudLocalPrevToggleSetting;
}

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static const s8 kCanonical[4] = { 1, 1, 1, 1 };

int main(void) {
    Object_Racer racers[2] = { { 0, 0 }, { 1, 0 } };
    EndpointState host = { { 1, 1, 1, 1 }, 1 };
    EndpointState guest = { { 1, 1, 1, 1 }, 1 };
    (void) sHudLocalPrevToggleSetting;

    stubRosterActive = 1;

    /* Seat 0 (host's player) presses R-C; both endpoints simulate it. */
    memset(stubPressedMask, 0, sizeof(stubPressedMask));
    stubPressedMask[0] = R_CBUTTONS;
    endpoint_enter(&host, 0);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 0, "host own press hides host minimap");
    endpoint_leave(&host);
    endpoint_enter(&guest, 1);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 1, "remote seat0 press leaves guest minimap on");
    endpoint_leave(&guest);

    /* Seat 1 (guest's player) presses R-C. */
    memset(stubPressedMask, 0, sizeof(stubPressedMask));
    stubPressedMask[1] = R_CBUTTONS;
    endpoint_enter(&host, 0);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 0, "remote seat1 press leaves host value untouched");
    endpoint_leave(&host);
    endpoint_enter(&guest, 1);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 0, "guest own press hides guest minimap");
    endpoint_leave(&guest);

    /* Rollback resimulation replays the guest's earlier press edge. */
    stubPresentationAllowed = 0;
    endpoint_enter(&guest, 1);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 0, "replayed local press does not flip twice");
    endpoint_leave(&guest);
    stubPresentationAllowed = 1;

    /* Live press again restores it (toggle still works after a replay). */
    endpoint_enter(&guest, 1);
    hud_tick_all(racers, 2);
    CHECK(HUD_MINIMAP_TOGGLE == 1, "subsequent live local press toggles back");
    endpoint_leave(&guest);

    CHECK(memcmp(gHudToggleSettings, kCanonical, sizeof(kCanonical)) == 0,
          "canonical gHudToggleSettings unchanged while online");
    CHECK(hud_minimap_toggle_slot() != &gHudToggleSettings[gHUDNumPlayers],
          "online slot is endpoint-local, not canonical");

    /* Offline: original ROM slot, any seat toggles, resim gate irrelevant. */
    stubRosterActive = 0;
    stubLocalSeat = -1;
    CHECK(hud_minimap_toggle_slot() == &gHudToggleSettings[gHUDNumPlayers],
          "offline slot is gHudToggleSettings[gHUDNumPlayers]");
    memset(stubPressedMask, 0, sizeof(stubPressedMask));
    stubPressedMask[1] = R_CBUTTONS;
    rc_tick_seat(&racers[1]);
    CHECK(gHudToggleSettings[gHUDNumPlayers] == 0, "offline press toggles canonical slot");
    stubPresentationAllowed = 0;
    rc_tick_seat(&racers[1]);
    CHECK(gHudToggleSettings[gHUDNumPlayers] == 1, "offline toggle ignores presentation gate");
    stubPresentationAllowed = 1;

    if (failures != 0) {
        printf("test_vita_lan_local_minimap_toggle_behavior: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_vita_lan_local_minimap_toggle_behavior: PASS\n");
    return 0;
}
"""


def slice_rc_block(text: str) -> str:
    # game_ui.c keeps the donor N64 hud_player_tick under #ifndef NATIVE_PORT;
    # the native (Vita-built) copy is the one inside the #ifdef NATIVE_PORT
    # hud_player_tick, which is the last R-C block in the file.
    start = text.rfind(RC_START)
    if start < 0:
        raise ValueError("R-C mutation block not found in game_ui.c")
    native = text.rfind("#ifdef NATIVE_PORT", 0, start)
    donor = text.rfind("#ifndef NATIVE_PORT", 0, start)
    if native < 0 or donor > native:
        raise ValueError("last R-C mutation block is not in the NATIVE_PORT path")
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise ValueError("unterminated R-C mutation block")


def slice_helpers(text: str) -> str:
    start = text.find(HELPER_START)
    end = text.find(HELPER_END)
    if start < 0 or end < 0 or end < start:
        raise ValueError("minimap toggle helper block not found in game_ui.c")
    return text[start:end + len(HELPER_END)]


def find_cc() -> str | None:
    env = os.environ.get("CC")
    if env:
        return env
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    for candidate in (r"C:\msys64\mingw64\bin\gcc.exe", r"C:\msys64\ucrt64\bin\gcc.exe"):
        if Path(candidate).is_file():
            return candidate
    return None


def main() -> int:
    text = GAME_UI.read_text(encoding="utf-8", errors="replace")
    try:
        helpers = slice_helpers(text)
        rc_block = slice_rc_block(text)
    except ValueError as exc:
        print(f"FAIL local minimap toggle behaviour: {exc}", file=sys.stderr)
        return 1
    if not re.search(r"hud_minimap_toggle_owned\(racer\)", rc_block):
        print("FAIL local minimap toggle behaviour: R-C block is not gated on "
              "hud_minimap_toggle_owned(racer)", file=sys.stderr)
        return 1
    source = PRELUDE + "\n" + helpers + "\n" + DRIVER.replace("__RC_BLOCK__", rc_block)
    cc = find_cc()
    if cc is None:
        print("FAIL local minimap toggle behaviour: no host C compiler "
              "(set CC)", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="mdkr64-minimap-toggle-") as tmp:
        src = Path(tmp) / "minimap_toggle_fixture.c"
        binary = Path(tmp) / ("minimap_toggle_fixture" + (".exe" if os.name == "nt" else ""))
        src.write_text(source, encoding="utf-8")
        compiled = subprocess.run(
            [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", str(src), "-o", str(binary)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False)
        if compiled.returncode != 0:
            print("FAIL local minimap toggle behaviour: compile failed",
                  file=sys.stderr)
            print(compiled.stdout, file=sys.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60, check=False)
        sys.stdout.write(run.stdout)
        if run.returncode != 0 or \
                "test_vita_lan_local_minimap_toggle_behavior: PASS" not in run.stdout:
            print(f"FAIL local minimap toggle behaviour: exit {run.returncode}",
                  file=sys.stderr)
            return 1
    print("PASS local minimap toggle behaviour: own-seat only, remote press "
          "ignored, replay cannot double-flip, canonical slot untouched online, "
          "offline uses gHudToggleSettings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
