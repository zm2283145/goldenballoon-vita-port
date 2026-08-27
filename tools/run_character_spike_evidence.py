#!/usr/bin/env python3
"""Run isolated, redacted custom-character release-spike evidence.

The command accepts either the built-in ``fixture`` source or an ordinary GLB,
DAE, or unambiguous ZIP plus a reviewed decisions file. It never installs into
the player's character library. Source conversion, packaging, compilation, and
the disposable runtime catalog live in an operating-system temporary directory;
the selected evidence directory receives only redacted logs, screenshots, and
``evidence.json``.

Native Khronos validation is mandatory for external sources. The explicit
synthetic seam exists only so CI can exercise the generated CC0 fixture on
machines that do not carry the pinned platform validator binary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_compiler as compiler  # noqa: E402
import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_spike_fixture as fixture  # noqa: E402


EVIDENCE_SCHEMA = "mdkr-character-spike-evidence-v1"
DECISIONS_SCHEMA = "mdkr-character-spike-decisions-v1"
MAX_LOG_BYTES = 8 * 1024 * 1024
RUN_FRAMES = 360
CONTEXTS = (
    ("select", 1, "select.idle"),
    ("car", 1, "race.steer"),
    ("hovercraft", 1, "race.steer"),
    ("plane", 1, "race.steer"),
    ("car", 4, "race.steer"),
)


class EvidenceError(RuntimeError):
    """Actionable refusal from the evidence boundary."""


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _file_record(path: Path) -> dict[str, Any]:
    payload = path.read_bytes()
    return {"bytes": len(payload), "sha256": _sha256(payload)}


def _run(
    command: list[str], *, env: dict[str, str] | None = None,
    timeout: int = 180,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command, cwd=ROOT, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise EvidenceError(
            f"command exceeded the {timeout}-second evidence bound: "
            f"{Path(command[0]).name}"
        ) from error


def _json_result(process: subprocess.CompletedProcess[str], action: str) -> dict:
    if process.returncode != 0:
        detail = (process.stdout or "").strip()[-2000:]
        raise EvidenceError(f"{action} failed: {detail}")
    try:
        parsed = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        raise EvidenceError(f"{action} returned malformed JSON") from error
    if not isinstance(parsed, dict) or parsed.get("ok", True) is False:
        raise EvidenceError(f"{action} did not report success")
    return parsed


def _bounded_json(path: Path, maximum: int, description: str) -> dict:
    if not path.is_file() or path.stat().st_size > maximum:
        raise EvidenceError(f"{description} is missing or exceeds {maximum} bytes")
    try:
        parsed = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{description} is not strict UTF-8 JSON") from error
    if not isinstance(parsed, dict):
        raise EvidenceError(f"{description} must be a JSON object")
    return parsed


def _require_text(value: Any, field: str, maximum: int) -> str:
    if (not isinstance(value, str) or not value.strip() or
            len(value.encode("utf-8")) > maximum or
            any(ord(character) < 0x20 for character in value)):
        raise EvidenceError(f"reviewed decisions field {field} is invalid")
    return value


def _external_manifest(
    model: Path, decisions_path: Path, portrait: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    decisions = _bounded_json(decisions_path, 256 * 1024, "reviewed decisions")
    expected = {
        "schema", "id", "display_name", "license_spdx", "attribution",
        "source_url", "donor", "vehicles", "source_forward",
        "target_height_m", "minimap_rgb", "sockets", "rig_roles",
        "disabled_semantics",
    }
    unknown = set(decisions) - expected
    missing = expected - set(decisions)
    if decisions.get("schema") != DECISIONS_SCHEMA or unknown or missing:
        detail = []
        if decisions.get("schema") != DECISIONS_SCHEMA:
            detail.append("unsupported schema")
        if unknown:
            detail.append("unknown: " + ", ".join(sorted(unknown)))
        if missing:
            detail.append("missing: " + ", ".join(sorted(missing)))
        raise EvidenceError("reviewed decisions are invalid (" + "; ".join(detail) + ")")
    package_id = _require_text(decisions["id"], "id", 64)
    if probe.ID_RE.fullmatch(package_id) is None:
        raise EvidenceError("reviewed decisions id is not a safe package id")
    display_name = _require_text(decisions["display_name"], "display_name", 96)
    spdx = _require_text(decisions["license_spdx"], "license_spdx", 128)
    attribution = _require_text(decisions["attribution"], "attribution", 256)
    source_url = _require_text(decisions["source_url"], "source_url", 2048)
    if re.fullmatch(r"https?://[^\s]+", source_url) is None:
        raise EvidenceError(
            "reviewed decisions source_url must be an HTTP(S) provenance URL, "
            "not a local path"
        )
    donor = decisions["donor"]
    vehicles = decisions["vehicles"]
    source_forward = decisions["source_forward"]
    height = decisions["target_height_m"]
    minimap_rgb = decisions["minimap_rgb"]
    sockets = decisions["sockets"]
    roles = decisions["rig_roles"]
    disabled = decisions["disabled_semantics"]
    if donor not in probe.GAMEPLAY_DONORS:
        raise EvidenceError("reviewed decisions donor is unsupported")
    if (not isinstance(vehicles, list) or not vehicles or
            len(set(vehicles)) != len(vehicles) or
            any(vehicle not in probe.VEHICLE_NAMES for vehicle in vehicles)):
        raise EvidenceError("reviewed decisions vehicles are invalid")
    if source_forward not in probe.SOURCE_FORWARD_AXES:
        raise EvidenceError("reviewed decisions source_forward is invalid")
    if (isinstance(height, bool) or not isinstance(height, (int, float)) or
            not 0.1 <= float(height) <= 10.0):
        raise EvidenceError("reviewed decisions target_height_m is invalid")
    if (not isinstance(minimap_rgb, list) or len(minimap_rgb) != 3 or
            any(isinstance(value, bool) or not isinstance(value, int) or
                not 0 <= value <= 255 for value in minimap_rgb)):
        raise EvidenceError("reviewed decisions minimap_rgb is invalid")
    if (not isinstance(sockets, dict) or set(sockets) != {"seat", "head"} or
            any(not isinstance(value, str) for value in sockets.values())):
        raise EvidenceError("reviewed decisions sockets must name seat and head")
    if (not isinstance(roles, dict) or set(roles) != set(probe.HUMANOID_ROLES) or
            any(not isinstance(value, str) or not value for value in roles.values()) or
            len(set(roles.values())) != len(roles)):
        raise EvidenceError("reviewed decisions must map 16 distinct humanoid roles")
    if (not isinstance(disabled, list) or len(set(disabled)) != len(disabled) or
            any(value not in probe.DISABLEABLE_SEMANTICS for value in disabled)):
        raise EvidenceError("reviewed decisions disabled_semantics are invalid")

    generated, inferred = wizard.build_manifest(
        model, package_id, display_name, spdx, attribution, source_url,
        donor, vehicles, source_forward=source_forward,
        target_height_m=float(height), socket_overrides=sockets,
        portrait=portrait, minimap_rgb=minimap_rgb,
    )
    generated["schema"] = probe.PACKAGE_SCHEMA_V4
    generated["rig"] = {
        "mode": "humanoid-retarget-v1",
        "reviewed": True,
        "roles": {
            role: {
                "node": node,
                "inferred": False,
                "confidence": 1.0,
            }
            for role, node in roles.items()
        },
    }
    mapped = generated["animations"].get("states", {})
    absent = [semantic for semantic in disabled if semantic not in mapped]
    if absent:
        raise EvidenceError(
            "reviewed disabled semantics have no inferred source mapping: "
            + ", ".join(absent)
        )
    if disabled:
        generated["animations"]["disabled_states"] = disabled
    report = probe.inspect_glb(model, require_character=True)
    errors = probe.validate_manifest(generated, report)
    if errors:
        raise EvidenceError("reviewed manifest is invalid: " + "; ".join(errors))
    public_decisions = {
        "id": package_id,
        "display_name": display_name,
        "license_spdx": spdx,
        "attribution": attribution,
        "source_url": source_url,
        "donor": donor,
        "vehicles": vehicles,
        "source_forward": source_forward,
        "target_height_m": float(height),
        "sockets": sockets,
        "rig_mode": "humanoid-retarget-v1",
        "rig_reviewed": True,
        "rig_roles": len(roles),
        "disabled_semantics": disabled,
        "wizard_inference": {
            "fallback": inferred["fallback"],
            "mapped_states": inferred["mapped_states"],
        },
        "portrait_source": "procedural evidence placeholder",
    }
    return generated, public_decisions


def _ppm(path: Path) -> tuple[int, int, bytes]:
    payload = path.read_bytes()
    if not payload.startswith(b"P6\n"):
        raise EvidenceError("renderer screenshot is not binary PPM")
    offset = 3
    tokens: list[bytes] = []
    while len(tokens) < 3:
        while offset < len(payload) and payload[offset] in b" \t\r\n":
            offset += 1
        if offset < len(payload) and payload[offset] == ord("#"):
            end = payload.find(b"\n", offset)
            if end < 0:
                raise EvidenceError("renderer screenshot has a truncated comment")
            offset = end + 1
            continue
        end = offset
        while end < len(payload) and payload[end] not in b" \t\r\n":
            end += 1
        if end == offset:
            raise EvidenceError("renderer screenshot header is truncated")
        tokens.append(payload[offset:end])
        offset = end
    try:
        width, height, maximum = map(int, tokens)
    except ValueError as error:
        raise EvidenceError("renderer screenshot dimensions are invalid") from error
    if payload[offset:offset + 2] == b"\r\n":
        offset += 2
    elif offset < len(payload) and payload[offset] in b" \t\r\n":
        offset += 1
    else:
        raise EvidenceError("renderer screenshot header has no pixel delimiter")
    pixels = payload[offset:]
    if (width <= 0 or height <= 0 or maximum != 255 or
            len(pixels) != width * height * 3):
        raise EvidenceError("renderer screenshot pixel payload is invalid")
    return width, height, pixels


def _rgb_png(width: int, height: int, pixels: bytes) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        scanlines.extend(pixels[y * width * 3:(y + 1) * width * 3])
    return (
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                   8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(bytes(scanlines), 9)) +
        chunk(b"IEND", b"")
    )


def _redact(text: str, secrets: list[str]) -> str:
    redacted = text
    replacements = (
        "<SOURCE>", "<LICENSE>", "<ROM>", "<TEMP>", "<DECISIONS>",
    )
    ordered = sorted(
        {secret for secret in secrets if secret}, key=len, reverse=True
    )
    for index, secret in enumerate(ordered):
        replacement = replacements[index] if index < len(replacements) \
            else "<PRIVATE>"
        redacted = redacted.replace(secret, replacement)
    encoded = redacted.encode("utf-8", errors="replace")
    if len(encoded) > MAX_LOG_BYTES:
        redacted = encoded[-MAX_LOG_BYTES:].decode("utf-8", errors="replace")
        redacted = "<LOG-TRUNCATED>\n" + redacted
    return redacted


def _parse_result(output: str, label: str) -> dict[str, Any]:
    result_lines = [line for line in output.splitlines()
                    if "character_workshop_result:" in line]
    if not result_lines:
        raise EvidenceError(f"{label} emitted no exact result")
    line = result_lines[-1]
    patterns = {
        "measurement": r"warmup=(\d+) realtime=(\d+)",
        "timing": (
            r"samples=(\d+) p50us=(\d+) p95us=(\d+) p99us=(\d+) "
            r"maxus=(\d+) replacements=(\d+)"
        ),
        "contact": (
            r"contacts=(\d+) contactMaxUm=(\d+) contactWitness=([0-9a-f]+) "
            r"contactWitnessErrorUm=(\d+),(\d+),(\d+),(\d+)"
        ),
        "fit": (
            r"fit=(\d+) fitAnchorUm=(-?\d+),(-?\d+),(-?\d+) "
            r"fitBoundsYUm=(-?\d+),(-?\d+) "
            r"fitForwardMilli=(-?\d+),(-?\d+),(-?\d+)"
        ),
        "pose": r"pose=(\d+) phase=(\d+) poseTicks=(\d+) poseFallback=(\d+)",
        "gpu": (
            r"gpu=(\d+)/([0-9a-f]+) sceneGpuNs=(\d+),(\d+),(\d+) "
            r"characterGpuNs=(\d+),(\d+),(\d+)"
        ),
        "environment": (
            r"backend=([^ ]+) adapter=(.*?) driver=(.*?) "
            r"vendor=([0-9a-f]{8}) device=([0-9a-f]{8}) "
            r"output=(\d+)x(\d+) render=(\d+)x(\d+)"
        ),
    }
    matches = {name: re.search(pattern, line) for name, pattern in patterns.items()}
    missing = [name for name, match in matches.items() if match is None]
    if missing:
        raise EvidenceError(f"{label} result omits: {', '.join(missing)}")
    measurement = [int(value) for value in matches["measurement"].groups()]
    timing = [int(value) for value in matches["timing"].groups()]
    contact_groups = matches["contact"].groups()
    contact = [int(contact_groups[0]), int(contact_groups[1]),
               int(contact_groups[2], 16),
               *(int(value) for value in contact_groups[3:])]
    fit = [int(value) for value in matches["fit"].groups()]
    pose = [int(value) for value in matches["pose"].groups()]
    gpu_groups = matches["gpu"].groups()
    gpu = [int(gpu_groups[0]), int(gpu_groups[1], 16),
           *(int(value) for value in gpu_groups[2:])]
    environment = matches["environment"].groups()
    if measurement != [1, 1]:
        raise EvidenceError(
            f"{label} did not produce a warmed real-time measurement"
        )
    if timing[0] < 40 or timing[5] <= 0:
        raise EvidenceError(f"{label} did not produce a warmed replacement window")
    if fit[0] != 1 or fit[4] > fit[5]:
        raise EvidenceError(f"{label} returned invalid fit evidence")
    if pose[2] <= 0 or pose[3] != 0:
        raise EvidenceError(f"{label} used an unintended package fallback pose")
    if "[WGPU-MODERN-CHARACTER]" not in output or "refusedDraws=0" not in output:
        raise EvidenceError(f"{label} did not render a clean modern character")
    return {
        "measurement": {
            "warmup_completed": True,
            "realtime_pacing": True,
        },
        "wall_microseconds": {
            "samples": timing[0], "p50": timing[1], "p95": timing[2],
            "p99": timing[3], "max": timing[4],
        },
        "replacements": timing[5],
        "contacts": {
            "solves": contact[0], "maximum_micrometres": contact[1],
            "witness_mask": contact[2],
            "limb_error_micrometres": contact[3:7],
        },
        "fit": {
            "anchor_micrometres": fit[1:4],
            "bounds_y_micrometres": fit[4:6],
            "forward_milli": fit[6:9],
        },
        "pose": {
            "id": pose[0], "phase_milli": pose[1],
            "ticks": pose[2], "package_fallback_ticks": pose[3],
        },
        "gpu": {
            "status": gpu[0], "scope_mask": gpu[1],
            "scene_samples": gpu[2], "scene_p50_nanoseconds": gpu[3],
            "scene_p95_nanoseconds": gpu[4],
            "character_samples": gpu[5],
            "character_p50_nanoseconds": gpu[6],
            "character_p95_nanoseconds": gpu[7],
        },
        "environment": {
            "backend": environment[0], "adapter": environment[1],
            "driver": environment[2], "vendor": environment[3],
            "device": environment[4],
            "output": [int(environment[5]), int(environment[6])],
            "render": [int(environment[7]), int(environment[8])],
        },
    }


def _exact_context(
    binary: Path, rom: Path, characters: Path, package_id: str,
    context: str, players: int, semantic: str, work: Path,
    evidence: Path, secrets: list[str],
) -> dict[str, Any]:
    label = f"{context}-{players}p"
    arm = work / label
    arm.mkdir(parents=True)
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update({
        "LC_ALL": "C", "MDKR_AUDIO": "0", "MDKR_TRACE": "1",
        "MDKR_PRESENT_PERF": "1", "MDKR_RENDERER": "webgpu",
        "MDKR_PACE_REALTIME": "1",
        "MDKR_SIMULATION_CADENCE": "enhanced",
        "MDKR_RENDER_SCALE": "1", "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_WORKSHOP_PREVIEW": context,
        "MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS": str(players),
        "MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE": semantic,
        "MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE": "500",
        "MDKR64_HIDDEN": "1",
    })
    for player in range(players):
        env[f"MDKR_CUSTOM_CHARACTER_P{player + 1}"] = package_id
    command = [
        str(binary), "--headless-frames", str(RUN_FRAMES), "--rom", str(rom),
        "--window-size", "1280x960", "--restored",
    ]
    process = _run(command, env=env, timeout=180)
    output = process.stdout or ""
    log_name = f"{label}.log"
    (evidence / log_name).write_text(
        _redact("[REALTIME-MEASUREMENT]\n" + output, secrets),
        encoding="utf-8",
    )
    if process.returncode != 0:
        raise EvidenceError(f"{label} exact renderer exited with {process.returncode}")
    expected = f"character_workshop_preview: started context={context} players={players}"
    if expected not in output:
        raise EvidenceError(f"{label} did not enter the requested game context")
    parsed = _parse_result(output, label)

    # Screenshot publication performs synchronous filesystem work by design.
    # Keep that cost out of the real-time cadence window instead of allowing
    # one PNG/PPM write to masquerade as a rendering p99 regression. The
    # second, deterministic run must still enter the exact context and render
    # the same clean replacement; only its timing is deliberately discarded.
    capture_env = dict(env)
    capture_env.pop("MDKR_PACE_REALTIME", None)
    capture_env.update({
        "MDKR_DUMP_FROM": str(RUN_FRAMES - 2),
        "MDKR_DUMP_EVERY": "10000",
    })
    capture = _run(
        command + ["--dump-frames", str(arm)],
        env=capture_env, timeout=180,
    )
    capture_output = capture.stdout or ""
    combined_log = (
        "[REALTIME-MEASUREMENT]\n" + output +
        "\n[ISOLATED-SCREENSHOT-CAPTURE]\n" + capture_output
    )
    (evidence / log_name).write_text(
        _redact(combined_log, secrets), encoding="utf-8"
    )
    if capture.returncode != 0:
        raise EvidenceError(
            f"{label} screenshot renderer exited with {capture.returncode}"
        )
    if expected not in capture_output:
        raise EvidenceError(
            f"{label} screenshot run did not enter the requested game context"
        )
    if ("[WGPU-MODERN-CHARACTER]" not in capture_output or
            "refusedDraws=0" not in capture_output):
        raise EvidenceError(
            f"{label} screenshot run did not render a clean modern character"
        )
    ppm_files = sorted(arm.glob("frame_*.ppm"))
    if len(ppm_files) != 1:
        raise EvidenceError(f"{label} produced {len(ppm_files)} screenshots")
    width, height, pixels = _ppm(ppm_files[0])
    screenshot_name = f"{label}.png"
    screenshot = evidence / screenshot_name
    screenshot.write_bytes(_rgb_png(width, height, pixels))
    parsed.update({
        "context": context,
        "players": players,
        "semantic": semantic,
        "screenshot": {
            "file": screenshot_name,
            "width": width,
            "height": height,
            "separate_from_timing": True,
            **_file_record(screenshot),
        },
        "log": log_name,
    })
    return parsed


def _ensure_evidence_destination(path: Path) -> None:
    if path.exists():
        if not path.is_dir():
            raise EvidenceError("evidence destination is not a directory")
        if any(path.iterdir()):
            raise EvidenceError(
                "evidence destination must be new or empty; existing evidence "
                "is never overwritten"
            )
    else:
        path.mkdir(parents=True)


def _git_revision() -> str:
    result = _run(["git", "rev-parse", "HEAD"], timeout=10)
    value = (result.stdout or "").strip()
    return value if result.returncode == 0 and re.fullmatch(
        r"[0-9a-f]{40}", value
    ) else "unavailable"


def run_evidence(args: argparse.Namespace) -> dict[str, Any]:
    evidence = args.evidence_dir.resolve()
    _ensure_evidence_destination(evidence)
    rom = args.rom.resolve(strict=True)
    binary = (args.binary.resolve(strict=True) if args.binary else
              (args.build.resolve() / "mdkr64").resolve(strict=True))
    if not os.access(binary, os.X_OK):
        raise EvidenceError("selected game binary is not executable")
    fixture_source = args.source == "fixture"
    if args.synthetic_validation_seam and not fixture_source:
        raise EvidenceError(
            "the synthetic validation seam is forbidden for external sources"
        )
    source_path = None if fixture_source else Path(args.source).resolve(strict=True)
    license_path = args.license.resolve(strict=True)
    if not license_path.is_file() or license_path.stat().st_size > 1024 * 1024:
        raise EvidenceError("license input is missing or exceeds 1 MiB")
    decisions_path = args.decisions.resolve(strict=True) if args.decisions else None
    if not fixture_source and decisions_path is None:
        conventional = source_path.with_suffix(
            source_path.suffix + ".mdkr-character-spike.json"
        )
        if conventional.is_file():
            decisions_path = conventional.resolve()
        else:
            raise EvidenceError(
                "external evidence requires --decisions or the adjacent "
                f"{conventional.name} reviewed decision file"
            )
    secrets = [str(rom), str(license_path)]
    if source_path is not None:
        secrets.append(str(source_path))
    if decisions_path is not None:
        secrets.append(str(decisions_path))

    with tempfile.TemporaryDirectory(prefix="mdkr-character-spike-") as temporary:
        work = Path(temporary)
        secrets.append(str(work))
        source_work = work / "source"
        source_work.mkdir()
        characters = work / "characters"
        package = work / "candidate.mdkrchar"
        if fixture_source:
            fixture_report = fixture.write_fixture(source_work)
            model = source_work / "model.glb"
            portrait = source_work / "portrait.png"
            manifest_path = source_work / "manifest.json"
            generated_license = source_work / "LICENSE.txt"
            if generated_license.read_bytes() != license_path.read_bytes():
                raise EvidenceError(
                    "fixture --license bytes do not match the checked-in CC0 license"
                )
            source_record = fixture_report["files"]["model.glb"]
            public_decisions = {
                "id": fixture.FIXTURE_ID,
                "display_name": fixture.FIXTURE_NAME,
                "license_spdx": "CC0-1.0",
                "attribution": fixture.FIXTURE_ATTRIBUTION,
                "source_url": fixture.FIXTURE_SOURCE_URL,
                "donor": "diddy",
                "vehicles": ["car", "hovercraft", "plane"],
                "source_forward": "+z",
                "target_height_m": 1.25,
                "sockets": {"seat": "Hip", "head": "Head"},
                "rig_mode": "humanoid-retarget-v1",
                "rig_reviewed": True,
                "rig_roles": 16,
                "disabled_semantics": ["select.idle"],
                "portrait_source": "procedural CC0 fixture",
                "adversarial_properties": fixture_report["properties"],
            }
        else:
            if source_path.suffix.lower() == ".glb":
                model = source_path
            elif source_path.suffix.lower() in (".dae", ".zip"):
                model = source_work / "converted.glb"
                convert_env = os.environ.copy()
                if args.validator:
                    convert_env["MDKR_GLTF_VALIDATOR"] = str(
                        args.validator.resolve(strict=True)
                    )
                converted = _run([
                    sys.executable, str(ROOT / "tools" /
                                         "character_package_manager.py"),
                    "convert-authoring-source", str(source_path), str(model),
                ], env=convert_env)
                _json_result(converted, "authoring conversion")
            else:
                raise EvidenceError("--source must be fixture, GLB, DAE, or ZIP")
            portrait = source_work / "portrait.png"
            portrait.write_bytes(fixture.portrait_png())
            generated, public_decisions = _external_manifest(
                model, decisions_path, portrait
            )
            manifest_path = source_work / "manifest.json"
            manifest_path.write_text(
                json.dumps(generated, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            source_record = _file_record(source_path)

        packed = _run([
            sys.executable, str(ROOT / "tools" / "character_asset_probe.py"),
            "pack", "--model", str(model), "--manifest", str(manifest_path),
            "--license", str(license_path), "--portrait", str(portrait),
            "--output", str(package),
        ])
        pack_result = _json_result(packed, "source package build")
        manager = (ROOT / "tests" / "run_character_manager_fixture.py"
                   if args.synthetic_validation_seam else
                   ROOT / "tools" / "character_package_manager.py")
        install_env = os.environ.copy()
        if args.validator:
            install_env["MDKR_GLTF_VALIDATOR"] = str(
                args.validator.resolve(strict=True)
            )
        installed = _run([
            sys.executable, str(manager), "--directory", str(characters),
            "install", str(package),
        ], env=install_env)
        install_result = _json_result(installed, "isolated reviewed installation")
        report = install_result.get("report")
        validation = install_result.get("validation")
        if not isinstance(report, dict) or not isinstance(validation, dict):
            raise EvidenceError("installation omitted compiler or validator evidence")
        if report.get("rig_reviewed") is not True or report.get("rig_roles") != 16:
            raise EvidenceError("installed source is not a reviewed 16-role humanoid")

        context_results = []
        comparison_environment = None
        for context, players, semantic in CONTEXTS:
            result = _exact_context(
                binary, rom, characters, install_result["id"], context,
                players, semantic, work, evidence, secrets,
            )
            if context == "select" and result["contacts"]["solves"] != 0:
                raise EvidenceError("select evidence fabricated vehicle contacts")
            if context != "select" and result["contacts"]["solves"] <= 0:
                raise EvidenceError(f"{context} evidence did not execute contacts")
            environment = result["environment"]
            if comparison_environment is None:
                comparison_environment = environment
            elif environment != comparison_environment:
                raise EvidenceError("exact context GPU/output identity changed mid-run")
            context_results.append(result)

        performance_target = all(
            result["wall_microseconds"]["p95"] <= 18_334 and
            result["wall_microseconds"]["p99"] <= 25_000
            for result in context_results
        )
        contact_advisory = {
            result["context"]: {
                "maximum_millimetres":
                    result["contacts"]["maximum_micrometres"] / 1000.0,
                "within_initial_hand_target": all(
                    error <= 25_000
                    for error in result["contacts"]["limb_error_micrometres"][:2]
                ) if result["context"] != "select" else None,
                "within_initial_foot_target": all(
                    error <= 40_000
                    for error in result["contacts"]["limb_error_micrometres"][2:]
                ) if result["context"] != "select" else None,
            }
            for result in context_results if result["players"] == 1
        }
        rom_bytes = rom.read_bytes()
        evidence_manifest = {
            "schema": EVIDENCE_SCHEMA,
            "status": "pass",
            "captured_unix": int(time.time()),
            "source": {
                "kind": "generated-cc0-fixture" if fixture_source
                        else source_path.suffix.lower().lstrip("."),
                **source_record,
            },
            "license": {
                **_file_record(license_path),
                "spdx_from_reviewed_input": public_decisions["license_spdx"],
            },
            "rom": {"sha256": _sha256(rom_bytes), "bytes": len(rom_bytes)},
            "tools": {
                "git_revision": _git_revision(),
                "python": platform.python_version(),
                "compiler": compiler.COMPILER_ID,
                "app_sha256": _file_record(binary)["sha256"],
                "validator": {
                    "version": validation.get("validator_version"),
                    "commit": validation.get("validator_commit"),
                    "target": validation.get("validator_target"),
                    "executable_sha256": validation.get("validator_sha256"),
                    "synthetic_seam": bool(args.synthetic_validation_seam),
                },
            },
            "decisions": public_decisions,
            "package": {
                "source_sha256": pack_result.get("sha256"),
                "compiled_sha256": install_result.get("compiled_sha256"),
                "compiler_source_digest": install_result.get(
                    "cache_source_digest"
                ),
                "vertices": report.get("vertices"),
                "triangles": report.get("triangles"),
                "materials": report.get("materials"),
                "textures": report.get("textures"),
                "joints": report.get("joints"),
                "animations": report.get("animations"),
                "motion_channels": report.get("motion_channels"),
                "static_animations": report.get("static_animations"),
                "active_semantic_mask": report.get("semantic_mask"),
                "disabled_semantic_mask": report.get(
                    "disabled_semantic_mask"
                ),
                "disabled_semantics": report.get("disabled_semantics"),
                "rig_roles": report.get("rig_roles"),
                "rig_reviewed": report.get("rig_reviewed"),
            },
            "summary": {
                "contexts_completed": len(context_results),
                "performance_target_met": performance_target,
                "contact_advisory": contact_advisory,
            },
            "contexts": context_results,
        }
        encoded = json.dumps(evidence_manifest, indent=2, sort_keys=True) + "\n"
        forbidden = [secret for secret in secrets if secret]
        if any(secret in encoded for secret in forbidden):
            raise EvidenceError("redacted evidence manifest contains a private path")
        if any(key in evidence_manifest for key in ("model", "rom_path", "source_path")):
            raise EvidenceError("redacted evidence manifest contains private payload fields")
        (evidence / "evidence.json").write_text(encoded, encoding="utf-8")
        return evidence_manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", required=True,
        help="literal 'fixture' or a GLB, DAE, or unambiguous ZIP",
    )
    parser.add_argument("--license", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--decisions", type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--validator", type=Path)
    parser.add_argument(
        "--synthetic-validation-seam", action="store_true",
        help="CI-only: permitted exclusively with --source fixture",
    )
    args = parser.parse_args()
    try:
        result = run_evidence(args)
        print(json.dumps({
            "ok": True,
            "evidence": str(args.evidence_dir.resolve()),
            "contexts": result["summary"]["contexts_completed"],
            "performance_target_met":
                result["summary"]["performance_target_met"],
        }, indent=2, sort_keys=True))
        return 0
    except (EvidenceError, OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, indent=2),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
