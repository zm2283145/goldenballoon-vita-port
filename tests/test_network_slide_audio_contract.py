#!/usr/bin/env python3
"""Guard confirmed-state ownership of multiplayer racer slide loops."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin + len(start))]


def main() -> None:
    audio = (ROOT / "game/src/audio.c").read_text(encoding="utf-8")
    vehicle = (ROOT / "game/src/audio_vehicle.c").read_text(encoding="utf-8")
    racer = (ROOT / "game/src/racer.c").read_text(encoding="utf-8")
    runtime = (
        ROOT / "platform/rollback/rollback_game_runtime.c"
    ).read_text(encoding="utf-8")
    main_loop = (ROOT / "game/src/thread3_main.c").read_text(encoding="utf-8")

    presentation_play = section(
        audio, "void sound_play_presentation(", "void sound_play_spatial(")
    assert "sound_play_table_raw(" in presentation_play
    assert "GAMEPLAY_EVENT_TRACE(" not in presentation_play
    assert "rollback_sound_request(" not in presentation_play

    capture = section(
        vehicle,
        "void racer_sound_capture_confirmed_network_slides(",
        "void racer_sound_update_network_slides(",
    )
    assert "confirmedMask & (1u << slot)" in capture
    assert "sNetworkSlidePresentation[slot]" in capture

    update = section(
        vehicle,
        "void racer_sound_update_network_slides(",
        "void racer_sound_stop_active(",
    )
    assert "sNetworkSlidePresentation[slot]" in update
    assert "sound_play_presentation(" in update
    assert "get_racer_objects(" not in update

    drift = section(
        racer, "// sounds for drifting and sliding", "// Velocity of steering input"
    )
    assert "networkSlideSound" not in drift
    assert "audspat_play_sound_at_position(" not in drift

    voice = section(
        racer,
        "void play_random_character_voice(",
        "f32 handle_racer_top_speed(",
    )
    assert "mdkr_net_roster_runtime_active()" in voice
    assert voice.index("chooseVoice &&") < voice.index("rand_range(0, range - 1)")
    assert (
        voice.index("if (tempRacer->soundMask == NULL)")
        < voice.index("audspat_play_sound_at_position(")
    )

    assert "racer_sound_capture_confirmed_network_slides(" in runtime
    assert (
        main_loop.index("racer_sound_capture_confirmed_network_slides(")
        < main_loop.index("racer_sound_update_network_slides(")
    )
    print("test_network_slide_audio_contract: PASS")


if __name__ == "__main__":
    main()
