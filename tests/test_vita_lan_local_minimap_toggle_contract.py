#!/usr/bin/env python3
"""Guard that the online minimap on/off toggle stays local to each endpoint.

Every endpoint runs every canonical seat's input through hud_tick, so a toggle
written to the shared gHudToggleSettings let one player's R-C press hide the
other player's minimap. Online endpoints must flip a local copy only for their
own seat's live (non-resimulated) press, while hud_setting() -- read by the
simulation's 3-view TT camera/sort/visibility -- keeps the canonical value.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body for {signature}")


def main() -> None:
    ui = (ROOT / "game/src/game_ui.c").read_text(encoding="utf-8")

    assert '#include "rollback/rollback_game_runtime.h"' in ui

    is_local = function_body(ui, "static s32 hud_minimap_toggle_is_local(void)")
    assert "mdkr_net_roster_runtime_active()" in is_local

    slot = function_body(ui, "static s8 *hud_minimap_toggle_slot(void)")
    assert "&sHudLocalToggleSettings[gHUDNumPlayers]" in slot
    assert "&gHudToggleSettings[gHUDNumPlayers]" in slot

    owned = function_body(
        ui, "static s32 hud_minimap_toggle_owned(const Object_Racer *racer)"
    )
    assert "if (!hud_minimap_toggle_is_local())" in owned
    assert "mdkr_rollback_game_runtime_presentation_allowed()" in owned
    assert "mdkr_net_roster_runtime_canonical_to_local(" in owned
    assert "racer->playerIndex" in owned

    tick = function_body(
        ui, "static void hud_player_tick(Object *obj, s32 updateRate)"
    )
    toggle = tick.index("& R_CBUTTONS")
    toggle_block = tick[toggle : tick.index("if (gRaceStartShowHudStep == 0)", toggle)]
    assert "hud_minimap_toggle_owned(racer)" in toggle_block
    assert "hud_minimap_toggle_slot()" in toggle_block
    assert "gHudToggleSettings" not in toggle_block, (
        "native HUD tick must not write the shared minimap toggle"
    )

    setting = function_body(ui, "s8 hud_setting(void)")
    assert "gHudToggleSettings[gHUDNumPlayers]" in setting, (
        "simulation readers must keep the canonical toggle"
    )

    native_ui = ui[ui.index("static void hud_player_tick(") :]
    stale_reads = re.findall(
        r"gHudToggleSettings\[gHUDNumPlayers\]\s*(?:==|!=)", native_ui
    )
    assert not stale_reads, "draw-side minimap reads must use HUD_MINIMAP_TOGGLE"
    assert native_ui.count("HUD_MINIMAP_TOGGLE") >= 5

    init = ui.index("gPrevToggleSetting = gHudToggleSettings[1];")
    init_block = ui[init : ui.index("}\n}", init)]
    assert "sHudLocalPrevToggleSetting = sHudLocalToggleSettings[1];" in init_block
    assert "sHudLocalToggleSettings[1] = sHudLocalPrevToggleSetting;" in init_block

    print("test_vita_lan_local_minimap_toggle_contract: PASS")


if __name__ == "__main__":
    main()
