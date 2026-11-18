#!/usr/bin/env python3
"""Guard actionable Vita LAN/relay failure text reaching the native dialog."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    lan = (ROOT / "platform/vita_lan.c").read_text(encoding="utf-8")
    relay = (ROOT / "platform/vita_relay.c").read_text(encoding="utf-8")
    menu = (ROOT / "game/src/menu.c").read_text(encoding="utf-8")
    runtime = (
        ROOT / "platform/rollback/rollback_game_runtime.c"
    ).read_text(encoding="utf-8")
    game = (ROOT / "game/src/thread3_main.c").read_text(encoding="utf-8")

    assert "s_failureMessage[128]" in lan
    assert "*message = s_failureMessage" in lan
    assert '"ux0:data/goldenballoon/mdkr_network_failure.log"' in lan
    assert "persist_failure_detail(failure, failure_message(failure), detail)" in lan
    assert 'fopen(MDKR_LAN_FAILURE_LOG_PATH, "w")' in lan
    for reason in (
        "INVALID LAN PACKET",
        "CHANNEL MISMATCH",
        "FRONTEND RX TIMEOUT",
        "RACE RX TIMEOUT",
        "INPUT GAP",
        "LATE INPUT",
        "INVALID INPUT",
        "INPUT REJECT",
    ):
        assert reason in lan
    assert "mdkr_vita_relay_format_diagnostics(" in lan
    assert "INVALID RELAY PACKET l=%u s=%u c=%u q=%u last=%u" in relay
    assert "RELAY SEQUENCE sender=%u got=%u last=%u" in relay
    for field in ("tx=%u", "rx=%u", "q=%u", "seq=%u/%u,%u"):
        assert field in relay

    for reason in (
        "CORRECTION INPUT",
        "CORRECTION RESTORE",
        "CORRECTION RATE",
        "CORRECTION BEGIN",
        "CORRECTION SIM",
        "CORRECTION ALLOCATION",
        "CORRECTION COVERAGE",
        "CORRECTION SNAPSHOT",
        "INPUT DRAIN",
        "INPUT BRIDGE",
        "INPUT BEGIN",
        "BOUNDARY SIDE EFFECT",
        "BOUNDARY ALLOCATION",
        "BOUNDARY COVERAGE",
        "BOUNDARY SNAPSHOT",
        "BOUNDARY UNPREPARED",
    ):
        assert reason in runtime
    assert game.count("mdkr_vita_lan_report_failure_detail(") >= 2
    assert game.count("mdkr_rollback_game_runtime_failure_reason()") >= 2

    reset_begin = menu.index("void menu_vita_lan_abort_race_presentation(void)")
    reset_end = menu.index(
        "static void menu_vita_lan_return_to_title", reset_begin
    )
    reset = menu[reset_begin:reset_end]
    for operation in (
        "disable_cutscene_camera();",
        "cam_set_layout(VIEWPORT_LAYOUT_1_PLAYER);",
        "set_active_camera(PLAYER_ONE);",
        "set_viewport_properties(",
        "viewport_menu_set(",
        "viewport_world_regions_reset();",
        "bgdraw_set_func(NULL);",
        "bgdraw_texture_init(NULL, NULL, 0);",
        "dialogue_close(i);",
        "dialogue_clear(i);",
    ):
        assert operation in reset

    return_begin = reset_end
    return_end = menu.index("#endif", return_begin)
    return_to_title = menu[return_begin:return_end]
    unload = return_to_title.index("unload_level_menu();")
    load = return_to_title.index("load_menu_with_level_background(")
    dialog = return_to_title.index("menu_vita_lan_show_failure(")
    assert unload < load < dialog

    game_failure = game[
        game.index("mdkr_vita_lan_take_failure("):
        game.index("MdkrVitaLanControl control;")
    ]
    assert game_failure.index("menu_vita_lan_abort_race_presentation();") < (
        game_failure.index("unload_level_game();")
    )
    assert game_failure.index("unload_level_game();") < game_failure.index(
        "load_menu_with_level_background("
    )
    assert game_failure.index("load_menu_with_level_background(") < (
        game_failure.index("menu_vita_lan_show_failure(")
    )
    assert "if (!vitaLanGfxTaskSubmitted)" in game_failure
    assert "gSkipGfxTask = TRUE;" in game_failure
    print("test_vita_lan_failure_diagnostics: PASS")


if __name__ == "__main__":
    main()
