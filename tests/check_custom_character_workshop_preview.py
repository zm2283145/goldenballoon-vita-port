#!/usr/bin/env python3
"""Exact-game context gate for Character Workshop preview requests."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import deque
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_manifest_wizard as wizard  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    make_humanoid_glb,
    make_portrait_png,
    make_v4_manifest,
    rewrite_glb_document,
)


PACKAGE_ID = "org.mdkr.context-proof"
CONTACT_PACKAGE_ID = "org.mdkr.contact-proof"
TRANSPARENT_PACKAGE_ID = "org.mdkr.transparent-proof"
MASKED_PACKAGE_ID = "org.mdkr.masked-proof"
FRAMES = 180
PRODUCT_CAPTURE_FRAMES = 360


def read_png(path: Path, expected_colour_type: int) -> tuple[int, int, bytes]:
    payload = path.read_bytes()
    if len(payload) < 57 or payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("capture is not a complete PNG")
    offset = 8
    width = height = 0
    idat = bytearray()
    saw_ihdr = saw_iend = False
    while offset + 12 <= len(payload):
        size = struct.unpack_from(">I", payload, offset)[0]
        kind = payload[offset + 4:offset + 8]
        end = offset + 12 + size
        if end > len(payload):
            raise ValueError("capture has a truncated PNG chunk")
        data = payload[offset + 8:offset + 8 + size]
        expected_crc = struct.unpack_from(">I", payload, offset + 8 + size)[0]
        if zlib.crc32(kind + data) & 0xFFFFFFFF != expected_crc:
            raise ValueError("capture has a corrupt PNG chunk")
        if kind == b"IHDR":
            if saw_ihdr or size != 13 or offset != 8:
                raise ValueError("capture has an invalid PNG header")
            (width, height, bit_depth, colour_type, compression,
             filtering, interlace) = struct.unpack(">IIBBBBB", data)
            if (width == 0 or height == 0 or bit_depth != 8 or
                    colour_type != expected_colour_type or compression != 0 or
                    interlace != 0):
                raise ValueError(
                    "capture has the wrong canonical PNG colour contract"
                )
            saw_ihdr = True
        elif kind == b"IDAT":
            if not saw_ihdr or saw_iend:
                raise ValueError("capture has an out-of-order PNG data chunk")
            idat.extend(data)
        elif kind == b"IEND":
            if size != 0 or not saw_ihdr or not idat:
                raise ValueError("capture has an invalid PNG terminator")
            saw_iend = True
            offset = end
            break
        offset = end
    if not saw_iend or offset != len(payload):
        raise ValueError("capture is not a complete PNG")
    try:
        filtered = zlib.decompress(bytes(idat))
    except zlib.error as error:
        raise ValueError("capture has invalid compressed pixels") from error
    channels = 3 if expected_colour_type == 2 else 4
    stride = width * channels
    if len(filtered) != height * (stride + 1):
        raise ValueError("capture has the wrong decompressed pixel size")
    pixels = bytearray(width * height * channels)
    previous = bytearray(stride)
    source = 0
    for y in range(height):
        filter_kind = filtered[source]
        source += 1
        row = bytearray(filtered[source:source + stride])
        source += stride
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = (
                previous[index - channels] if index >= channels else 0
            )
            if filter_kind == 1:
                predictor = left
            elif filter_kind == 2:
                predictor = above
            elif filter_kind == 3:
                predictor = (left + above) // 2
            elif filter_kind == 4:
                candidate = left + above - upper_left
                left_error = abs(candidate - left)
                above_error = abs(candidate - above)
                diagonal_error = abs(candidate - upper_left)
                predictor = (left if left_error <= above_error and
                             left_error <= diagonal_error else
                             above if above_error <= diagonal_error else
                             upper_left)
            elif filter_kind == 0:
                predictor = 0
            else:
                raise ValueError("capture uses an invalid PNG row filter")
            row[index] = (row[index] + predictor) & 0xFF
        pixels[y * stride:(y + 1) * stride] = row
        previous = row
    return width, height, bytes(pixels)


def read_png_rgb(path: Path) -> tuple[int, int, bytes]:
    return read_png(path, 2)


def read_png_rgba(path: Path) -> tuple[int, int, bytes]:
    return read_png(path, 6)


def require_fixture_composition(
        width: int, height: int, pixels: bytes, *, require_central: bool = True
) -> None:
    """Find the generated fixture's contiguous brown-red material on screen.

    The threshold intentionally allows normal GPU/lighting variation. The
    fixture is the only large contiguous surface in this hue range; checking a
    component rather than a global pixel count prevents course scenery or a
    donor's few red texels from passing a missing/off-screen character.
    """
    mask = bytearray(width * height)
    for pixel in range(width * height):
        red, green, blue = pixels[pixel * 3:pixel * 3 + 3]
        if (120 <= red <= 220 and 20 <= green <= 75 and blue <= 35 and
                red >= green * 2.5):
            mask[pixel] = 1
    visited = bytearray(width * height)
    largest_area = 0
    largest_bounds = (0, 0, 0, 0)
    for seed, present in enumerate(mask):
        if not present or visited[seed]:
            continue
        queue = deque([seed])
        visited[seed] = 1
        area = 0
        min_x = max_x = seed % width
        min_y = max_y = seed // width
        while queue:
            current = queue.popleft()
            y, x = divmod(current, width)
            area += 1
            min_x, max_x = min(min_x, x), max(max_x, x)
            min_y, max_y = min(min_y, y), max(max_y, y)
            for neighbour in (current - 1, current + 1,
                              current - width, current + width):
                if (neighbour < 0 or neighbour >= len(mask) or
                        visited[neighbour] or not mask[neighbour]):
                    continue
                neighbour_y, neighbour_x = divmod(neighbour, width)
                if abs(neighbour_x - x) + abs(neighbour_y - y) != 1:
                    continue
                visited[neighbour] = 1
                queue.append(neighbour)
        if area > largest_area:
            largest_area = area
            largest_bounds = (min_x, min_y, max_x, max_y)
    min_x, min_y, max_x, max_y = largest_bounds
    component_width = max_x - min_x + 1
    component_height = max_y - min_y + 1
    component_box_area = component_width * component_height
    centre_x = (min_x + max_x) / 2.0
    centre_y = (min_y + max_y) / 2.0
    # Judge material occupancy relative to its own bounded component. A top
    # view legitimately presents less coloured surface than a front view, and
    # tying this threshold to the full 4K render target rejected a clearly
    # visible, centered subject. Independent minimum dimensions below still
    # prevent a tiny speck from satisfying the gate.
    if (largest_area < max(512, component_box_area // 5) or
            max_x - min_x < width * 3 // 100 or
            max_y - min_y < height * 3 // 100 or
            (require_central and (
                min_x < width * 15 // 100 or max_x > width * 85 // 100 or
                min_y < height * 15 // 100 or max_y > height * 80 // 100 or
                abs(centre_x - width / 2.0) > width * 15 // 100 or
                abs(centre_y - height / 2.0) > height * 15 // 100))):
        raise ValueError(
            "generated character is absent, clipped, or outside the central "
            f"inspection frame (area={largest_area}, bounds={largest_bounds})"
        )


def require_model_alpha_composition(
        width: int, height: int, pixels: bytes) -> None:
    total = width * height
    alpha = pixels[3::4]
    transparent = sum(value == 0 for value in alpha)
    visible = [index for index, value in enumerate(alpha) if value != 0]
    if transparent < total // 2 or len(visible) < total // 500 or \
            len(visible) > total * 2 // 3:
        raise ValueError(
            "model-only capture lacks a bounded subject and genuinely "
            f"transparent background (transparent={transparent}, "
            f"visible={len(visible)}, total={total})"
        )
    if any(
        pixels[index * 4:index * 4 + 3] != b"\x00\x00\x00"
        for index, value in enumerate(alpha) if value == 0
    ):
        raise ValueError("transparent pixels retain hidden matte colour")
    xs = [index % width for index in visible]
    ys = [index // width for index in visible]
    bounds = (min(xs), min(ys), max(xs), max(ys))
    centre_x = (bounds[0] + bounds[2]) / 2.0
    centre_y = (bounds[1] + bounds[3]) / 2.0
    subject_height = bounds[3] - bounds[1] + 1
    if (bounds[0] < width * 5 // 100 or
            bounds[2] > width * 95 // 100 or
            bounds[1] < height * 5 // 100 or
            bounds[3] > height * 95 // 100 or
            subject_height < height * 60 // 100 or
            subject_height > height * 85 // 100 or
            abs(centre_x - width / 2.0) > width * 20 // 100 or
            abs(centre_y - height / 2.0) > height * 20 // 100):
        raise ValueError(
            "model-only subject is clipped, outside the inspection frame, or "
            "outside 60-85% portrait occupancy "
            f"(bounds={bounds}, height={subject_height}/{height})"
        )
    rgb = bytearray(total * 3)
    for index in range(total):
        rgb[index * 3:index * 3 + 3] = pixels[index * 4:index * 4 + 3]
    # Alpha bounds above describe the complete isolated subject. A particular
    # lit material face can legitimately sit off-centre on a rotated 3D model
    # (especially in exact top/underside views), so use it only as a material-
    # presence witness here. Scene captures still require the coloured fixture
    # itself to occupy the expected central gameplay region.
    require_fixture_composition(
        width, height, bytes(rgb), require_central=False)


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
    # Preview composition must exercise a genuinely three-dimensional subject;
    # the probe suite's minimal single triangle is correctly edge-on from the
    # exact top camera and therefore cannot prove a top-view renderer contract.
    model.write_bytes(make_animated_glb(volumetric=True))
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
    contact_model = source / "contact-model.glb"
    contact_manifest_path = source / "contact-manifest.json"
    contact_package = source / "contact-proof.mdkrchar"
    transparent_model = source / "transparent-model.glb"
    transparent_manifest_path = source / "transparent-manifest.json"
    transparent_package = source / "transparent-proof.mdkrchar"
    masked_model = source / "masked-model.glb"
    masked_manifest_path = source / "masked-manifest.json"
    masked_package = source / "masked-proof.mdkrchar"
    portrait_bytes = portrait.read_bytes()
    contact_model.write_bytes(make_humanoid_glb())
    contact_manifest = make_v4_manifest(portrait_bytes, humanoid=True)
    contact_manifest["id"] = CONTACT_PACKAGE_ID
    contact_manifest["display_name"] = "Contact Proof"
    contact_manifest["license"] = {
        "spdx": "CC0-1.0",
        "attribution": "Generated MDKR contact fixture",
        "source_url": "https://example.invalid/contact-proof",
    }
    contact_manifest["gameplay"] = {
        "donor": "bumper", "vehicles": ["car", "hovercraft", "plane"],
    }
    contact_manifest_path.write_text(
        json.dumps(contact_manifest, indent=2) + "\n", encoding="utf-8")

    def transparent_material(document: dict[str, object]) -> None:
        material = document["materials"][0]  # type: ignore[index]
        material["alphaMode"] = "BLEND"  # type: ignore[index]
        pbr = material["pbrMetallicRoughness"]  # type: ignore[index]
        pbr["baseColorFactor"][3] = 0.5  # type: ignore[index]

    transparent_model.write_bytes(rewrite_glb_document(
        make_animated_glb(volumetric=True), transparent_material))
    transparent_manifest, _ = wizard.build_manifest(
        transparent_model, TRANSPARENT_PACKAGE_ID, "Transparent Proof",
        "CC0-1.0", "Generated MDKR transparent fixture",
        "https://example.invalid/transparent-proof", "bumper",
        ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[90, 210, 140],
    )
    transparent_manifest_path.write_text(
        json.dumps(transparent_manifest, indent=2) + "\n", encoding="utf-8")

    def mixed_masked_material(document: dict[str, object]) -> None:
        materials = document["materials"]  # type: ignore[index]
        masked = json.loads(json.dumps(materials[0]))
        masked["name"] = "alpha-tested-detail"
        masked["alphaMode"] = "MASK"
        masked["alphaCutoff"] = 0.5
        materials.append(masked)
        primitives = document["meshes"][0]["primitives"]  # type: ignore[index]
        detail = json.loads(json.dumps(primitives[0]))
        detail["material"] = 1
        primitives.append(detail)

    masked_model.write_bytes(rewrite_glb_document(
        make_animated_glb(volumetric=True), mixed_masked_material))
    masked_manifest, _ = wizard.build_manifest(
        masked_model, MASKED_PACKAGE_ID, "Masked Multi-primitive Proof",
        "CC0-1.0", "Generated MDKR alpha-test fixture",
        "https://example.invalid/masked-proof", "bumper",
        ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[90, 210, 140],
    )
    masked_manifest_path.write_text(
        json.dumps(masked_manifest, indent=2) + "\n", encoding="utf-8")

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
    packed_contact = run([
        sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
        "pack", "--model", str(contact_model),
        "--manifest", str(contact_manifest_path),
        "--license", str(license_path), "--portrait", str(portrait),
        "--output", str(contact_package),
    ])
    output += packed_contact.stdout or ""
    if packed_contact.returncode != 0:
        failures.append("contact witness package generation failed")
    packed_transparent = run([
        sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
        "pack", "--model", str(transparent_model),
        "--manifest", str(transparent_manifest_path),
        "--license", str(license_path), "--portrait", str(portrait),
        "--output", str(transparent_package),
    ])
    output += packed_transparent.stdout or ""
    if packed_transparent.returncode != 0:
        failures.append("transparent witness package generation failed")
    packed_masked = run([
        sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
        "pack", "--model", str(masked_model),
        "--manifest", str(masked_manifest_path),
        "--license", str(license_path), "--portrait", str(portrait),
        "--output", str(masked_package),
    ])
    output += packed_masked.stdout or ""
    if packed_masked.returncode != 0:
        failures.append("masked multi-primitive package generation failed")
    if not failures:
        installed = run([
            sys.executable,
            str(ROOT / "tests" / "run_character_manager_fixture.py"),
            "--directory", str(characters), "install", str(package),
        ])
        output += installed.stdout or ""
        if installed.returncode != 0:
            failures.append("temporary catalog install failed")
        installed_contact = run([
            sys.executable,
            str(ROOT / "tests" / "run_character_manager_fixture.py"),
            "--directory", str(characters), "install", str(contact_package),
        ])
        output += installed_contact.stdout or ""
        if installed_contact.returncode != 0:
            failures.append("temporary contact catalog install failed")
        for fixture_label, fixture_package in (
            ("transparent", transparent_package),
            ("masked multi-primitive", masked_package),
        ):
            installed_fixture = run([
                sys.executable,
                str(ROOT / "tests" / "run_character_manager_fixture.py"),
                "--directory", str(characters), "install",
                str(fixture_package),
            ])
            output += installed_fixture.stdout or ""
            if installed_fixture.returncode != 0:
                failures.append(
                    f"temporary {fixture_label} catalog install failed")

    arms = [
        ("select", 1, True, None, None, False, None, None, None, None),
        ("car", 1, False, None, None, False, None, None, None, None),
        ("hovercraft", 1, False, None, None, False, None, None, None, None),
        ("plane", 1, False, None, None, False, None, None, None, None),
        ("car", 3, False, None, None, False, None, None, None, None),
        ("car", 4, True, None, None, False, None, None, None, None),
        ("car", 1, False, "select.idle", "250", False,
         None, None, None, None),
        ("car", 1, False, "race.finish_win", "750", True,
         None, None, None, None),
        ("car", 1, False, "select.idle", "500", False,
         180, 15, "bright", "scene"),
        ("car", 1, False, "select.idle", "500", False,
         180, 15, "bright", "model-alpha"),
        ("car", 4, False, "select.idle", "500", False,
         180, 15, "bright", "model-alpha"),
        ("car", 1, False, "select.idle", "500", False,
         0, 90, "bright", "model-alpha"),
        ("car", 1, False, "select.idle", "500", False,
         0, -90, "bright", "model-alpha"),
    ]
    arms = [arm + (PACKAGE_ID,) for arm in arms]
    arms.append((
        "car", 1, False, None, None, False,
        None, None, None, None, CONTACT_PACKAGE_ID,
    ))
    arms.append((
        "car", 1, False, None, None, False,
        None, None, None, None, TRANSPARENT_PACKAGE_ID,
    ))
    arms.append((
        "car", 1, False, None, None, False,
        None, None, None, None, MASKED_PACKAGE_ID,
    ))
    arm_draws: dict[str, int] = {}
    subject_capture_draws: dict[str, int] = {}
    captures: dict[str, Path] = {}
    reported_dimensions: dict[str, tuple[int, int, int, int]] = {}
    comparison_environment: tuple[str, str, str, str, str] | None = None
    if not failures:
        for (context, players, capture, pose, pose_phase,
             expect_fallback, view_yaw, view_pitch, lighting,
             capture_kind, package_id) in arms:
            label = (f"{context}-{players}p" if pose is None else
                     f"{context}-{players}p-pose" +
                     ("-fallback" if expect_fallback else "") +
                     (f"-{capture_kind}-capture" if capture_kind else ""))
            if view_pitch == 90:
                label += "-top"
            elif view_pitch == -90:
                label += "-underside"
            if package_id == CONTACT_PACKAGE_ID:
                label += "-contact-witness"
            elif package_id == TRANSPARENT_PACKAGE_ID:
                label += "-transparent-witness"
            elif package_id == MASKED_PACKAGE_ID:
                label += "-masked-multiprimitive-witness"
            force_gpu_timing_disabled = (
                context == "select" and players == 1 and pose is None
            )
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
                env[f"MDKR_CUSTOM_CHARACTER_P{player + 1}"] = package_id
            if force_gpu_timing_disabled:
                env["MDKR_WEBGPU_GPU_TIMING"] = "0"
            if pose is not None and pose_phase is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE"] = pose
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE"] = pose_phase
            product_capture_path = arm_dir / "stabilized.png"
            if view_yaw is not None:
                env["MDKR_CHARACTER_WORKSHOP_VIEW_YAW_DEGREES"] = str(view_yaw)
                env["MDKR_CHARACTER_WORKSHOP_VIEW_PITCH_DEGREES"] = str(view_pitch)
                env["MDKR_CHARACTER_WORKSHOP_LIGHTING"] = str(lighting)
            if capture_kind is not None:
                env["MDKR_CHARACTER_WORKSHOP_CAPTURE_PNG"] = str(
                    product_capture_path
                )
                env["MDKR_CHARACTER_WORKSHOP_CAPTURE_KIND"] = capture_kind
            auto_return = (
                context == "car" and players == 1
                and pose == "select.idle" and pose_phase == "500"
                and view_yaw == 180 and view_pitch == 15
                and lighting == "bright" and capture_kind == "scene"
            )
            if auto_return:
                env["MDKR_CHARACTER_WORKSHOP_CAPTURE_AUTO_RETURN"] = "1"
            run_frames = (
                PRODUCT_CAPTURE_FRAMES if capture_kind is not None else FRAMES
            )
            command = [
                str(binary), "--headless-frames", str(run_frames), "--rom",
                str(rom), "--window-size", "1280x960", "--restored",
            ]
            if capture:
                env.update(MDKR_DUMP_FROM=str(run_frames - 2),
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
                expected_phase = int(pose_phase)
                pose_match = re.search(
                    rf"pose={expected_pose} phase={expected_phase} "
                    r"transitionFrom=0 transitionPhase=0 "
                    r"transition=0/0/0 transitionBlend=0,0 "
                    r"transitionSource=([1-3]),0 "
                    r"poseTicks=(\d+) poseFallback=(\d+)",
                    arm_output,
                )
                if pose_match is None or int(pose_match.group(2)) <= 0:
                    failures.append(
                        f"{label} did not drive the held semantic phase"
                    )
                elif expect_fallback:
                    if int(pose_match.group(3)) != int(pose_match.group(2)):
                        failures.append(
                            f"{label} did not report complete source fallback"
                        )
                elif int(pose_match.group(3)) != 0:
                    failures.append(
                        f"{label} unexpectedly used source fallback"
                    )
            if capture_kind is not None:
                kind_value = 1 if capture_kind == "model-alpha" else 0
                visual_match = re.search(
                    rf"view={view_yaw},{view_pitch} lighting=1 "
                    r"cameraTicks=(\d+) "
                    r"lightingDraws=(\d+) capture=1/[01]/[01] "
                    rf"kind={kind_value} "
                    r"captureStableFrames=(\d+) bytes=\d+",
                    arm_output,
                )
                if (
                    visual_match is None
                    or int(visual_match.group(1)) <= 0
                    or int(visual_match.group(2)) <= 0
                ):
                    failures.append(
                        f"{label} did not prove camera and character-light application"
                    )
                if auto_return and (
                    "[WORKSHOP-CAPTURE] auto-return after presented frame="
                    not in arm_output
                    or "[SDL] headless: reached" in arm_output
                ):
                    failures.append(
                        f"{label} did not return immediately after its "
                        "presented stabilized capture"
                    )
                armed_match = re.search(
                    rf"character_workshop_capture: armed kind={capture_kind} "
                    r"stableFrames=(\d+) "
                    r"countdown=\d+ ", arm_output,
                )
                result_capture_match = re.search(
                    r"character_workshop_result: .*capture=1/1/0 "
                    rf"kind={kind_value} "
                    r"captureStableFrames=(\d+) bytes=0 ",
                    arm_output,
                )
                if (
                    armed_match is None
                    or int(armed_match.group(1)) < 12
                    or result_capture_match is None
                    or int(result_capture_match.group(1)) < 12
                ):
                    failures.append(
                        f"{label} did not wait for and save a stable visible frame"
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
                minimum_samples = 12 if auto_return else 40
                if (
                    warmup != 1 or realtime != 0
                    or samples < minimum_samples
                ):
                    failures.append(
                        f"{label} result did not isolate its synthetic "
                        "post-warmup capture/performance sample"
                    )
                if replacements <= 0:
                    failures.append(f"{label} result counted no package replacements")
            gpu_match = re.search(
                r"gpu=(\d+)/([0-9a-f]+) "
                r"sceneGpuNs=(\d+),(\d+),(\d+) "
                r"characterGpuNs=(\d+),(\d+),(\d+) "
                r"gpuExcluded=(\d+),(\d+),(\d+)",
                arm_output,
            )
            if gpu_match is None:
                failures.append(
                    f"{label} emitted no explicit GPU timestamp contract"
                )
            else:
                values = [int(value, 16) if index == 1 else int(value)
                          for index, value in enumerate(gpu_match.groups())]
                (gpu_status, gpu_scopes, scene_samples, scene_p50,
                 scene_p95, character_samples, character_p50,
                 character_p95, gpu_pending, gpu_ring_full,
                 gpu_invalid) = values
                if force_gpu_timing_disabled:
                    if gpu_status != 0 or gpu_scopes != 0 or any(values[2:]):
                        failures.append(
                            f"{label} did not preserve force-disabled GPU "
                            "timing as explicit unsupported evidence"
                        )
                elif gpu_scopes == 0:
                    if gpu_status != 0 or any(values[2:]):
                        failures.append(
                            f"{label} reported values for unsupported GPU timing"
                        )
                else:
                    gpu_noninvalid = (scene_samples + gpu_pending +
                                      gpu_ring_full)
                    scene_invalid_gap = samples - gpu_noninvalid
                    minimum_usable = max(1, (samples * 2) // 3)
                    quality_available = (
                        gpu_status == 3 and
                        scene_samples >= minimum_usable
                    )
                    quality_rejected = (
                        gpu_status == 5 and
                        scene_samples < minimum_usable and
                        gpu_invalid > 0
                    )
                    # A rejected window may also contain wall-cadence frames
                    # where presentation intentionally submitted no scene
                    # pass. Exact exclusion equality is meaningful only for
                    # an actionable available window; the error contract must
                    # still keep every reported counter bounded and honest.
                    scene_distribution_valid = (
                        scene_p50 > 0 and scene_p95 >= scene_p50
                    ) if scene_samples > 0 else (
                        quality_rejected and
                        scene_p50 == 0 and scene_p95 == 0
                    )
                    if (not (quality_available or quality_rejected) or
                            not (gpu_scopes & 0x1) or
                            not scene_distribution_valid or
                            gpu_pending > 6 or gpu_ring_full > samples or
                            gpu_invalid > samples or
                            gpu_noninvalid > samples or
                            (quality_available and
                             scene_invalid_gap > gpu_invalid) or
                            (quality_available and
                             not (gpu_scopes & 0x2) and
                             scene_invalid_gap != gpu_invalid)):
                        failures.append(
                            f"{label} returned inconsistent scene-pass GPU "
                            f"timestamps status={gpu_status} scopes={gpu_scopes:x} "
                            f"scene={scene_samples, scene_p50, scene_p95} "
                            f"excluded={gpu_pending, gpu_ring_full, gpu_invalid} "
                            f"wallSamples={samples}"
                        )
                    if gpu_scopes & 0x2:
                        if quality_available and (
                                character_samples <= 0 or
                                character_p50 <= 0 or
                                character_p95 < character_p50):
                            failures.append(
                                f"{label} exposed in-pass capability without "
                                "valid character-draw timestamps"
                            )
                        elif (quality_rejected and character_samples > 0 and
                              (character_p50 <= 0 or
                               character_p95 < character_p50)):
                            failures.append(
                                f"{label} returned malformed partial "
                                "character-draw timestamps"
                            )
                    elif any((character_samples, character_p50,
                              character_p95)):
                        failures.append(
                            f"{label} fabricated character-only GPU timing"
                        )
            contact_match = re.search(
                r"contacts=(\d+) contactMaxUm=\d+ "
                r"contactWitness=([0-9a-f]+) "
                r"contactWitnessErrorUm=(\d+),(\d+),(\d+),(\d+) "
                r"contactLHUm=root:(-?\d+),(-?\d+),(-?\d+) "
                r"bend:(-?\d+),(-?\d+),(-?\d+) "
                r"target:(-?\d+),(-?\d+),(-?\d+) "
                r"end:(-?\d+),(-?\d+),(-?\d+)",
                arm_output,
            )
            if contact_match is None:
                failures.append(f"{label} emitted no bounded contact witness contract")
            elif pose is None:
                values = [int(value, 16) if index == 1 else int(value)
                          for index, value in enumerate(contact_match.groups())]
                solves, mask = values[0:2]
                errors = values[2:6]
                root, bend = values[6:9], values[9:12]
                target, endpoint = values[12:15], values[15:18]
                if context == "select":
                    if solves != 0 or mask != 0 or any(
                            errors + root + bend + target + endpoint):
                        failures.append(
                            f"{label} fabricated vehicle contacts in character select"
                        )
                elif solves == 0:
                    if mask != 0 or any(
                            errors + root + bend + target + endpoint):
                        failures.append(
                            f"{label} detached witnesses from its zero solve count"
                        )
                else:
                    measured = math.isqrt(sum(
                        (endpoint[axis] - target[axis]) ** 2
                        for axis in range(3)
                    ))
                    if (
                        mask != 0xF
                        or any(value < 0 or value > 1_000_000_000
                               for value in errors)
                        or abs(measured - errors[0]) > 3
                        or root == bend
                    ):
                        failures.append(
                            f"{label} returned inconsistent exact hand/foot witnesses "
                            f"mask={mask:x} errors={errors} measuredLH={measured} "
                            f"root={root} bend={bend} target={target} end={endpoint}"
                        )
                if package_id == CONTACT_PACKAGE_ID and solves == 0:
                    failures.append(
                        f"{label} did not execute automatic humanoid contacts"
                    )
            fit_match = re.search(
                r"character_workshop_result: .* fit=(\d+) "
                r"fitAnchorUm=(-?\d+),(-?\d+),(-?\d+) "
                r"fitBoundsYUm=(-?\d+),(-?\d+) "
                r"fitForwardMilli=(-?\d+),(-?\d+),(-?\d+)",
                arm_output,
            )
            if fit_match is None:
                failures.append(
                    f"{label} emitted no target-space fit measurement"
                )
            else:
                (fit_valid, anchor_x, anchor_y, anchor_z, bounds_min_y,
                 bounds_max_y, forward_x, forward_y,
                 forward_z) = map(int, fit_match.groups())
                forward_length_squared = (
                    forward_x * forward_x + forward_y * forward_y +
                    forward_z * forward_z
                )
                if fit_valid != 1 or bounds_min_y > bounds_max_y:
                    failures.append(
                        f"{label} returned an invalid calibrated fit volume"
                    )
                if not 995000 <= forward_length_squared <= 1005000:
                    failures.append(
                        f"{label} returned a non-unit facing direction"
                    )
                if forward_z <= 0:
                    failures.append(
                        f"{label} fixture faces backward in its target frame"
                    )
                if abs(anchor_x) > 5000 or abs(anchor_y) > 5000 or \
                        abs(anchor_z) > 5000:
                    failures.append(
                        f"{label} automatic anchor drifted from target zero"
                    )
                if context == "select" and bounds_min_y < -5000:
                    failures.append(
                        f"{label} calibrated volume penetrates the roster floor"
                    )
            camera_match = re.search(
                r"fitLandmarks=([0-9a-f]+) "
                r"headUm=(-?\d+),(-?\d+),(-?\d+) cameraFit=(\d+) "
                r"cameraBoundsMilli=(-?\d+),(-?\d+),(-?\d+),(-?\d+)/([0-9a-f]+) "
                r"cameraViewport=(-?\d+),(-?\d+),(\d+),(\d+) "
                r"cameraHeadMilli=(-?\d+),(-?\d+),(-?\d+)/([0-9a-f]+)",
                arm_output,
            )
            if camera_match is None:
                failures.append(
                    f"{label} emitted no exact gameplay-camera/anatomy evidence"
                )
            else:
                values = [int(value, 16) if index in (0, 9, 17) else int(value)
                          for index, value in enumerate(camera_match.groups())]
                (landmark_mask, head_x, head_y, head_z, camera_valid,
                 bounds_left, bounds_top, bounds_right, bounds_bottom,
                 camera_bounds_flags,
                 viewport_x, viewport_y, viewport_width, viewport_height,
                 camera_head_x, camera_head_y, camera_head_depth,
                 camera_head_flags) = values
                if (
                    landmark_mask & 0x4 == 0
                    or any(abs(value) > 1_000_000_000
                           for value in (head_x, head_y, head_z))
                    or camera_valid != 1
                    or bounds_left >= bounds_right
                    or bounds_top >= bounds_bottom
                    or camera_bounds_flags & ~0x7F
                    or viewport_x < 0
                    or viewport_y < 0
                    or viewport_width <= 0
                    or viewport_height <= 0
                    or camera_head_flags & ~0x7F
                ):
                    failures.append(
                        f"{label} returned inconsistent gameplay-camera evidence "
                        f"landmarks={landmark_mask:x} camera={camera_valid} "
                        f"bounds={(bounds_left, bounds_top, bounds_right, bounds_bottom, camera_bounds_flags)} "
                        f"viewport={(viewport_x, viewport_y, viewport_width, viewport_height)} "
                        f"head={(camera_head_x, camera_head_y, camera_head_depth, camera_head_flags)}"
                    )
            shell_tested = shell_submitted = 0
            surface_match = re.search(
                r"surface=(\d+) shell=(\d+)/(\d+) "
                r"subject=(\d+)/(\d+) crossings=(\d+)/(\d+) "
                r"crossingUm=(-?\d+),(-?\d+),(-?\d+)",
                arm_output,
            )
            if surface_match is None:
                failures.append(
                    f"{label} emitted no exact vehicle-body surface witness"
                )
            else:
                (surface_valid, shell_tested, shell_submitted,
                 subject_tested, subject_submitted, crossing_triangles,
                 crossing_pairs, crossing_x, crossing_y,
                 crossing_z) = map(int, surface_match.groups())
                if context == "select":
                    if any((surface_valid, shell_tested, shell_submitted,
                            subject_tested, subject_submitted,
                            crossing_triangles, crossing_pairs, crossing_x,
                            crossing_y, crossing_z)):
                        failures.append(
                            f"{label} fabricated a vehicle shell in character select"
                        )
                elif (
                    surface_valid != 1
                    or shell_tested <= 0
                    or shell_tested > shell_submitted
                    or subject_tested <= 0
                    or subject_tested > subject_submitted
                    or crossing_triangles > subject_tested
                    or crossing_pairs < crossing_triangles
                    or ((crossing_triangles == 0) != (crossing_pairs == 0))
                    or any(abs(value) > 1_000_000_000 for value in (
                        crossing_x, crossing_y, crossing_z
                    ))
                    or (crossing_pairs == 0 and any((
                        crossing_x, crossing_y, crossing_z
                    )))
                ):
                    failures.append(
                        f"{label} returned inconsistent vehicle-body surface "
                        f"evidence valid={surface_valid} "
                        f"shell={shell_tested, shell_submitted} "
                        f"subject={subject_tested, subject_submitted} "
                        f"crossings={crossing_triangles, crossing_pairs} "
                        f"first={crossing_x, crossing_y, crossing_z}"
                    )
            volume_match = re.search(
                r"volume=(\d+) topology=(\d+),(\d+),(\d+),(\d+) "
                r"containment=(\d+),(\d+),(\d+),(\d+) "
                r"containmentDepthUm=(\d+) "
                r"containmentPointUm=(-?\d+),(-?\d+),(-?\d+)",
                arm_output,
            )
            if volume_match is None:
                failures.append(
                    f"{label} emitted no closed-volume qualification witness"
                )
            else:
                (volume_qualified, open_edges, nonmanifold_edges,
                 winding_edges, self_pairs, containment_samples,
                 inside_samples, boundary_samples, outside_samples,
                 depth_um, deepest_x, deepest_y,
                 deepest_z) = map(int, volume_match.groups())
                topology_defects = (
                    open_edges + nonmanifold_edges + winding_edges + self_pairs
                )
                if context == "select":
                    if any(map(int, volume_match.groups())):
                        failures.append(
                            f"{label} fabricated a vehicle volume in character select"
                        )
                elif (
                    volume_qualified not in (0, 1)
                    or containment_samples > 2048
                    or inside_samples + boundary_samples + outside_samples
                    != containment_samples
                    or depth_um > 1_000_000_000
                    or any(abs(value) > 1_000_000_000 for value in (
                        deepest_x, deepest_y, deepest_z
                    ))
                ):
                    failures.append(
                        f"{label} returned structurally invalid volume evidence"
                    )
                elif volume_qualified:
                    if (
                        topology_defects != 0
                        or shell_tested != shell_submitted
                        or containment_samples == 0
                        or ((inside_samples == 0) != (depth_um == 0))
                        or (inside_samples == 0 and any((
                            deepest_x, deepest_y, deepest_z
                        )))
                    ):
                        failures.append(
                            f"{label} made an unqualified containment claim"
                        )
                elif (
                    containment_samples != 0
                    or any((inside_samples, boundary_samples, outside_samples,
                            depth_um, deepest_x, deepest_y, deepest_z))
                    or (topology_defects == 0
                        and shell_tested == shell_submitted)
                ):
                    failures.append(
                        f"{label} retained samples for an unqualified volume"
                    )
            visibility_match = re.search(
                r"visibility=(\d+)/(\d+) visibilitySize=(\d+)x(\d+) "
                r"visibilityDraws=(\d+),(\d+),(\d+),(\d+) "
                r"visibilityGrid=(\d+)x(\d+) "
                r"visibilityTiles=(\d+)/(\d+) "
                r"visibilityMask=([0-9a-f]{16})/([0-9a-f]{16})",
                arm_output,
            )
            if visibility_match is None:
                failures.append(
                    f"{label} emitted no exact opaque-depth visibility witness"
                )
            else:
                values = [
                    int(value, 16) if index >= 12 else int(value)
                    for index, value in enumerate(visibility_match.groups())
                ]
                (visibility_valid, visibility_qualified,
                 visibility_width, visibility_height,
                 visibility_draws, opaque_draws, masked_draws,
                 transparent_draws, grid_columns, grid_rows,
                 scene_tiles, isolated_tiles,
                 scene_mask, isolated_mask) = values
                common_visibility_valid = (
                    visibility_valid == 1
                    and visibility_width > 0
                    and visibility_height > 0
                    and visibility_draws > 0
                    and opaque_draws + masked_draws + transparent_draws
                    == visibility_draws
                    and (grid_columns, grid_rows) == (8, 8)
                    and scene_tiles <= isolated_tiles <= 64
                    and scene_mask.bit_count() == scene_tiles
                    and isolated_mask.bit_count() == isolated_tiles
                    and scene_mask & ~isolated_mask == 0
                )
                qualified_visibility_valid = (
                    visibility_qualified == 1
                    and transparent_draws == 0
                )
                transparent_visibility_valid = (
                    visibility_qualified == 0
                    and transparent_draws > 0
                    and isolated_tiles == 0
                    and scene_tiles == 0
                    and isolated_mask == 0
                    and scene_mask == 0
                )
                if not common_visibility_valid or not (
                        qualified_visibility_valid or
                        transparent_visibility_valid):
                    failures.append(
                        f"{label} returned inconsistent opaque-depth "
                        f"visibility evidence valid={visibility_valid}/"
                        f"{visibility_qualified} size="
                        f"{visibility_width}x{visibility_height} draws="
                        f"{visibility_draws, opaque_draws, masked_draws, transparent_draws} "
                        f"grid={grid_columns}x{grid_rows} tiles="
                        f"{scene_tiles, isolated_tiles} masks="
                        f"{scene_mask:016x}/{isolated_mask:016x}"
                    )
                if package_id == TRANSPARENT_PACKAGE_ID and not (
                        transparent_visibility_valid and
                        transparent_draws == visibility_draws):
                    failures.append(
                        f"{label} did not preserve blended transparency as "
                        "explicitly unqualified visibility evidence"
                    )
                if package_id == MASKED_PACKAGE_ID and not (
                        qualified_visibility_valid and opaque_draws > 0 and
                        masked_draws > 0 and transparent_draws == 0 and
                        visibility_draws >= 2):
                    failures.append(
                        f"{label} did not replay the complete mixed opaque/"
                        "alpha-tested primitive set"
                    )
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
            if capture_kind is not None:
                try:
                    if capture_kind == "model-alpha":
                        png_width, png_height, png_pixels = read_png_rgba(
                            product_capture_path
                        )
                        require_model_alpha_composition(
                            png_width, png_height, png_pixels
                        )
                    else:
                        png_width, png_height, png_pixels = read_png_rgb(
                            product_capture_path
                        )
                        require_fixture_composition(
                            png_width, png_height, png_pixels
                        )
                except (OSError, ValueError) as error:
                    failures.append(
                        f"{label} did not write its requested product PNG: "
                        f"{error}"
                    )
                else:
                    png_dimensions = (png_width, png_height)
                    reported = reported_dimensions.get(label)
                    if reported is None or png_dimensions != reported[0:2]:
                        failures.append(
                            f"{label} PNG dimensions {png_dimensions!r} do not "
                            "match the exact renderer output"
                        )
                expected_queue = (
                    "kind=modern-character-alpha channels=4"
                    if capture_kind == "model-alpha"
                    else "kind=scene channels=3"
                )
                if expected_queue not in arm_output:
                    failures.append(
                        f"{label} did not queue the typed capture product"
                    )
                if capture_kind == "model-alpha" and (
                    "[WGPU-CHARACTER-CAPTURE] ready=1" not in arm_output
                ):
                    failures.append(
                        f"{label} did not prove isolated renderer capture"
                    )
                if capture_kind == "model-alpha":
                    capture_match = re.search(
                        r"\[WGPU-CHARACTER-CAPTURE\] ready=1 subject=0 "
                        r"draws=(\d+)/(\d+) target=\d+x\d+ "
                        r"projection=target-to-clip",
                        arm_output,
                    )
                    if capture_match is None:
                        failures.append(
                            f"{label} did not publish a subject-scoped projection"
                        )
                    else:
                        rendered, recorded = map(int, capture_match.groups())
                        if rendered == 0 or rendered != recorded:
                            failures.append(
                                f"{label} replayed an incomplete subject capture"
                            )
                        subject_capture_draws[label] = rendered

    if not failures:
        transition_dir = evidence / "car-1p-transition"
        transition_dir.mkdir(parents=True, exist_ok=True)
        transition_env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        transition_env.update(
            LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
            MDKR_PRESENT_PERF="1", MDKR_RENDERER="webgpu",
            MDKR_RENDER_SCALE="1", MDKR_VIDEO_CONFIG_PATH=os.devnull,
            MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
            MDKR_CHARACTER_WORKSHOP_PREVIEW="car",
            MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS="1",
            MDKR_CUSTOM_CHARACTER_P1=PACKAGE_ID,
            MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE="race.finish_win",
            MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE="750",
            MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_POSE=
                "select.idle",
            MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_PHASE="250",
            MDKR64_HIDDEN="1",
        )
        process = run([
            str(binary), "--headless-frames", str(FRAMES), "--rom",
            str(rom), "--window-size", "1280x960", "--restored",
        ], env=transition_env)
        transition_output = process.stdout or ""
        output += "\n===== car-1p-transition =====\n" + transition_output
        transition_match = re.search(
            r"pose=12 phase=750 transitionFrom=1 transitionPhase=250 "
            r"transition=(\d+)/(\d+)/(\d+) "
            r"transitionBlend=(\d+),(\d+) "
            r"transitionSource=([1-3]),([1-3]) "
            r"poseTicks=(\d+) poseFallback=(\d+)",
            transition_output,
        )
        if process.returncode != 0 or transition_match is None:
            failures.append(
                "exact A-to-B transition did not publish its runtime contract"
            )
        else:
            (switches, blending, completions, _, _, from_source, to_source,
             ticks, fallback) = map(int, transition_match.groups())
            if switches <= 0 or completions > switches or blending > ticks:
                failures.append(
                    "exact transition counters were internally inconsistent"
                )
            if fallback <= 0 or 3 not in (from_source, to_source):
                failures.append(
                    "transition did not expose its expected package-fallback leg"
                )

    one_player_subject = subject_capture_draws.get(
        "car-1p-pose-model-alpha-capture"
    )
    four_player_subject = subject_capture_draws.get(
        "car-4p-pose-model-alpha-capture"
    )
    if (one_player_subject is None or four_player_subject is None or
            one_player_subject != four_player_subject):
        failures.append(
            "model-alpha replay was not invariant between one- and four-player "
            f"sessions ({one_player_subject!r} vs {four_player_subject!r})"
        )

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

    transition_rejection_arms = [
        ("same-transition-semantic", "race.steer", "500",
         "invalid Character Workshop transition request"),
        ("unknown-transition-semantic", "race.dance", "500",
         "invalid Character Workshop transition request"),
        ("unpaired-transition", "select.idle", None,
         "transition source and phase must be provided together"),
    ]
    if not failures:
        for label, from_pose, from_phase, marker in transition_rejection_arms:
            env = {
                key: value for key, value in os.environ.items()
                if not key.startswith(("MDKR", "GE007_"))
            }
            env.update(
                LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                MDKR_RENDERER="webgpu", MDKR_VIDEO_CONFIG_PATH=os.devnull,
                MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
                MDKR_CHARACTER_WORKSHOP_PREVIEW="car",
                MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS="1",
                MDKR_CUSTOM_CHARACTER_P1=PACKAGE_ID,
                MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE="race.steer",
                MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE="500",
                MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_POSE=
                    from_pose,
                MDKR64_HIDDEN="1",
            )
            if from_phase is not None:
                env[
                    "MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_PHASE"
                ] = from_phase
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

    visual_rejection_arms = [
        ("unpaired-view", "car", "race.steer", "500", "90", None,
         None, None, None, 60,
         "view and lighting fields must be provided together"),
        ("invalid-view-yaw", "car", "race.steer", "500", "181", "0",
         "neutral", None, None, 60, "invalid Character Workshop view request"),
        ("invalid-view-pitch", "car", "race.steer", "500", "0", "91",
         "neutral", None, None, 60, "invalid Character Workshop view request"),
        ("invalid-lighting", "car", "race.steer", "500", "0", "0",
         "studio", None, None, 60, "invalid Character Workshop view request"),
        ("select-camera-orbit", "select", "select.idle", "500", "90", "0",
         "neutral", None, None, 60, "invalid Character Workshop view request"),
        ("visual-on-live", "car", None, None, "0", "0", "bright", None,
         None, 60, "view and lighting fields must be provided together"),
        ("capture-on-live", "car", None, None, None, None, None,
         "live-capture.png", "scene", 60,
         "invalid Character Workshop capture request"),
        ("uppercase-capture", "car", "race.steer", "500", None, None,
         None, "inspection.PNG", "scene", 60,
         "invalid Character Workshop capture request"),
        ("existing-capture", "car", "race.steer", "500", "0", "0",
         "neutral", "existing.png", "scene", 180,
         "Character Workshop capture could not be armed"),
        ("missing-capture-kind", "car", "race.steer", "500", "0", "0",
         "neutral", "missing-kind.png", None, 60,
         "capture path and kind must be provided together"),
        ("capture-kind-without-path", "car", "race.steer", "500", "0", "0",
         "neutral", None, "scene", 60,
         "capture path and kind must be provided together"),
        ("invalid-capture-kind", "car", "race.steer", "500", "0", "0",
         "neutral", "invalid-kind.png", "matte", 60,
         "invalid Character Workshop capture kind: matte"),
    ]
    if not failures:
        for (label, context, pose, phase, yaw, pitch, lighting,
             capture_name, capture_kind, frames, marker) in visual_rejection_arms:
            arm_dir = evidence / label
            arm_dir.mkdir(parents=True, exist_ok=True)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("MDKR", "GE007_"))}
            env.update(
                LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                MDKR_RENDERER="webgpu", MDKR_VIDEO_CONFIG_PATH=os.devnull,
                MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
                MDKR_CHARACTER_WORKSHOP_PREVIEW=context,
                MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS="1",
                MDKR_CUSTOM_CHARACTER_P1=PACKAGE_ID,
                MDKR64_HIDDEN="1",
            )
            if pose is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE"] = pose
            if phase is not None:
                env["MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE"] = phase
            if yaw is not None:
                env["MDKR_CHARACTER_WORKSHOP_VIEW_YAW_DEGREES"] = yaw
            if pitch is not None:
                env["MDKR_CHARACTER_WORKSHOP_VIEW_PITCH_DEGREES"] = pitch
            if lighting is not None:
                env["MDKR_CHARACTER_WORKSHOP_LIGHTING"] = lighting
            if capture_name is not None:
                capture_path = arm_dir / capture_name
                if label == "existing-capture":
                    capture_path.write_bytes(b"preserve me")
                env["MDKR_CHARACTER_WORKSHOP_CAPTURE_PNG"] = str(capture_path)
            if capture_kind is not None:
                env["MDKR_CHARACTER_WORKSHOP_CAPTURE_KIND"] = capture_kind
            process = run([
                str(binary), "--headless-frames", str(frames), "--rom",
                str(rom), "--window-size", "1280x960", "--restored",
            ], env=env)
            arm_output = process.stdout or ""
            output += f"\n===== {label} =====\n{arm_output}"
            if process.returncode == 0:
                failures.append(f"{label} did not fail closed")
            if marker not in arm_output:
                failures.append(f"{label} did not report its exact refusal")
            if label == "existing-capture" and capture_path.read_bytes() != b"preserve me":
                failures.append("existing capture was modified despite refusal")

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
        "and runtime A/B cross-fades with per-leg blend/source/fallback "
        "accounting, deterministic camera/light controls, "
        "target-frame anchor/bounds/facing plus exact gameplay-camera/anatomy "
        "measurements, exclusive stabilized "
        "RGB gameplay and transparent RGBA model-only PNG capture, "
        "exact four-contact post-solve witnesses and qualified retained-vehicle "
        "surface intersection samples, exact isolated-versus-scene "
        "opaque-depth region evidence, one-to-four-player WebGPU "
        "stress, exact nonblocking scene/character GPU timestamp contracts "
        "with honest capability fallback, and fail-closed invalid requests"
    )
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
