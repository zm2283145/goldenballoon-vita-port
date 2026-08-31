#!/usr/bin/env python3
"""Prove package identity survives a real race into minimap and results."""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import make_animated_glb  # noqa: E402
from character_validation_fixture import accepted_character_validation  # noqa: E402


PACKAGE_ID = "org.mdkr.identity-surface-proof"
DISPLAY_NAME = "Identity Surface Proof"
MINIMAP_RGB = (17, 231, 83)
RACE_CAPTURE = 5000
CAPTURE_INTERVAL = 50
FINAL_FRAME = 5500


class IdentitySurfaceError(RuntimeError):
    pass


def make_identity_portrait(size: int = 40) -> bytes:
    """Return a distinctive, dependency-free RGBA PNG test card."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
            ">I", checksum
        )

    colours = (
        (240, 30, 200, 255),
        (20, 230, 240, 255),
        (245, 210, 25, 255),
        (25, 220, 70, 255),
    )
    scanlines = bytearray()
    for y in range(size):
        scanlines.append(0)
        for x in range(size):
            quadrant = (2 if y >= size // 2 else 0) + (
                1 if x >= size // 2 else 0
            )
            scanlines.extend(colours[quadrant])
    return (
        probe.PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(scanlines)))
        + chunk(b"IEND", b"")
    )


def install_fixture(root: Path) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "identity-surface-proof.mdkrchar"
    model.write_bytes(make_animated_glb())
    portrait.write_bytes(make_identity_portrait())
    manifest, _ = wizard.build_manifest(
        model,
        PACKAGE_ID,
        DISPLAY_NAME,
        "CC0-1.0",
        "Generated MDKR identity-surface fixture",
        "https://example.invalid/identity-surface-proof",
        "bumper",
        ["car", "hovercraft", "plane"],
        portrait=portrait,
        minimap_rgb=list(MINIMAP_RGB),
    )
    manifest["identity"].update({
        "short_name": "Identity Proof",
        "narration_name": "Identity Surface Proof character",
        "sort_label": "Proof, Identity Surface",
    })
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


def make_race_script(path: Path) -> None:
    """Navigate to Time Trial, then advance its post-race flow at R=2."""
    events = [
        "1250 START 4",
        "1330 START 4",
        "1450 A 4",
        "1540 A 4",
        "1750 A 4",
        "1900 DOWN 4",
        "2000 A 4",
        "2150 A 4",
        "2320 A 4",
        "2400 DOWN 6",
        "2460 A 4",
        "2600 A 4",
    ]
    events.extend(f"{frame} A 5" for frame in range(5300, FINAL_FRAME, 24))
    path.write_text("\n".join(events) + "\n", encoding="utf-8")


def result_card_pixels(path: Path) -> list[tuple[int, int, int]]:
    width, height, pixels = read_ppm(path)
    values: list[tuple[int, int, int]] = []
    # Single-player race-times card at the retail stopwatch portrait anchor.
    for y in range(int(height * 0.72), int(height * 0.88)):
        for x in range(int(width * 0.105), int(width * 0.215)):
            offset = (y * width + x) * 3
            values.append(tuple(pixels[offset:offset + 3]))
    return values


def validate_result_card(path: Path) -> None:
    pixels = result_card_pixels(path)
    # The authored proof card intentionally consists of four flat quadrants;
    # fewer than four colours means it was absent or clipped away.
    if len(set(pixels)) < 4:
        raise IdentitySurfaceError("custom result-card region is visually empty")
    classes = {
        "magenta": sum(r > 100 and b > 80 and r > g * 1.8
                       for r, g, b in pixels),
        "cyan": sum(g > 90 and b > 90 and r < g * 0.65
                    for r, g, b in pixels),
        "yellow": sum(r > 100 and g > 80 and b < g * 0.55
                      for r, g, b in pixels),
        "green": sum(g > 90 and g > r * 1.7 and g > b * 1.7
                     for r, g, b in pixels),
    }
    minimum = max(8, len(pixels) // 25)
    missing = [name for name, count in classes.items() if count < minimum]
    if missing:
        raise IdentitySurfaceError(
            "custom result card lost authored quadrants: "
            f"counts={classes}, region={len(pixels)}"
        )


def validate_minimap_signal(path: Path) -> None:
    width, height, pixels = read_ppm(path)
    matches = 0
    # Ancient Lake's minimap and local-player arrow occupy the lower-right
    # presentation region. Restrict the pixel witness so similarly green world
    # art cannot satisfy the authored marker-colour contract.
    for y in range(int(height * 0.55), int(height * 0.95)):
        for x in range(int(width * 0.82), int(width * 0.98)):
            offset = (y * width + x) * 3
            red, green, blue = pixels[offset:offset + 3]
            matches += red < 55 and green > 175 and 45 < blue < 125
    if matches < 2:
        raise IdentitySurfaceError(
            "race capture contains no visible authored minimap-colour signal"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve()
    if not binary.is_file() or not rom.is_file():
        print(
            "check_custom_character_identity_surfaces: FAIL -- missing "
            "binary or ROM",
            file=sys.stderr,
        )
        return 2

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(
            prefix="mdkr-character-identity-surfaces-"
        )
        root = Path(temporary.name)
    else:
        root = args.evidence_dir.resolve()
        root.mkdir(parents=True, exist_ok=True)
    frames = root / "frames"
    saves = root / "saves"
    script = root / "identity-surface-race.txt"
    frames.mkdir()
    saves.mkdir()
    try:
        characters = install_fixture(root)
        make_race_script(script)
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        environment.update({
            "LC_ALL": "C",
            "MDKR_AUDIO": "0",
            "MDKR_AUTOPILOT": "1",
            "MDKR_TRACE": "1",
            "MDKR_RENDERER": "webgpu",
            "MDKR_RENDER_SCALE": "1",
            "MDKR_LOAD_TRACK": "5:0",
            "MDKR_VIDEO_CONFIG_PATH": os.devnull,
            "MDKR_SAVE_DIR": str(saves),
            "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
            "MDKR_CUSTOM_CHARACTER_P1": PACKAGE_ID,
            "MDKR_DUMP_FROM": str(RACE_CAPTURE),
            "MDKR_DUMP_EVERY": str(CAPTURE_INTERVAL),
            "MDKR64_HIDDEN": "1",
        })
        process = subprocess.run(
            [
                str(binary),
                "--headless-frames", str(FINAL_FRAME),
                "--input-script", str(script),
                "--dump-frames", str(frames),
                "--window-size", "1280x960",
                "--rom", str(rom),
            ],
            cwd=ROOT,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
            check=False,
        )
        output = process.stdout or ""
        (root / "run.log").write_text(output, encoding="utf-8")
        if process.returncode != 0:
            raise IdentitySurfaceError(
                f"game exited with {process.returncode}\n{output[-6000:]}"
            )
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer",
                       "runtime error:"):
            if marker in output:
                raise IdentitySurfaceError(f"game emitted {marker}")
        required = (
            re.escape(f"[modern-character] P1={PACKAGE_ID} donor=1"),
            r"custom_roster_commit: player=0 controller=0 package="
            + re.escape(PACKAGE_ID)
            + r" donor=1 revision=[1-9][0-9]*",
            r"custom_character_minimap: player=0 name="
            + re.escape(DISPLAY_NAME)
            + r" revision=[1-9][0-9]* rgb=17,231,83",
            r"custom_character_results_portrait: player=0 name="
            + re.escape(DISPLAY_NAME)
            + r" revision=[1-9][0-9]* source=package-card",
        )
        for pattern in required:
            if re.search(pattern, output) is None:
                raise IdentitySurfaceError(
                    f"missing runtime proof matching {pattern!r}"
                )
        validate_minimap_signal(frames / f"frame_{RACE_CAPTURE}.ppm")
        card_errors: list[str] = []
        for capture in sorted(frames.glob("frame_*.ppm")):
            if capture.name == f"frame_{RACE_CAPTURE}.ppm":
                continue
            try:
                validate_result_card(capture)
                break
            except IdentitySurfaceError as error:
                card_errors.append(f"{capture.name}: {error}")
        else:
            raise IdentitySurfaceError(
                "no captured post-race frame rendered the authored package "
                "card; " + "; ".join(card_errors)
            )
    except (
        OSError,
        subprocess.SubprocessError,
        IdentitySurfaceError,
        probe.ProbeError,
        manager.ManagerError,
    ) as error:
        print(
            f"check_custom_character_identity_surfaces: FAIL -- {error}",
            file=sys.stderr,
        )
        print(f"evidence: {root}", file=sys.stderr)
        return 1
    print(
        "check_custom_character_identity_surfaces: PASS -- generated package "
        "kept its donor gameplay profile while authored minimap colour and "
        "portrait reached a real race and post-race card"
    )
    if args.evidence_dir is not None:
        print(f"evidence: {root}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
