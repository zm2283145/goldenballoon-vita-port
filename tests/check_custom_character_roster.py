#!/usr/bin/env python3
"""Rendered proof for the independent custom-character browser.

The fixture is generated and installed into a temporary catalog, so this gate
never needs a community asset and never changes the player's character library.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
# Every engine launch below scrubs MDKR* from the inherited environment, which
# also drops the suite's save pin; since the 1.5.2 per-user save unification an
# unpinned engine reads the SHARED per-user save. Pin one private directory for
# the whole run so no arm can read or write the host's real progress.
HERMETIC_SAVE_DIR = Path(tempfile.mkdtemp(prefix="mdkr-hermetic-save-")) / "save"
HERMETIC_SAVE_DIR.mkdir()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_manifest_wizard as wizard  # noqa: E402
from test_character_asset_probe import make_animated_glb, make_portrait_png  # noqa: E402


# The generated fixture portrait (make_portrait_png) is an exact RGBA ramp:
# red tracks x, green tracks y, and blue is the constant plane 160. That plane
# is what proves a portrait was actually blitted -- mere colour variety is not,
# because the animated character scene behind the chip supplies plenty of it.
FIXTURE_PORTRAIT_BLUE = 160
FIXTURE_PORTRAIT_BLUE_TOLERANCE = 8


def portrait_signature(sample: "list[tuple[int, int, int]]") -> tuple[float, int]:
    """Return the share of samples on the fixture blue plane and the colour count."""
    if not sample:
        return 0.0, 0
    on_plane = sum(
        1 for _, _, blue in sample
        if abs(blue - FIXTURE_PORTRAIT_BLUE) <= FIXTURE_PORTRAIT_BLUE_TOLERANCE
    )
    return on_plane / len(sample), len(set(sample))


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
    menu_source = (ROOT / "game" / "src" / "menu.c").read_text(
        encoding="utf-8"
    )
    preview_start = menu_source.find("s32 mdkr_workshop_preview_prepare(")
    preview_end = menu_source.find("\n}\n#endif", preview_start)
    if preview_start < 0 or preview_end < 0:
        failures.append("Workshop Preview prepare seam is unavailable")
    else:
        preview_source = menu_source[preview_start:preview_end]
        guard_at = preview_source.find("adventure_party_runtime_is_active()")
        mutation_at = preview_source.find("reset_character_id_slots()")
        if guard_at < 0 or mutation_at < 0 or guard_at > mutation_at:
            failures.append(
                "active Adventure Party guard does not precede preview mutation"
            )
        refusal = "aparty_workshop_preview_refused: reason=active_session"
        if refusal not in preview_source:
            failures.append("party-active preview refusal has no diagnostic")
    if '"NO ART", ALIGN_MIDDLE_CENTER' not in menu_source:
        failures.append("missing portrait does not disclose NO ART in the roster")

    # The launcher room screen no longer chooses characters, so the online
    # appearance disclosure has to live on the select screen. An online match
    # cannot be driven from this offline gate -- the roster runtime is armed by
    # a validated match manifest, with no env override -- so the seam is pinned
    # in source, the way the preview guard and NO ART arms above are.
    seats_start = menu_source.find("s32 customSeats = 0;")
    seats_end = menu_source.find('"R: CUSTOM RACERS"', seats_start)
    if seats_start < 0 or seats_end < 0:
        failures.append("custom seat name row seam is unavailable")
    else:
        seat_source = menu_source[seats_start:seats_end]
        disclosure = '"LOCAL LOOK ONLY - OTHERS SEE BUILT-IN RACER"'
        if disclosure not in seat_source:
            failures.append(
                "online seats do not disclose that the appearance is local"
            )
        elif not (0 <= seat_source.find("mdkr_net_roster_runtime_active()")
                  < seat_source.find(disclosure)):
            failures.append(
                "local-appearance disclosure is not gated on an online session"
            )
        elif "customSeats > 0" not in seat_source:
            failures.append(
                "local-appearance disclosure is drawn without a custom pick"
            )

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
        "1250 START 4 P1\n"
        "1330 START 4 P1\n"
        "1420 A 4 P2\n"
        "1500 R 4 P1\n"
        "1520 RIGHT 4 P2\n"
        "1580 A 4 P1\n",
        encoding="utf-8",
    )
    if not failures:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
            MDKR_RENDERER="webgpu", MDKR_RENDER_SCALE="1",
            MDKR_VIDEO_CONFIG_PATH=os.devnull,
            MDKR_SAVE_DIR=str(HERMETIC_SAVE_DIR),
            MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
            MDKR_DUMP_FROM="1538", MDKR_DUMP_EVERY="120",
            MDKR64_HIDDEN="1",
        )
        process = subprocess.run([
            str(binary), "--headless-frames", "1660", "--input-script",
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
    if "custom_roster_input_passthrough: controller=1" not in output:
        failures.append(
            "P2 selection input was not serviced while P1 browsed the roster"
        )
    selected_match = re.search(
        r"custom_character_name: context=selected player=0 "
        r"package=org\.mdkr\.roster-proof mode=native reason=none .*"
        r"width=(\d+) truncated=1",
        output,
    )
    if selected_match is None or int(selected_match.group(1)) > 34:
        failures.append(
            "selected-player label did not use bounded native UTF-8 "
            "ellipsis fitting"
        )
    if "custom_character_portrait: player=0" not in output:
        failures.append("selected-player portrait was not published")

    dumps = sorted(frames.glob("frame_*.ppm"))
    if len(dumps) != 2:
        failures.append(
            f"expected browser and selected-player captures, got {len(dumps)}"
        )
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
            plane, colours = portrait_signature([
                pixel(x, y) for y in range(48, 82, 4)
                for x in range(143, 178, 4)
            ])
            if plane < 0.9 or colours < 12:
                failures.append(
                    "catalog portrait tile did not carry the fixture image "
                    f"(blue plane {plane:.0%}, {colours} colours)"
                )

        width, height, pixels = read_ppm(dumps[1])
        if width % 320 or height % 240 or width * 3 != height * 4:
            failures.append(
                "selected-player capture has the wrong presentation shape"
            )
        else:
            scale = width // 320

            def selected_pixel(x: int, y: int) -> tuple[int, int, int]:
                offset = ((y * scale) * width + x * scale) * 3
                return tuple(pixels[offset:offset + 3])  # type: ignore[return-value]

            plane, colours = portrait_signature([
                selected_pixel(x, y) for y in range(151, 186, 4)
                for x in range(5, 40, 4)
            ])
            if plane < 0.9 or colours < 12:
                failures.append(
                    "selected-player 40x40 portrait chip did not carry the "
                    f"fixture image (blue plane {plane:.0%}, {colours} colours)"
                )

    if failures:
        print("check_custom_character_roster: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"  evidence: {evidence}", file=sys.stderr)
        return 1
    print("check_custom_character_roster: PASS -- isolated package catalog, "
          "real R-button route, concurrent P2 input, bounded native names, "
          "catalog and selected-player portraits, and modal layout")
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
