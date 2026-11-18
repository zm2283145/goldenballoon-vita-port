#!/usr/bin/env python3
"""Guard weapon-hit rollback authority and presentation isolation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin + len(start))]


def main() -> None:
    racer = (ROOT / "game/src/racer.c").read_text(encoding="utf-8")
    weapons = (ROOT / "game/src/object_functions.c").read_text(encoding="utf-8")
    objects = (ROOT / "game/src/objects.c").read_text(encoding="utf-8")
    view = (ROOT / "game/src/rollback_authority_view.c").read_text(
        encoding="utf-8"
    )
    authority = (
        ROOT / "platform/rollback/rollback_game_authority.c"
    ).read_text(encoding="utf-8")
    runtime = (
        ROOT / "platform/rollback/rollback_game_runtime.c"
    ).read_text(encoding="utf-8")

    projectile = section(
        weapons, "void weapon_projectile(", "void rocket_prevent_overshoot("
    )
    assert "racer->attackType = ATTACK_EXPLOSION;" in projectile
    assert projectile.index("racer->attackType = ATTACK_EXPLOSION;") < (
        projectile.index("free_object(obj);")
    )

    attack = section(
        racer,
        "void racer_attack_handler_car(",
        "void update_onscreen_AI_racer(",
    )
    assert "play_random_character_voice(" in attack
    assert "racer->unk18C = 360;" in attack
    assert "racer->attackType = ATTACK_NONE;" in attack
    assert "tempRacer->unk1D1 = rand_range(-80, 80);" in racer
    assert "gCurrentStickX += tempRacer->unk1D1;" in racer

    voice = section(
        racer,
        "void play_random_character_voice(",
        "f32 handle_racer_top_speed(",
    )
    assert "mdkr_net_roster_runtime_active()" in voice
    assert "chooseVoice &&" in voice
    assert voice.index("chooseVoice &&") < voice.index(
        "if (tempRacer->soundMask == NULL)"
    )

    trail = section(
        weapons,
        "void play_rocket_trailing_sound(",
        "void weapon_trap(",
    )
    assert "mdkr_rollback_game_runtime_presentation_allowed()" in trail
    destroy = section(objects, "void obj_destroy(", "void obj_update(")
    assert "mdkr_rollback_game_runtime_presentation_allowed()" in destroy
    free = section(objects, "void free_object(", "void gParticlePtrList_flush(")
    assert (
        "GAMEPLAY_EVENT_DESPAWN, object->objectID, object->behaviorId" in free
    )

    for field in (
        "racer->attackType",
        "racer->spinout_timer",
        "racer->shieldTimer",
        "object->properties.projectile.timer",
        "weapon->weaponID",
        "weapon->owner",
        "weapon->target",
    ):
        assert field in view

    assert "MDKR_ROLLBACK_TAG_OBJECT_POOL_BACKING" in authority
    assert "gFreeListCount" in authority
    assert "objects.object_list" in authority
    assert "D_8011D544" in authority
    assert "[ROLLBACK-ITEM]" in runtime
    assert "[ROLLBACK-DIGEST]" in runtime
    print("test_online_weapon_rollback_contract: PASS")


if __name__ == "__main__":
    main()
