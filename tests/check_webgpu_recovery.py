#!/usr/bin/env python3
"""Exercise WebGPU bring-up, frame-failure, surface, and device recovery.

The lifecycle transition table itself is exhaustively covered by the ROM-free
C test. This integration check proves that the real native backend wires each
class of transition to a bounded action at a complete-frame boundary. Every
injection uses the public MDKR_WEBGPU_FAULT vocabulary. WebGPU may rebuild
WebGPU once, but it must never enter GL unless GL was explicitly selected.
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

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent
SKINNED_PACKAGE_ID = "org.mdkr.webgpu-fault-proof"


class CheckFailure(RuntimeError):
    pass


def build_skinned_fixture(directory: Path) -> Path:
    """Install a generated custom character into a private catalog.

    The custom-character skinned renderer only draws once a Workshop package
    is installed and assigned, so its fault points are unreachable on every
    ROM-only route. The fixture below is generated here (synthetic animated
    GLB plus a synthetic portrait), so this stays ROM-art free and never
    touches the player's real character library.
    """

    sys.path.insert(0, str(ROOT.parent / "tools"))
    sys.path.insert(0, str(ROOT))
    import character_manifest_wizard as wizard  # noqa: E402
    from test_character_asset_probe import (  # noqa: E402
        make_animated_glb,
        make_portrait_png,
    )

    source = directory / "source"
    source.mkdir(parents=True, exist_ok=True)
    catalog = directory / "characters"
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "fault-proof.mdkrchar"
    model.write_bytes(make_animated_glb(volumetric=True))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, SKINNED_PACKAGE_ID, "Fault Proof", "CC0-1.0",
        "Generated MDKR fixture", "https://example.invalid/fault-proof",
        "bumper", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[90, 210, 140],
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    for command in (
        [
            sys.executable, str(ROOT.parent / "tools" / "character_asset_probe.py"),
            "pack", "--model", str(model), "--manifest", str(manifest_path),
            "--license", str(license_path), "--portrait", str(portrait),
            "--output", str(package),
        ],
        [
            sys.executable, str(ROOT / "run_character_manager_fixture.py"),
            "--directory", str(catalog), "install", str(package),
        ],
    ):
        process = subprocess.run(
            command, cwd=ROOT.parent, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=120, check=False,
        )
        if process.returncode != 0:
            raise CheckFailure(
                "custom-character fixture install failed: "
                f"{' '.join(command)}\n{process.stdout}"
            )
    return catalog


def run_case(
    binary: Path,
    rom: Path,
    name: str,
    injected: dict[str, str],
    required: tuple[str, ...],
    forbidden: tuple[str, ...] = (),
    frames: int = 5,
    mode: str = "pure",
    dump_frames: bool = False,
    hidden: bool = True,
    expect_failure: bool = False,
    extra_args: tuple[str, ...] = (),
) -> str:
    always_forbidden = (
        "[CRASH]",
        "[FATAL]",
        "AddressSanitizer",
        "UndefinedBehaviorSanitizer",
        "runtime error:",
        "Validation Error",
        "validation error",
        "Assertion failed",
        "assertion failed",
        "SIGABRT",
        "SIGSEGV",
        "Segmentation fault",
        "Abort trap",
        "[webgpu] device error",
    )
    env = os.environ.copy()
    env.update({"MDKR_RENDERER": "webgpu", "MDKR_AUDIO": "0"})
    if hidden:
        env["MDKR64_HIDDEN"] = "1"
        env.pop("MDKR_TEST_VISIBLE_HEADLESS", None)
    else:
        env.pop("MDKR64_HIDDEN", None)
        env["MDKR_TEST_VISIBLE_HEADLESS"] = "1"
        # A bounded headless loop otherwise runs as fast as the CPU and can
        # finish before the native compositor grants the newly shown window
        # its first drawable. Surface-resource fault arms need a real drawable,
        # so pace them at the historical one-field realtime cadence.
        env["MDKR_PACE_REALTIME"] = "1"
        env["MDKR_SIMULATION_CADENCE"] = "enhanced"
        env["MDKR_SYNTH_FIELDS"] = "1"
    env.update(injected)
    with tempfile.TemporaryDirectory(prefix="mdkr-wgpu-fault-") as temporary:
        case_dir = Path(temporary)
        env["MDKR_SAVE_DIR"] = str(case_dir / "save")
        # Isolate the video config with the save (see check_door_blocks.py).
        env["MDKR_VIDEO_CONFIG_PATH"] = str(case_dir / "save" / "video.ini")
        command = [
            str(binary),
            "--rom",
            str(rom),
            "--headless-frames",
            str(frames),
            f"--{mode}",
        ]
        command.extend(extra_args)
        if dump_frames:
            frame_dir = case_dir / "frames"
            frame_dir.mkdir()
            command.extend(("--dump-frames", str(frame_dir)))
        proc = subprocess.run(
            command,
            cwd=Path(__file__).resolve().parent.parent,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=90,
            check=False,
        )
    output = proc.stdout
    if expect_failure and proc.returncode != 1:
        raise CheckFailure(
            f"{name}: expected clean EXIT_FAILURE (1), got "
            f"{proc.returncode}\n{output[-6000:]}"
        )
    if not expect_failure and proc.returncode != 0:
        raise CheckFailure(
            f"{name}: exited {proc.returncode}\n{output[-6000:]}"
        )
    missing = [text for text in required if text not in output]
    escaped = [
        text for text in (*always_forbidden, *forbidden) if text in output
    ]
    if missing or escaped:
        raise CheckFailure(
            f"{name}: missing={missing} forbidden={escaped}\n{output[-6000:]}"
        )
    print(f"  PASS {name}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--rom", required=True)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.is_file():
        raise CheckFailure(f"binary not found: {binary}")
    if not rom.is_file():
        raise CheckFailure(f"ROM not found: {rom}")

    def completion(frames: int) -> str:
        return f"[SDL] headless: reached {frames} frames, exiting cleanly."

    startup_fail_closed_points = (
        "bringup.instance",
        "bringup.surface",
        "bringup.adapter",
        "bringup.device-defaults",
        "bringup.queue",
        "capabilities.format",
    )
    for point in startup_fail_closed_points:
        run_case(
            binary,
            rom,
            f"{point} -> fail closed",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "stopping without an automatic fallback",
            ),
            ("[SDL] GL ready:", "gfx_init(gl)"),
            expect_failure=True,
        )

    run_case(
        binary,
        rom,
        "raised device limits -> default-limit retry",
        {"MDKR_WEBGPU_FAULT": "bringup.device-limits"},
        (
            "[webgpu-fault] injected bringup.device-limits@1",
            "retrying with default limits",
            "[webgpu] backend initialized",
            completion(5),
        ),
        ("[SDL] GL ready:",),
    )

    run_case(
        binary,
        rom,
        "feature-limited depth adapter",
        {"MDKR_WEBGPU_FAULT": "capabilities.depth-clip-absent"},
        (
            "[webgpu-fault] injected capabilities.depth-clip-absent@1",
            "far-plane-crossing geometry will use the frontend's "
            "homogeneous-z clamp",
            completion(20),
        ),
        ("[SDL] GL ready:",),
        frames=20,
    )

    for point in (
        "capabilities.configure",
        "surface.configure",
        "frame.encoder",
        "frame.pass",
        "frame.finish",
        "queue.submit",
        "surface.error",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> one native reinit",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "attempting one native device reinitialization",
                "native device recovery succeeded; gameplay continues",
                completion(20),
            ),
            ("switched to OpenGL",),
            frames=20,
        )

    run_case(
        binary,
        rom,
        "device loss -> WebGPU reinit",
        {"MDKR_WEBGPU_FAULT": "device.lost"},
        (
            "[webgpu-fault] injected device.lost@1",
            "attempting one native device reinitialization",
            "native device recovery succeeded; gameplay continues",
            completion(20),
        ),
        ("switched to OpenGL",),
        frames=20,
    )

    run_case(
        binary,
        rom,
        "attachment-only surface blit -> device rebuild",
        {
            "MDKR_TEST_WEBGPU_SURFACE_USAGES": "attachment",
            "MDKR_TEST_OCCLUDED_SURFACE_FAULTS": "1",
            "MDKR_WEBGPU_FAULT": "surface.blit-view",
        },
        (
            "[webgpu-fault] injected surface.blit-view@1",
            "native device recovery succeeded; gameplay continues",
            completion(120),
        ),
        ("switched to OpenGL",),
        frames=120,
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "direct-present view failure -> complete offscreen fallback",
        {
            "MDKR_TEST_WEBGPU_DIRECT_VIEW": "1",
            "MDKR_TEST_OCCLUDED_SURFACE_FAULTS": "1",
            "MDKR_RENDER_SCALE": "1",
            "MDKR_REMASTER_FX": "1",
            "MDKR_WEBGPU_FAULT": "surface.direct-view",
        },
        (
            "[webgpu-fault] injected surface.direct-view@1",
            completion(120),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=120,
        mode="remastered",
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "device loss recovery failure -> terminal WebGPU",
        {
            "MDKR_WEBGPU_FAULT": "device.lost",
            "MDKR_TEST_WEBGPU_RECOVERY_FAIL": "1",
        },
        (
            "[webgpu-fault] injected device.lost@1",
            "injected native recovery failure",
            "recovery failed; terminating cleanly without an automatic OpenGL fallback",
        ),
        ("[SDL] GL ready:", "switched to OpenGL"),
        frames=20,
        expect_failure=True,
    )
    run_case(
        binary,
        rom,
        "presented-surface capture allocation -> local diagnostic failure",
        {
            "MDKR_WEBGPU_FAULT": "capture.surface-buffer",
            "GE007_WEBGPU_DUMP_SURFACE": "1",
        },
        (
            "[webgpu-fault] injected capture.surface-buffer@1",
            completion(20),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=20,
        hidden=False,
    )

    run_case(
        binary,
        rom,
        "second device loss -> bounded terminal stop",
        {"MDKR_WEBGPU_FAULT": "device.lost@all"},
        (
            "[webgpu-fault] injected device.lost@1",
            "native device recovery succeeded; gameplay continues",
            "[webgpu-fault] injected device.lost@2",
            "recovery failed; terminating cleanly without an automatic OpenGL fallback",
        ),
        ("[SDL] GL ready:", "switched to OpenGL"),
        frames=20,
        expect_failure=True,
    )

    run_case(
        binary,
        rom,
        "persistent surface timeout is bounded and terminal after one rebuild",
        {
            "MDKR_WEBGPU_FAULT": "surface.timeout@all",
            # This arm owns the 120-attempt recovery boundary, not the separate
            # nonblocking-admission policy. Admit each fault deterministically
            # so two complete streaks fit inside the 300-frame process bound.
            "MDKR_TEST_RENDER_FULL_ADMISSION": "1",
        },
        (
            "[webgpu-fault] injected surface.timeout@1",
            "surface recovery failed for 120 consecutive frames",
            "native device recovery succeeded; gameplay continues",
            "recovery failed; terminating cleanly without an automatic OpenGL fallback",
        ),
        ("[SDL] GL ready:", "switched to OpenGL"),
        frames=300,
        expect_failure=True,
    )

    for point in (
        "surface.suboptimal",
        "surface.timeout",
        "surface.outdated",
        "surface.lost",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> bounded in-place repair",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(20),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=20,
        )

    run_case(
        binary,
        rom,
        "alpha capability fallback",
        {"MDKR_WEBGPU_FAULT": "capabilities.alpha"},
        (
            "[webgpu-fault] injected capabilities.alpha@1",
            completion(20),
        ),
        ("attempting one native device reinitialization",),
        frames=20,
    )
    run_case(
        binary,
        rom,
        "present-mode capability fallback",
        {
            "MDKR_WEBGPU_FAULT": "capabilities.present",
            "GE007_WEBGPU_PRESENT": "mailbox",
        },
        (
            "[webgpu-fault] injected capabilities.present@1",
            "requested=mailbox effective=fifo",
            "reason=capabilities-unavailable",
            completion(20),
        ),
        ("attempting one native device reinitialization",),
        frames=20,
    )

    pure_fatal_points = (
        "frame.noise-buffer",
        "target.scene-texture",
        "target.scene-view",
        "target.depth-texture",
        "target.depth-view",
        "target.post-texture",
        "target.post-view",
        "target.resolve-texture",
        "target.resolve-view",
        "target.output-depth-texture",
        "target.output-depth-view",
        "shader.bind-group-layout",
        "shader.module",
        "shader.pipeline-layout",
        "shader.pipeline-sync",
        "texture.texture",
        "texture.view",
        "texture.sampler",
        "draw.white-texture",
        "draw.white-view",
        "draw.default-sampler",
        "draw.vertex-buffer",
        "draw.bind-group",
    )
    for point in pure_fatal_points:
        run_case(
            binary,
            rom,
            f"{point} -> device rebuild",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "native device recovery succeeded; gameplay continues",
                completion(120),
            ),
            ("switched to OpenGL",),
            frames=120,
        )

    remastered_fatal_points = (
        "resolve.module",
        "resolve.bind-group-layout",
        "resolve.uniform",
        "resolve.sampler",
        "resolve.pipeline-layout",
        "resolve.pipeline",
        "resolve.bind-group",
        "resolve.pass",
        "post.module",
        "post.bind-group-layout",
        "post.uniform",
        "post.sampler-nearest",
        "post.sampler-linear",
        "post.pipeline-layout",
        "post.pipeline",
        "post.bind-group",
        "post.pass",
        "overlay.output-pass",
    )
    for point in remastered_fatal_points:
        run_case(
            binary,
            rom,
            f"{point} -> device rebuild",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                "native device recovery succeeded; gameplay continues",
                completion(120),
            ),
            ("switched to OpenGL",),
            frames=120,
            mode="remastered",
        )

    for point in ("texture.mip-texture", "texture.mip-view"):
        run_case(
            binary,
            rom,
            f"{point} -> faithful single-level fallback",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(120),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=120,
            mode="remastered",
        )

    run_case(
        binary,
        rom,
        "internal frame-capture allocation -> local diagnostic failure",
        {
            "MDKR_WEBGPU_FAULT": "capture.frame-buffer",
            "GE007_WEBGPU_DUMP_FRAME": "1",
        },
        (
            "[webgpu-fault] injected capture.frame-buffer@1",
            completion(20),
        ),
        (
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=20,
    )
    for point in (
        "readback.buffer",
        "readback.encoder",
        "readback.finish",
        "readback.map",
    ):
        run_case(
            binary,
            rom,
            f"{point} -> local diagnostic failure",
            {"MDKR_WEBGPU_FAULT": point},
            (
                f"[webgpu-fault] injected {point}@1",
                completion(20),
            ),
            (
                "attempting one native device reinitialization",
                "switched to OpenGL",
            ),
            frames=20,
            dump_frames=True,
        )

    # ---------------------------------------------------------------
    # Custom-character (Character Workshop) skinned renderer.
    #
    # These points only exist while a player has installed and selected a
    # custom character, so no ROM-only route reaches them. Install a generated
    # fixture package into a private catalog and drive the direct Workshop
    # preview context, which draws the skinned character every frame and, once
    # replacement draws are flowing, requests the optional occlusion-evidence
    # pass. Each arm requires its own injection line, so a point that never
    # fires on this route fails here instead of being credited silently.
    with tempfile.TemporaryDirectory(prefix="mdkr-wgpu-skinned-") as skinned_dir:
        catalog = build_skinned_fixture(Path(skinned_dir))
        skinned_env = {
            "MDKR_TRACE": "1",
            "MDKR_RENDER_SCALE": "1",
            "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(catalog),
            "MDKR_CHARACTER_WORKSHOP_PREVIEW": "car",
            "MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS": "1",
            "MDKR_CUSTOM_CHARACTER_P1": SKINNED_PACKAGE_ID,
        }
        skinned_args = ("--window-size", "1280x960")
        skinned_frames = 180

        skinned_local_degrade_points = (
            "skinned.module",
            "skinned.bind-group-layout",
            "skinned.pipeline-layout",
            "skinned.pipeline",
            "skinned.vertex-buffer",
            "skinned.index-buffer",
            "skinned.texture",
            "skinned.view",
            "skinned.uniform",
            "skinned.sampler",
            "skinned.bind-group",
        )
        for point in skinned_local_degrade_points:
            output = run_case(
                binary,
                rom,
                f"{point} -> custom draw refused, retail scene continues",
                {**skinned_env, "MDKR_WEBGPU_FAULT": point},
                (
                    f"[webgpu-fault] injected {point}@1",
                    "character_workshop_result:",
                    completion(skinned_frames),
                ),
                (
                    "attempting one native device reinitialization",
                    "switched to OpenGL",
                ),
                frames=skinned_frames,
                mode="restored",
                extra_args=skinned_args,
            )
            refused = re.search(
                r"\[WGPU-MODERN-CHARACTER\].*?refusedDraws=(\d+)", output
            )
            if refused is None or int(refused.group(1)) < 1:
                raise CheckFailure(
                    f"{point}: no refused custom draw was recorded; the "
                    "injection did not reach the skinned draw path\n"
                    + output[-6000:]
                )

        skinned_visibility_points = (
            "skinned.visibility-query",
            "skinned.visibility-resolve",
            "skinned.visibility-readback",
            "skinned.visibility-texture",
            "skinned.visibility-view",
            "skinned.visibility-seed-pipeline",
            "skinned.visibility-equal-pipeline",
            "skinned.visibility-occluded-pipeline",
            "skinned.visibility-pass",
        )
        for point in skinned_visibility_points:
            output = run_case(
                binary,
                rom,
                f"{point} -> occlusion evidence unavailable, preview continues",
                {**skinned_env, "MDKR_WEBGPU_FAULT": f"{point}@all"},
                (
                    f"[webgpu-fault] injected {point}@1",
                    "character_workshop_result:",
                    completion(skinned_frames),
                ),
                (
                    "attempting one native device reinitialization",
                    "switched to OpenGL",
                ),
                frames=skinned_frames,
                mode="restored",
                extra_args=skinned_args,
            )
            visibility = re.search(
                r"character_workshop_result:.*?visibility=(-?\d+)/(-?\d+)",
                output,
            )
            if visibility is None or visibility.group(1) != "0":
                raise CheckFailure(
                    f"{point}: the Workshop still published visibility "
                    "evidence after the injection\n" + output[-6000:]
                )

    segmented = run_case(
        binary,
        rom,
        "forced-small vertex stream -> bounded segmentation",
        {"MDKR_TEST_WEBGPU_VERTEX_SEGMENT_BYTES": "4096"},
        (completion(120),),
        (
            "vertex-buffer segment allocation failed",
            "attempting one native device reinitialization",
            "switched to OpenGL",
        ),
        frames=120,
        mode="remastered",
    )
    segment_match = re.search(
        r"\[WGPU-LIMITS\].*vertexBytes=(\d+)\s+"
        r"vertexSegments=(\d+)\s+vertexSegmentCap=(\d+)",
        segmented,
    )
    if (
        segment_match is None
        or int(segment_match.group(1)) <= int(segment_match.group(3))
        or int(segment_match.group(2)) < 2
    ):
        raise CheckFailure(
            "forced-small vertex stream did not prove segmentation\n"
            + segmented[-6000:]
        )

    shader_guard = run_case(
        binary,
        rom,
        "forced shader-index exhaustion -> no-reuse guard and terminal stop",
        {"MDKR_TEST_WEBGPU_SHADER_LIMIT": "1"},
        (
            "shader hard limit reached (effective=1 hard=4096); "
            "refusing unsafe pointer reuse",
            "native device recovery succeeded; gameplay continues",
            "recovery failed; terminating cleanly without an automatic OpenGL fallback",
        ),
        ("[SDL] GL ready:", "switched to OpenGL"),
        frames=120,
        expect_failure=True,
    )
    guard_match = re.search(
        r"\[WGPU-LIMITS\]\s+shaders=(\d+)/4096\s+pipelines=(\d+).*"
        r"tableOverflow=(\d+)\s+pipelineFailures=(\d+)",
        shader_guard,
    )
    if (
        guard_match is None
        or int(guard_match.group(1)) != 1
        or int(guard_match.group(3)) < 2
        or int(guard_match.group(4)) != 0
    ):
        raise CheckFailure(
            "forced shader-index exhaustion did not prove bounded refusal "
            "without pipeline corruption\n"
            + shader_guard[-6000:]
        )

    case_count = (
        len(startup_fail_closed_points)
        + 2
        + 7
        + 4
        + 4
        + 2
        + len(pure_fatal_points)
        + len(remastered_fatal_points)
        + 2
        + 4
        + 4
        + len(skinned_local_degrade_points)
        + len(skinned_visibility_points)
    )
    print(
        "PASS: WebGPU recovery integration "
        f"({case_count} injected cases + forced vertex segmentation and "
        "shader-index exhaustion)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckFailure, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}")
        raise SystemExit(1)
