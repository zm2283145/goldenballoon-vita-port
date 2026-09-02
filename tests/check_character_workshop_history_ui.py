#!/usr/bin/env python3
"""ROM-free rendered routing proof for source-bound Workshop history."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
import character_asset_probe as probe  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_humanoid_glb,
    make_portrait_png,
)
from character_validation_fixture import accepted_character_validation  # noqa: E402

PACKAGE_ID = "org.mdkr.history-proof"


def verify_status_capture_scope() -> None:
    source = (ROOT / "platform" / "app" / "ui_settings.cpp").read_text(
        encoding="utf-8"
    )
    setter = source[source.index("void setStatus("):
                    source.index("void drawWorkshopStatusHistory()")]
    if "g_workshopStatusCaptureDepth == 0u" not in setter:
        raise RuntimeError(
            "Workshop history is not isolated from unrelated settings status"
        )
    for function in (
        "bool Settings_activateCharacterWorkshopPrimaryAction()",
        "bool Settings_drawCharacterWorkshop(",
        "void Settings_serviceCharacterWork()",
        "SettingsCharacterStudioFrame Settings_drawCharacterOffsetStudio(",
    ):
        start = source.index(function)
        if "WorkshopStatusCapture capture" not in source[start:start + 500]:
            raise RuntimeError(
                f"character status source is outside scoped history: {function}"
            )
    generic = source[source.index("void reportResult("):
                     source.index("bool resultSucceeded(")]
    if "WorkshopStatusCapture" in generic:
        raise RuntimeError(
            "generic video status was incorrectly routed into Workshop history"
        )


def install_fixture(root: Path, *, display_name: str = "History Proof",
                    package_id: str = PACKAGE_ID,
                    source_name: str = "source") -> Path:
    source = root / source_name
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "history-proof.mdkrchar"
    model.write_bytes(make_humanoid_glb(with_lod=True))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, package_id, display_name, "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/history-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[100, 180, 240], rig_mode="humanoid-retarget-v1",
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    probe.build_package(
        model, manifest_path, license_path, package, portrait_path=portrait
    )
    with accepted_character_validation(manager):
        manager.install(package, characters)
    return characters


def run_delete_confirmation(binary: Path, root: Path, characters: Path,
                            *, collision: bool) -> None:
    run_root = root / ("delete-collision" if collision else "delete-unique")
    prefs = run_root / "prefs"
    saves = run_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=package\n",
        encoding="utf-8",
    )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "12",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_SMOKE_CHARACTER_REMOVAL_CONFIRMATION":
            "mdkr64-character-removal-confirmation-v1",
    })
    process = subprocess.run(
        [str(binary)], cwd=run_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    marker = (
        f"character-delete-confirmation package={PACKAGE_ID} "
        f"collision={1 if collision else 0} "
        f"confirm={'package-id' if collision else 'display-name'} "
        "hold-ms=1250 external-source=retained"
    )
    if process.returncode != 0 or marker not in process.stdout:
        raise RuntimeError(
            "friendly deletion confirmation did not render its "
            f"{'collision' if collision else 'unique-name'} policy\n" +
            process.stdout[-8000:]
        )


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def run_gamepad_import_focus(binary: Path, root: Path,
                             characters: Path) -> None:
    tab_root = root / "vehicles-gamepad-import-focus"
    prefs = tab_root / "prefs"
    saves = tab_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=vehicles\n"
        "ui_scale=2.0\n",
        encoding="utf-8",
    )
    (tab_root / "video.ini").write_text(
        "[Accessibility]\nSpeech=1\n", encoding="utf-8"
    )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "40",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(tab_root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_SMOKE_A11Y_WALK": "1",
        "MDKR_APP_SMOKE_INPUT": "gamepad",
        "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
        "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS": "1",
        "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS_TOKEN":
            "mdkr64-character-import-focus-v1",
        "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN": "1",
        "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN_TOKEN":
            "mdkr64-character-workshop-return-v1",
        "MDKR_A11Y_TRACE": "1",
    })
    process = subprocess.run(
        [str(binary)], cwd=tab_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    required = (
        "character import-focus shortcut queued input=gamepad",
        "character-import-focus shortcut=1 mutation=0",
        "text=Browse for another character source",
        "character Workshop return shortcut queued input=gamepad",
        "character-workshop-return shortcut=1 mutation=0",
        "character-workshop-return focus=primary",
    )
    if process.returncode != 0 or any(
            marker not in process.stdout for marker in required):
        raise RuntimeError(
            "controller Back/View could not escape the dense editor and "
            "focus safe character import\n" + process.stdout[-8000:]
        )


def run_tab(binary: Path, root: Path, characters: Path, tab: str,
            expected: tuple[str, ...], rom: Path | None) -> None:
    tab_root = root / tab
    prefs = tab_root / "prefs"
    saves = tab_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    accessible = tab in ("rig-motion", "profile", "vehicles", "performance")
    preferences = (
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        f"character_workshop_last_tab={tab}\n" +
        ("ui_scale=2.0\n" if accessible else "")
    )
    if rom is not None:
        preferences += f"rom_path={rom}\n"
    (prefs / "mdkr64_app.ini").write_text(
        preferences, encoding="utf-8"
    )
    if accessible:
        (tab_root / "video.ini").write_text(
            "[Accessibility]\nSpeech=1\n", encoding="utf-8"
        )
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "520" if accessible else "8",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(tab_root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if accessible:
        environment.update({
            "MDKR_APP_SMOKE_A11Y_WALK": "1",
            "MDKR_APP_SMOKE_INPUT": "keyboard",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS": "1",
            "MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS_TOKEN":
                "mdkr64-character-import-focus-v1",
            "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN": "1",
            "MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN_TOKEN":
                "mdkr64-character-workshop-return-v1",
            "MDKR_A11Y_TRACE": "1",
        })
    process = subprocess.run(
        [str(binary)], cwd=tab_root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{tab} history route exited {process.returncode}\n"
            f"{process.stdout[-8000:]}"
        )
    expected_layout = "compact" if accessible else "wide"
    expected_scale = "2.0" if accessible else "1.0"
    expected_popup = "1" if accessible else "0"
    ux_marker = (
        "character-workshop-ux "
        f"layout={expected_layout} scale={expected_scale} "
        "glyphs=arrows "
        f"tab-list-popup={expected_popup} "
        "readiness-row-links=1 header-next=1 status-history=3 "
        "undo=visible-tool library-next=1 rig-band=1 overlay-guidance=1 "
        "vehicle-fit-use=1 delete=name-or-id+hold controller-port=1 "
        "keyboard-authoring=required "
        "pad-play-setup=complete"
    )
    if ux_marker not in process.stdout:
        raise RuntimeError(
            f"{tab} omitted the complete responsive Workshop UX contract\n"
            + process.stdout[-8000:]
        )
    for tool in expected:
        marker = f"character-history tool={tool} "
        if marker not in process.stdout:
            raise RuntimeError(
                f"{tab} did not render {tool} history controls\n"
                f"{process.stdout[-8000:]}"
            )
    if "character-workshop-primary kind=open-readiness-task " not in process.stdout:
        raise RuntimeError(
            f"{tab} bypassed the selected installed character's readiness "
            "task in the persistent primary action\n" + process.stdout[-8000:]
        )
    if (
        "character-source-secondary-picker rendered=1 focusable=1 "
        "mutation=deferred-review" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} omitted the secondary source picker while the "
            "persistent primary action owned installed-character readiness\n" +
            process.stdout[-8000:]
        )
    if accessible and (
        "text=Browse for another character source" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} keyboard/speech traversal could not reach the secondary "
            "source picker\n" + process.stdout[-8000:]
        )
    if accessible and (
        "character-import-focus shortcut=1 mutation=0" not in process.stdout
    ):
        raise RuntimeError(
            f"{tab} did not route the import-focus shortcut through the "
            "Workshop\n" + process.stdout[-8000:]
        )
    if accessible and any(
        marker not in process.stdout for marker in (
            "character-workshop-return shortcut=1 mutation=0",
            "character-workshop-return focus=primary",
        )
    ):
        raise RuntimeError(
            f"{tab} could not return from its dense editor to the persistent "
            "launcher action row\n" + process.stdout[-8000:]
        )
    if tab == "profile":
        profile_marker = (
            "character-donor-profile-gallery package=" + PACKAGE_ID
            + " profiles=10 glyph=project-owned-metric-badge "
            + f"copyrighted-art=0 exact={1 if rom else 0} "
            "responsive=1 keyboard=1"
        )
        if profile_marker not in process.stdout:
            raise RuntimeError(
                "vehicle route did not render the project-owned donor "
                "profile library\n" + process.stdout[-8000:]
            )
        for spoken in (
            "text=Krunch gameplay profile",
            "text=Diddy gameplay profile",
        ):
            if spoken not in process.stdout:
                raise RuntimeError(
                    "profile keyboard/speech walk missed " + spoken
                    + "\n" + process.stdout[-8000:]
                )
    if tab == "rig-motion":
        marker = (
            "character-animation-intent package=" + PACKAGE_ID +
            " semantics=13 static-detection=1 authored-toggle=1 "
            "disabled-preserved=1 "
            "fallback=reviewed-reference-or-package "
            "inspect-handoff=1 active-revision-guard=1 human-labels=1 "
            "responsive=table-or-cards"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "rig and motion route omitted explicit per-semantic animation "
                "intent\n" + process.stdout[-8000:]
            )
        suggestion_marker = (
            "character-rig-suggestion package=" + PACKAGE_ID +
            " joints=16 roles=16 named=16 hierarchy=0 "
            "common-ancestor-repairs=0 complete=1 structurally-valid=1 "
            "review-required=1"
        )
        if suggestion_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted its complete review-required structural "
                "proposal\n" + process.stdout[-8000:]
            )
        review_marker = (
            "character-rig-review package=" + PACKAGE_ID +
            " anatomy-tasks=5 motion-presets=5 source-bound=1 "
            "reset-on-basis-change=1 exact-test-handoff=1 "
            "context-claim=manual-after-save"
        )
        if review_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted its source-bound anatomy checklist and "
                "exact motion-battery handoff\n" + process.stdout[-8000:]
            )
        motion_marker = (
            "character-motion-authoring constraints=16 secondary-chains=8 "
            "dynamic-joints=64 direct-path=1 presets=4 normalized-axes=1 "
            "source-bound=1 undo=rig-review-reset spoken=1 responsive="
        )
        if motion_marker not in process.stdout:
            raise RuntimeError(
                "rig route omitted bounded direct-manipulation limits and "
                "secondary-chain authoring\n" + process.stdout[-8000:]
            )
        if "text=Apply 16-role humanoid proposal" not in process.stdout:
            raise RuntimeError(
                "keyboard/speech traversal could not reach the structural "
                "proposal action\n" + process.stdout[-8000:]
            )
        if "text=Standing silhouette" not in process.stdout:
            raise RuntimeError(
                "keyboard/speech traversal could not reach the motion battery\n"
                + process.stdout[-8000:]
            )
    if tab == "vehicles":
        marker = (
            "character-spatial-controls package=" + PACKAGE_ID +
            " planes=front,side,top placement=ground-or-seat yaw=context "
            "contacts=4 copy=vehicle-only undo=fit-history"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "vehicle route did not render the synchronized placement, "
                "facing, contact, copy, and undo contract\n" +
                process.stdout[-8000:]
            )
        studio_marker = (
            "character-offset-studio package=" + PACKAGE_ID +
            " contexts=select,car,hovercraft,plane "
            "guided-fit=1 "
            "workflow=preview,measure,fine-tune,evidence,review "
            "exact-rom-preview=1 "
            "inline-exact-still=managed-cache,scene-held-midpoint-auto-return "
            "compact-preview=1 "
            "disabled-package-preview=1 "
            "measured-starting-point=vertical,facing,contacts-least-squares "
            "quality-bands=datum,facing,proportions "
            "camera-occlusion=exact-visual-only "
            "reset=package-anchor "
            "review=current-source-and-fit"
        )
        if studio_marker not in process.stdout:
            raise RuntimeError(
                "offset studio omitted its exact-preview and measured-fit "
                "contract\n" + process.stdout[-8000:]
            )
        facing_columns = 1 if accessible else 4
        facing_marker = (
            "character-facing-studio package=" + PACKAGE_ID +
            " candidates=+z,-z,+x,-x equal-thumbnails=1 "
            f"responsive-columns={facing_columns} available=0 "
            "source-fit-context=current "
            "comparison=1p-select.idle@500-bright "
            "selection=reversible-global-yaw capture=model-alpha exact-rom=1"
        )
        if facing_marker not in process.stdout:
            raise RuntimeError(
                "offset studio omitted its equal exact-renderer facing "
                "candidate workflow\n" + process.stdout[-8000:]
            )
        spoken_controls = [
            "text=Prepare +Z view",
            "text=Continue fit: Character select",
            "text=Exact preview camera layout",
        ]
        if rom is not None:
            spoken_controls.append("text=Open exact Character select preview")
            spoken_controls.append("text=Capture exact still")
        for spoken in spoken_controls:
            if spoken not in process.stdout:
                raise RuntimeError(
                    "offset studio keyboard/speech walk missed " + spoken +
                    "\n" + process.stdout[-8000:]
                )
    if tab == "test":
        marker = (
            "character-pose-inspector package=" + PACKAGE_ID +
            " semantics=13 defaultPose=4 defaultPhase=500 "
            "held-presets=0,500,1000 transition=exact-runtime-bidirectional "
            "transitionDwellMs=1000 transitionCapture=disabled "
            "view=0,0 pitchRange=-90:90 top=exact lighting=0 "
            "capture=scene-or-model-alpha-png-create-only "
            "performanceEvidence=session-excluded"
        )
        if marker not in process.stdout:
            raise RuntimeError(
                "test route did not render the complete session-only pose "
                f"inspector contract\n{process.stdout[-8000:]}"
            )
    if tab == "performance":
        marker = (
            "character-performance-targets package=" + PACKAGE_ID +
            " targets=quality,balanced,performance,four-player custom=1 "
            "sourceBias=0.0 localBias=0.0 players=4 selectedLod=0 "
            "exactAssembly=1"
        )
        transition = re.search(
            r"lodBands=(\d+) inspectionHeight=12\.0 inspectionLod=0 "
            r"monotonic=([01]) dramatic=([01]) "
            r"importCeiling=unchanged history=performance "
            r"projectedPolicy=1 hysteresis=8% fallback=distance "
            r"blendOrder=posed-centroid-per-view "
            r"transparentSelfSort=visual-review "
            r"transparentSceneQueue=visual-review",
            process.stdout,
        )
        if marker not in process.stdout or transition is None or \
                int(transition.group(1)) < 1:
            raise RuntimeError(
                "performance route did not use the exact runtime-equivalent "
                f"target and assembly policy\n{process.stdout[-8000:]}"
            )
        for spoken in (
            "text=Quality", "text=Balanced", "text=Performance",
            "text=Four-player", "text=Authored LOD preference",
            "text=Projected character height",
            "text=Exact projected-height LOD bands",
        ):
            if spoken not in process.stdout:
                raise RuntimeError(
                    "performance keyboard/speech walk missed " + spoken +
                    "\n" + process.stdout[-8000:]
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve() if args.rom is not None else None
    try:
        verify_status_capture_scope()
        with tempfile.TemporaryDirectory(
                prefix="mdkr-workshop-history-") as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            before = inventory(characters)
            routes = (
                ("identity", ("Identity",)),
                ("rig-motion", ("Rig & Motion",)),
                ("profile", ("Gameplay",)),
                ("vehicles", ("Offset Studio",)),
                ("performance", ("Performance",)),
                ("test", ("Test",)),
            )
            for tab, tools in routes:
                run_tab(binary, root, characters, tab, tools, rom)
            run_gamepad_import_focus(binary, root, characters)
            run_delete_confirmation(
                binary, root, characters, collision=False
            )
            if inventory(characters) != before:
                raise RuntimeError(
                    "rendering history controls mutated installed character bytes"
                )
            collision_root = root / "collision-fixture"
            collision_root.mkdir()
            collision_characters = install_fixture(collision_root)
            install_fixture(
                collision_root,
                package_id="org.mdkr.history-proof-twin",
                source_name="source-twin",
            )
            collision_before = inventory(collision_characters)
            run_delete_confirmation(
                binary, collision_root, collision_characters,
                collision=True,
            )
            if inventory(collision_characters) != collision_before:
                raise RuntimeError(
                    "rendering collision-aware deletion confirmation mutated "
                    "installed character bytes"
                )
    except (OSError, RuntimeError, subprocess.SubprocessError,
            probe.ProbeError, manager.ManagerError) as error:
        print(f"check_character_workshop_history_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_workshop_history_ui: PASS -- exact-source Identity, "
          "Profile, Rig, Fit, Performance, Test history, project-owned "
          "accessible donor metric badges, reversible animation intent, "
          "spatial fit/contact controls, "
          "keyboard/controller dense-editor escape routes, "
          "wide and 200% compact tab/readiness/status/shortcut policy, "
          "unique-name and collision-safe hold-to-delete confirmation, "
          "accessible performance targets with runtime-equivalent "
          "LOD assembly math, and all semantic pose inspection controls render "
          "without mutating installed bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
