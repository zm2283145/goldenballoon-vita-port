#!/usr/bin/env python3
"""Rendered proof for the independent custom-character browser.

The fixture is generated and installed into a temporary catalog, so this gate
never needs a community asset and never changes the player's character library.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_manifest_wizard as wizard  # noqa: E402
from test_character_asset_probe import make_animated_glb, make_portrait_png  # noqa: E402


def command_ok(command: list[str], *, cwd: Path) -> tuple[bool, str]:
    process = subprocess.run(
        command, cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=90, check=False,
    )
    return process.returncode == 0, process.stdout or ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--build", "--build-dir", dest="build_dir",
                        type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    binary = (args.binary.resolve() if args.binary is not None
              else Path(resolve_binary(args.build_dir)).resolve())
    rom = args.rom.resolve()
    if binary is None or not binary.is_file() or not rom.is_file():
        print("check_custom_character_roster: FAIL -- missing binary or ROM",
              file=sys.stderr)
        return 2

    temporary = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr-custom-roster-")
        evidence = Path(temporary.name)
    else:
        evidence = args.evidence_dir.resolve()
        evidence.mkdir(parents=True, exist_ok=True)
    source = evidence / "source"
    characters = evidence / "characters"
    frames = evidence / "frames"
    source.mkdir(parents=True, exist_ok=True)
    frames.mkdir(parents=True, exist_ok=True)

    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "roster-proof.mdkrchar"
    model.write_bytes(make_animated_glb())
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, "org.mdkr.roster-proof", "\u03a1\u03cc\u03c3\u03c4\u03b5\u03c1 \u0394\u03bf\u03ba\u03b9\u03bc\u03ae", "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/roster-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[80, 180, 240],
    )
    manifest["identity"].update(
        short_name="\u0414\u0438\u043a\u0441\u0438",
        narration_name="\u03a1\u03cc\u03c3\u03c4\u03b5\u03c1 \u0394\u03bf\u03ba\u03b9\u03bc\u03ae",
        sort_label="Roster Proof",
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")

    failures: list[str] = []
    ok, output = command_ok([
        sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
        "pack", "--model", str(model), "--manifest", str(manifest_path),
        "--license", str(license_path), "--portrait", str(portrait),
        "--output", str(package),
    ], cwd=ROOT)
    if not ok:
        failures.append("license-clean package generation failed")
    if ok:
        ok, install_output = command_ok([
            sys.executable,
            str(ROOT / "tests" / "run_character_manager_fixture.py"),
            "--directory", str(characters), "install", str(package),
        ], cwd=ROOT)
        output += install_output
        if not ok:
            failures.append("temporary catalog install failed")

    script = evidence / "open-custom-roster.txt"
    script.write_text(
        "1250 START 4\n1330 START 4\n1500 R 4\n",
        encoding="utf-8",
    )
    if not failures:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
            MDKR_RENDERER="webgpu", MDKR_RENDER_SCALE="1",
            MDKR_VIDEO_CONFIG_PATH=os.devnull,
            MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
            MDKR_DUMP_FROM="1538", MDKR_DUMP_EVERY="10000",
            MDKR64_HIDDEN="1",
        )
        process = subprocess.run([
            str(binary), "--headless-frames", "1540", "--input-script",
            str(script), "--dump-frames", str(frames), "--rom", str(rom),
            "--window-size", "1280x960", "--restored",
        ], cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=90, check=False)
        output += process.stdout or ""
        if process.returncode != 0:
            failures.append(f"game exited with {process.returncode}")

    (evidence / "run.log").write_text(output, encoding="utf-8")
    for marker in ("[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            failures.append(f"fatal marker {marker}")
    if "catalog directory override:" not in output:
        failures.append("isolated catalog override was not honored")
    if "custom_roster: catalog=1 visible=1 rejected=0 pages=1" not in output:
        failures.append("generated package did not reach the roster model")
    for context in ("tile", "display"):
        witness = (
            "custom_character_name: context=" + context +
            " package=org.mdkr.roster-proof mode=native reason=none"
        )
        if witness not in output:
            failures.append(
                f"{context} did not use the ROM-independent native glyph path"
            )

    dumps = sorted(frames.glob("frame_*.ppm"))
    if len(dumps) != 1:
        failures.append(f"expected one browser capture, got {len(dumps)}")
    else:
        width, height, pixels = read_ppm(dumps[0])
        if width % 320 or height % 240 or width * 3 != height * 4:
            failures.append("browser capture has the wrong presentation shape")
        else:
            scale = width // 320

            def pixel(x: int, y: int) -> tuple[int, int, int]:
                offset = ((y * scale) * width + x * scale) * 3
                return tuple(pixels[offset:offset + 3])  # type: ignore[return-value]

            if max(pixel(2, 2)) > 36:
                failures.append("modal browser did not fully cover donor UI")
            portrait_colours = {
                pixel(x, y) for y in range(48, 82, 4)
                for x in range(143, 178, 4)
            }
            if len(portrait_colours) < 12:
                failures.append("catalog portrait was absent or visually empty")

    if failures:
        print("check_custom_character_roster: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"  evidence: {evidence}", file=sys.stderr)
        return 1
    print("check_custom_character_roster: PASS -- isolated package catalog, "
          "real R-button route, native Greek/Cyrillic names, centered identity "
          "portrait, and modal layout")
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
