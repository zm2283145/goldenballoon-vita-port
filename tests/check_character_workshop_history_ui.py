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
    make_animated_glb,
    make_portrait_png,
)
from character_validation_fixture import accepted_character_validation  # noqa: E402

PACKAGE_ID = "org.mdkr.history-proof"


def install_fixture(root: Path) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "history-proof.mdkrchar"
    model.write_bytes(make_animated_glb(with_lod=True))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, PACKAGE_ID, "History Proof", "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/history-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[100, 180, 240],
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


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def run_tab(binary: Path, root: Path, characters: Path, tab: str,
            expected: tuple[str, ...], rom: Path | None) -> None:
    tab_root = root / tab
    prefs = tab_root / "prefs"
    saves = tab_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    accessible = tab in ("vehicles", "performance")
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
            "MDKR_A11Y_TRACE": "1",
        })
    process = subprocess.run(
        [str(binary)], cwd=root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{tab} history route exited {process.returncode}\n"
            f"{process.stdout[-8000:]}"
        )
    for tool in expected:
        marker = f"character-history tool={tool} "
        if marker not in process.stdout:
            raise RuntimeError(
                f"{tab} did not render {tool} history controls\n"
                f"{process.stdout[-8000:]}"
            )
    if tab == "vehicles":
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
    if tab == "test":
        marker = (
            "character-pose-inspector package=" + PACKAGE_ID +
            " semantics=13 defaultPose=4 defaultPhase=500 "
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
            r"lodBands=(\d+) inspectionDistance=0 inspectionLod=0 "
            r"monotonic=([01]) dramatic=([01]) "
            r"importCeiling=unchanged history=performance",
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
            "text=Inspection distance", "text=Exact LOD distance bands",
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
        with tempfile.TemporaryDirectory(
                prefix="mdkr-workshop-history-") as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            before = inventory(characters)
            routes = (
                ("identity", ("Identity",)),
                ("rig-motion", ("Rig",)),
                ("vehicles", ("Profile", "Fit")),
                ("performance", ("Performance",)),
                ("test", ("Test setup",)),
            )
            for tab, tools in routes:
                run_tab(binary, root, characters, tab, tools, rom)
            if inventory(characters) != before:
                raise RuntimeError(
                    "rendering history controls mutated installed character bytes"
                )
    except (OSError, RuntimeError, subprocess.SubprocessError,
            probe.ProbeError, manager.ManagerError) as error:
        print(f"check_character_workshop_history_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_workshop_history_ui: PASS -- exact-source Identity, "
          "Profile, Rig, Fit, Performance, Test history, project-owned "
          "accessible donor metric badges, spatial fit/contact controls, "
          "accessible performance targets with runtime-equivalent "
          "LOD assembly math, and all semantic pose inspection controls render "
          "without mutating installed bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
