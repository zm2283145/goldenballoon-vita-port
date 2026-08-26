#!/usr/bin/env python3
"""Exact-game context gate for Character Workshop preview requests."""

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
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_manifest_wizard as wizard  # noqa: E402
from test_character_asset_probe import make_animated_glb, make_portrait_png  # noqa: E402


PACKAGE_ID = "org.mdkr.context-proof"
FRAMES = 180


def run(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=90, check=False,
    )


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
    if not binary.is_file() or not rom.is_file():
        print("check_custom_character_workshop_preview: FAIL -- missing binary or ROM",
              file=sys.stderr)
        return 2

    temporary = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr-context-preview-")
        evidence = Path(temporary.name)
    else:
        evidence = args.evidence_dir.resolve()
        evidence.mkdir(parents=True, exist_ok=True)
    source = evidence / "source"
    characters = evidence / "characters"
    source.mkdir(parents=True, exist_ok=True)

    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "context-proof.mdkrchar"
    model.write_bytes(make_animated_glb())
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, PACKAGE_ID, "Context Proof", "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/context-proof",
        "bumper", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[90, 210, 140],
    )
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")

    failures: list[str] = []
    output = ""
    packed = run([
        sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
        "pack", "--model", str(model), "--manifest", str(manifest_path),
        "--license", str(license_path), "--portrait", str(portrait),
        "--output", str(package),
    ])
    output += packed.stdout or ""
    if packed.returncode != 0:
        failures.append("license-clean package generation failed")
    if not failures:
        installed = run([
            sys.executable,
            str(ROOT / "tools" / "character_package_manager.py"),
            "--directory", str(characters), "install", str(package),
        ])
        output += installed.stdout or ""
        if installed.returncode != 0:
            failures.append("temporary catalog install failed")

    arms = [
        ("select", 1, True, None, None, False),
        ("car", 1, False, None, None, False),
        ("hovercraft", 1, False, None, None, False),
        ("plane", 1, False, None, None, False),
        ("car", 3, False, None, None, False),
        ("car", 4, True, None, None, False),
        ("car", 1, False, "select.idle", "250", False),
        ("car", 1, False, "race.finish_win", "750", True),
    ]
    arm_draws: dict[str, int] = {}
    captures: dict[str, Path] = {}
    reported_dimensions: dict[str, tuple[int, int, int, int]] = {}
    comparison_environment: tuple[str, str, str, str, str] | None = None
    if not failures:
        for (context, players, capture, pose, pose_phase,
             expect_fallback) in arms:
            label = (f"{context}-{players}p" if pose is None else
                     f"{context}-{players}p-pose" +
                     ("-fallback" if expect_fallback else ""))
            arm_dir = evidence / label
            arm_dir.mkdir(parents=True, exist_ok=True)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("MDKR", "GE007_"))}
            env.update(
                LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                MDKR_PRESENT_PERF="1",
                MDKR_RENDERER="webgpu", MDKR_RENDER_SCALE="1",
                MDKR_VIDEO_CONFIG_PATH=os.devnull,
                MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
                MDKR_CHARACTER_WORKSHOP_PREVIEW=context,
                MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS=str(players),
                MDKR64_HIDDEN="1",
            )
            for player in range(players):
                env[f"MDKR_CUSTOM_CHARACTER_P{player + 1}"] = PACKAGE_ID
            if pose is not None and pose_phase is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE"] = pose
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE"] = pose_phase
            command = [
                str(binary), "--headless-frames", str(FRAMES), "--rom",
                str(rom), "--window-size", "1280x960", "--restored",
            ]
            if capture:
                env.update(MDKR_DUMP_FROM=str(FRAMES - 2),
                           MDKR_DUMP_EVERY="10000")
                command.extend(["--dump-frames", str(arm_dir)])
            process = run(command, env=env)
            arm_output = process.stdout or ""
            output += f"\n===== {label} =====\n{arm_output}"
            if process.returncode != 0:
                failures.append(f"{label} exited with {process.returncode}")
                continue
            expected = (f"character_workshop_preview: started context={context} "
                        f"players={players}")
            if expected not in arm_output:
                failures.append(f"{label} did not enter its direct context")
            if pose is not None:
                expected_pose = 12 if expect_fallback else 1
                expected_phase = 750 if expect_fallback else 250
                pose_match = re.search(
                    rf"pose={expected_pose} phase={expected_phase} "
                    r"poseTicks=(\d+) poseFallback=(\d+)",
                    arm_output,
                )
                if pose_match is None or int(pose_match.group(1)) <= 0:
                    failures.append(
                        f"{label} did not drive the held semantic phase"
                    )
                elif expect_fallback:
                    if int(pose_match.group(2)) != int(pose_match.group(1)):
                        failures.append(
                            f"{label} did not report complete source fallback"
                        )
                elif int(pose_match.group(2)) != 0:
                    failures.append(
                        f"{label} unexpectedly used source fallback"
                    )
            if "donor0=1" not in arm_output:
                failures.append(f"{label} lost the non-Diddy donor profile")
            match = re.search(
                r"\[WGPU-MODERN-CHARACTER\] assetUploads=(\d+) draws=(\d+) "
                r"triangles=(\d+) refusedDraws=(\d+)", arm_output)
            if match is None:
                failures.append(f"{label} emitted no renderer accounting")
            else:
                uploads, draws, triangles, refused = map(int, match.groups())
                arm_draws[label] = draws
                if uploads != 1 or draws <= 0 or triangles <= 0 or refused != 0:
                    failures.append(f"{label} did not render one clean shared asset")
            for marker in ("[FATAL]", "AddressSanitizer", "runtime error:"):
                if marker in arm_output:
                    failures.append(f"{label} reported {marker}")
            result_match = re.search(
                r"character_workshop_result: warmup=(\d+) realtime=(\d+) "
                r"samples=(\d+).*replacements=(\d+)", arm_output)
            if result_match is None:
                failures.append(f"{label} emitted no bounded result")
            else:
                warmup, realtime, samples, replacements = map(
                    int, result_match.groups())
                if warmup != 1 or realtime != 0 or samples < 40:
                    failures.append(f"{label} result did not isolate a synthetic post-warmup sample")
                if replacements <= 0:
                    failures.append(f"{label} result counted no package replacements")
            environment_match = re.search(
                r"character_workshop_result: .* backend=(webgpu-[^ ]+) "
                r"adapter=(.*?) driver=(.*?) vendor=([0-9a-f]{8}) "
                r"device=([0-9a-f]{8}) output=(\d+)x(\d+) "
                r"render=(\d+)x(\d+)",
                arm_output,
            )
            if environment_match is None:
                failures.append(
                    f"{label} emitted no exact GPU/output comparison identity"
                )
            else:
                (backend, adapter, driver, vendor, device, output_w,
                 output_h, render_w, render_h) = environment_match.groups()
                if backend == "webgpu-unknown" or adapter == "unknown":
                    failures.append(f"{label} did not identify its WebGPU adapter")
                environment = (backend, adapter, driver, vendor, device)
                if comparison_environment is None:
                    comparison_environment = environment
                elif environment != comparison_environment:
                    failures.append(
                        f"{label} changed GPU identity between exact test arms"
                    )
                dimensions = tuple(
                    map(int, (output_w, output_h, render_w, render_h))
                )
                reported_dimensions[label] = dimensions
                if (dimensions[0:2] != dimensions[2:4] or
                        dimensions[0] < 1280 or dimensions[1] < 960 or
                        dimensions[0] * 3 != dimensions[1] * 4):
                    failures.append(
                        f"{label} comparison dimensions were incoherent: "
                        f"{dimensions!r}"
                    )
                elif len(set(reported_dimensions.values())) != 1:
                    failures.append(
                        f"{label} changed physical dimensions between exact "
                        "test arms"
                    )
            if capture:
                dumps = sorted(arm_dir.glob("frame_*.ppm"))
                if len(dumps) != 1:
                    failures.append(f"{label} produced {len(dumps)} captures")
                else:
                    captures[label] = dumps[0]

    rejection_arms = [
        ("invalid-context", "boat", "1", True, None, None, None,
         "invalid Character Workshop context: boat"),
        ("invalid-players", "car", "0", True, None, None, None,
         "invalid Character Workshop player count: 0"),
        ("missing-assignment", "car", "1", False, None, None, None,
         "Character Workshop package assignment is unavailable"),
        ("unsupported-vehicle", "plane", "1", True, "1", None, None,
         "Character Workshop package assignment is unavailable"),
        ("invalid-pose", "car", "1", True, None, "race.dance", "500",
         "invalid Character Workshop pose request"),
        ("invalid-pose-phase", "car", "1", True, None, "race.steer", "1001",
         "invalid Character Workshop pose request"),
        ("unpaired-pose", "car", "1", True, None, "race.steer", None,
         "pose and phase must be provided together"),
    ]
    if not failures:
        for (label, context, players, assign, vehicle_mask,
             pose, pose_phase, marker) in rejection_arms:
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("MDKR", "GE007_"))}
            env.update(
                LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                MDKR_RENDERER="webgpu", MDKR_VIDEO_CONFIG_PATH=os.devnull,
                MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
                MDKR_CHARACTER_WORKSHOP_PREVIEW=context,
                MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS=players,
                MDKR64_HIDDEN="1",
            )
            if assign:
                env["MDKR_CUSTOM_CHARACTER_P1"] = PACKAGE_ID
            if vehicle_mask is not None:
                env[f"MDKR_CUSTOM_CHARACTER_PROFILE_{PACKAGE_ID}_VEHICLE_MASK"] = vehicle_mask
            if pose is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE"] = pose
            if pose_phase is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE"] = pose_phase
            process = run([
                str(binary), "--headless-frames", "60", "--rom", str(rom),
                "--window-size", "1280x960", "--restored",
            ], env=env)
            arm_output = process.stdout or ""
            output += f"\n===== {label} =====\n{arm_output}"
            if process.returncode == 0:
                failures.append(f"{label} did not fail closed")
            if marker not in arm_output:
                failures.append(f"{label} did not report its exact refusal")

    if ("car-1p" in arm_draws and "car-4p" in arm_draws and
            arm_draws["car-4p"] <= arm_draws["car-1p"] * 3):
        failures.append("four-player stress did not multiply real character draws")

    for label, capture in captures.items():
        width, height, pixels = read_ppm(capture)
        reported = reported_dimensions.get(label)
        if reported is None or (width, height) != reported[2:4]:
            failures.append(
                f"{label} capture dimensions do not match the engine result"
            )
        if width % 320 or height % 240 or width * 3 != height * 4:
            failures.append(f"{label} capture has the wrong presentation shape")
            continue
        scale = width // 320

        def pixel(x: int, y: int) -> tuple[int, int, int]:
            offset = ((y * scale) * width + x * scale) * 3
            return tuple(pixels[offset:offset + 3])  # type: ignore[return-value]

        colours = {pixel(x, y) for y in range(12, 228, 12)
                   for x in range(12, 308, 12)}
        if len(colours) < 40:
            failures.append(f"{label} capture was visually empty")
        if label == "car-4p":
            divider = [pixel(160, y) for y in range(8, 232, 4)]
            divider += [pixel(x, 120) for x in range(8, 312, 4)]
            dark = sum(max(colour) < 32 for colour in divider)
            if dark < len(divider) // 2:
                failures.append("four-player stress did not render four viewports")

    (evidence / "run.log").write_text(output, encoding="utf-8")
    if failures:
        print("check_custom_character_workshop_preview: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(f"  evidence: {evidence}", file=sys.stderr)
        return 1
    print(
        "check_custom_character_workshop_preview: PASS -- direct "
        "select/car/hovercraft/plane routes, exact semantic-phase inspection "
        "with honest fallback accounting, one-to-four-player WebGPU stress, "
        "and fail-closed invalid requests"
    )
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
