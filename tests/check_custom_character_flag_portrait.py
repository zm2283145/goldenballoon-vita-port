#!/usr/bin/env python3
"""Real-ROM pixel proof for package portraits on collection-arena flags."""

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

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
from check_adventure_two import eeprom_image  # noqa: E402
from check_challenge_modes import (  # noqa: E402
    BASE_SCRIPT,
    PORTRAIT_DUMP_EVERY,
    PORTRAIT_DUMP_FROM,
    PORTRAIT_MIN_PIXELS_PER_FRAME,
    PORTRAIT_MIN_PIXELS_TOTAL,
    PORTRAIT_WITNESS_FRAMES,
    raster_difference,
)
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    make_portrait_png,
)


PACKAGE_ID = "org.mdkr.flag-portrait-proof"
CUSTOM_FLAG_RE = re.compile(
    r"charflag_bound: playerID=0 characterID=(\d+) texture=ok "
    r"identity=\d+ source=package-card package=" + re.escape(PACKAGE_ID)
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def install_fixture(root: Path) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "flag-portrait-proof.mdkrchar"
    model.write_bytes(make_animated_glb())
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model,
        PACKAGE_ID,
        "Flag Portrait Proof",
        "CC0-1.0",
        "Generated MDKR fixture",
        "https://example.invalid/flag-portrait-proof",
        "bumper",
        ["car", "hovercraft", "plane"],
        portrait=portrait,
        minimap_rgb=[72, 216, 144],
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    probe.build_package(
        model,
        manifest_path,
        license_path,
        package,
        portrait_path=portrait,
    )
    manager.install(package, characters)
    return characters


def make_script(path: Path) -> None:
    path.write_text(BASE_SCRIPT.read_text(encoding="utf-8"), encoding="utf-8")


def run_arm(
    binary: Path,
    rom: Path,
    characters: Path,
    root: Path,
    *,
    suppress: bool,
) -> tuple[str, list[Path]]:
    arm = root / ("suppressed" if suppress else "drawn")
    captures = arm / "frames"
    (arm / "save").mkdir(parents=True)
    captures.mkdir()
    (arm / "save" / "eeprom.bin").write_bytes(eeprom_image(False))
    script = arm / "challenge.txt"
    make_script(script)
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(
        {
            "LC_ALL": "C",
            "MDKR_AUDIO": "0",
            "MDKR_TRACE": "1",
            "MDKR_AUTOPILOT": "1",
            "MDKR_LOAD_TRACK": "11",
            "MDKR_CHALLENGE_OUTCOME": "win",
            "MDKR_RENDERER": "webgpu",
            "MDKR_RENDER_SCALE": "1",
            "MDKR64_HIDDEN": "1",
            "MDKR_VIDEO_CONFIG_PATH": os.devnull,
            "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
            "MDKR_CUSTOM_CHARACTER_P1": PACKAGE_ID,
            "MDKR_DUMP_FROM": str(PORTRAIT_DUMP_FROM),
            "MDKR_DUMP_EVERY": str(PORTRAIT_DUMP_EVERY),
        }
    )
    if suppress:
        environment["MDKR_SUPPRESS_PORTRAITS"] = "1"
    process = subprocess.run(
        [
            str(binary),
            "--headless-frames",
            str(PORTRAIT_WITNESS_FRAMES),
            "--input-script",
            str(script),
            "--dump-frames",
            str(captures),
            "--rom",
            str(rom),
        ],
        cwd=arm,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=300,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{'suppressed' if suppress else 'drawn'} arm exited "
            f"{process.returncode}\n{process.stdout[-8000:]}"
        )
    if BAD_RE.search(process.stdout):
        raise RuntimeError(
            "character-flag arm emitted a fatal diagnostic\n" +
            process.stdout[-8000:]
        )
    if CUSTOM_FLAG_RE.search(process.stdout) is None:
        raise RuntimeError(
            "player-one collection flag did not bind the exact package card\n" +
            process.stdout[-8000:]
        )
    if "custom_character_portrait: player=0 name=Flag Portrait Proof" not in (
        process.stdout
    ):
        raise RuntimeError(
            "package portrait was not materialized through the shared identity "
            "surface\n" + process.stdout[-8000:]
        )
    frames = sorted(captures.glob("frame_*.ppm"))
    if not frames:
        raise RuntimeError("character-flag arm produced no framebuffer witness")
    return process.stdout, frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument(
        "--build",
        "--build-dir",
        dest="build_dir",
        type=Path,
        default=Path(DEFAULT_BUILD_DIR),
    )
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    binary = (
        args.binary.resolve()
        if args.binary is not None
        else Path(resolve_binary(args.build_dir)).resolve()
    )
    rom = args.rom.resolve()
    if not binary.is_file() or not rom.is_file():
        print(
            "check_custom_character_flag_portrait: FAIL -- missing binary or ROM",
            file=sys.stderr,
        )
        return 2

    temporary = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr-custom-flag-")
        evidence = Path(temporary.name)
    else:
        evidence = args.evidence_dir.resolve()
        evidence.mkdir(parents=True, exist_ok=True)
    try:
        characters = install_fixture(evidence)
        installed_before = inventory(characters)
        drawn_log, drawn = run_arm(
            binary, rom, characters, evidence, suppress=False
        )
        suppressed_log, suppressed = run_arm(
            binary, rom, characters, evidence, suppress=True
        )
        drawn_by_name = {path.name: path for path in drawn}
        suppressed_by_name = {path.name: path for path in suppressed}
        if drawn_by_name.keys() != suppressed_by_name.keys():
            raise RuntimeError(
                "drawn and suppressed arms captured different frame sets"
            )
        differences = [
            raster_difference(drawn_by_name[name], suppressed_by_name[name])
            for name in sorted(drawn_by_name)
        ]
        if any(value < PORTRAIT_MIN_PIXELS_PER_FRAME for value in differences):
            raise RuntimeError(
                "a bound package-card flag did not paint a full portrait-shaped "
                f"region: {differences}"
            )
        if sum(differences) < PORTRAIT_MIN_PIXELS_TOTAL:
            raise RuntimeError(
                "package-card flag pixel witness was too small: "
                f"{differences}"
            )
        if inventory(characters) != installed_before:
            raise RuntimeError(
                "collection-arena rendering mutated installed package bytes"
            )
        (evidence / "drawn.log").write_text(drawn_log, encoding="utf-8")
        (evidence / "suppressed.log").write_text(
            suppressed_log, encoding="utf-8"
        )
    except (
        OSError,
        RuntimeError,
        subprocess.SubprocessError,
        probe.ProbeError,
        manager.ManagerError,
    ) as error:
        print(
            f"check_custom_character_flag_portrait: FAIL -- {error}",
            file=sys.stderr,
        )
        print(f"evidence: {evidence}", file=sys.stderr)
        return 1
    print(
        "check_custom_character_flag_portrait: PASS -- generated package "
        "portrait owns the Fire Mountain player flag and paints isolated "
        "WebGPU pixels without donor leakage or installed-byte mutation"
    )
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
