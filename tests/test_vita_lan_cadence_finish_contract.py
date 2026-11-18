#!/usr/bin/env python3
"""Guard Vita LAN wall-speed cadence and host-authoritative race completion."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin + len(start))]


def main() -> None:
    thread = (ROOT / "game/src/thread3_main.c").read_text(encoding="utf-8")
    objects = (ROOT / "game/src/objects.c").read_text(encoding="utf-8")
    menu = (ROOT / "game/src/menu.c").read_text(encoding="utf-8")
    runtime = (
        ROOT / "platform/rollback/rollback_game_runtime.c"
    ).read_text(encoding="utf-8")
    lan = (ROOT / "platform/vita_lan.c").read_text(encoding="utf-8")
    platform = (ROOT / "platform/platform_sdl_min.c").read_text(
        encoding="utf-8"
    )

    cadence = thread[thread.index("logicUpdateRate = video_logic_update_rate") :]
    accumulate = cadence.index("sVitaLanCadenceFields += measuredFields;")
    wait = cadence.index("vitaLanSimulationStalled = TRUE;", accumulate)
    author = cadence.index("logicUpdateRate = LOGIC_30FPS;", wait)
    assert accumulate < wait < author
    assert "(vitaLanSimulationStalled ||" in thread
    assert "mdkr_vita_lan_should_stall_race())" in thread
    assert (
        "mdkr_vita_lan_state() == MDKR_VITA_LAN_RACING"
        in platform[platform.index("bool platform_sim_cadence_is_enhanced") :]
    )

    assert "mdkr_rollback_game_runtime_network_active() &&" in objects
    assert "numHumanRacersFinished > 0" in objects
    assert "mdkr_vita_lan_finish_target_tick()" in runtime
    assert "authoritative_tick != 0u" in runtime
    assert "s_role == MDKR_LAN_ROLE_BROWSER" in lan
    assert "finishTick == s_finishLocalTick" in lan
    assert "unsigned int mdkr_vita_lan_finish_target_tick(void)" in lan

    postrace = between(
        menu,
        "case POSTRACE_STAGE_FADE_OUT:",
        "case POSTRACE_STAGE_END:",
    )
    lan_transition = between(
        postrace,
        "if (mdkr_vita_lan_state() == MDKR_VITA_LAN_RACING)",
        "#endif",
    )
    assert "transition_begin(&sMenuTransitionFadeIn);" in lan_transition
    assert "gMenuStage = POSTRACE_STAGE_END;" in lan_transition
    assert "buttonsPressed" not in lan_transition
    assert "mdkr_vita_lan_request_control" not in lan_transition
    assert "mdkr_vita_lan_restart_race" not in lan_transition
    assert postrace.index(lan_transition) < postrace.index(
        "if (buttonsPressed & (A_BUTTON | START_BUTTON))"
    )

    assert (
        'log_control("control-requested", wireType);' in lan
        and 'log_control("control-received", wireType);' in lan
    )
    assert 'return "RESTART";' in lan
    assert 'log_result("control-requested"' not in lan
    assert 'log_result("control-received"' not in lan
    print("test_vita_lan_cadence_finish_contract: PASS")


if __name__ == "__main__":
    main()
