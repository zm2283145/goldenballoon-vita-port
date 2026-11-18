#!/usr/bin/env python3
"""Guard Vita online stalls as front-image holds, never stale scene walks."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin + len(start))]


def main() -> None:
    thread = (ROOT / "game/src/thread3_main.c").read_text(encoding="utf-8")
    platform = (ROOT / "platform/platform_sdl_min.c").read_text(
        encoding="utf-8"
    )
    scheduler = (ROOT / "platform/stubs_dkr.c").read_text(encoding="utf-8")

    loop = between(thread, "void main_game_loop(void)", "void unload_level_game(void)")
    handoff = loop.index("vitaLanSkipDeferredGfx = sVitaLanSamePassGfxActive;")
    handoff_update = loop.index(
        "sVitaLanSamePassGfxActive = vitaLanSamePassGfx;", handoff
    )
    deferred_submit = loop.index("&& !vitaLanSamePassGfx")
    deferred_handoff = loop.index("&& !vitaLanSkipDeferredGfx", deferred_submit)
    deferred_call = loop.index("gfxtask_run_xbus(", deferred_submit)
    assert handoff < handoff_update < deferred_submit < deferred_handoff
    assert deferred_handoff < deferred_call

    stall_gate = loop.index("(vitaLanSimulationStalled ||")
    first_authoring = loop.index("presentation_task_authoring_begin(", stall_gate)
    hold_jump = loop.index("goto vita_lan_hold_frame;", stall_gate)
    assert stall_gate < hold_jump < first_authoring

    hold = between(loop, "vita_lan_hold_frame:", "vita_lan_finish_frame:")
    assert "gCurrDisplayList = gDisplayLists[gSPTaskNum];" in hold
    assert "render_scene(" not in hold
    assert "mode_game_render_stalled" not in thread

    finish = loop[loop.index("vita_lan_finish_frame:") :]
    same_pass = finish.index(
        "if (vitaLanSamePassGfx && !vitaLanSimulationStalled"
    )
    complete = finish.index("gSPEndDisplayList(gCurrDisplayList++);")
    submit_call = finish.index("gfxtask_run_xbus(", same_pass)
    submitted = finish.index("vitaLanGfxTaskSubmitted = TRUE;", submit_call)
    assert complete < same_pass < submit_call < submitted
    wait = between(
        finish,
        "if (vitaLanGfxTaskSubmitted)",
        "#else",
    )
    assert "gfxtask_wait()" in wait

    frame_service = between(
        platform,
        "static void platform_vita_frame_service(void)",
        "void platform_sdl_present(void)",
    )
    assert "mdkr_vita_lan_update();" in frame_service
    frame_sync = between(
        platform,
        "static void platform_frame_sync_impl(",
        "void platform_frame_sync(void)",
    )
    assert frame_sync.index("platform_vita_frame_service();") < frame_sync.index(
        "if (platform_exit_requested())"
    )

    assert "dl_fresh && (!subloop || endpoint_drew)" in scheduler
    assert "platform_frame_sync_no_swap();" in scheduler
    print("test_vita_lan_stall_presentation_contract: PASS")


if __name__ == "__main__":
    main()
