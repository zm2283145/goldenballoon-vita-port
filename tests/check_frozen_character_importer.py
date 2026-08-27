#!/usr/bin/env python3
"""Exercise raw-model intake through a built Character Workshop importer."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from test_character_asset_probe import make_animated_glb  # noqa: E402


MAX_OUTPUT_BYTES = 64 * 1024
INDEX_NAME = ".launcher-character-glb-intake.tsv"
RESULT_NAME = ".launcher-character-result.json"


class SmokeError(ValueError):
    """The frozen helper did not fulfill its authoring contract."""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise SmokeError(f"duplicate result key: {key}")
        result[key] = value
    return result


def _load_result(path: Path) -> dict[str, Any]:
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_OUTPUT_BYTES:
        raise SmokeError("raw intake result has an invalid size")
    try:
        result = json.loads(
            payload.decode("utf-8"), object_pairs_hook=_strict_object,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SmokeError("raw intake result is not strict UTF-8 JSON") from exc
    if not isinstance(result, dict):
        raise SmokeError("raw intake result root must be an object")
    return result


def check(executable_path: Path) -> None:
    if executable_path.is_symlink() or not executable_path.is_file():
        raise SmokeError("importer must be a regular, non-symlink file")
    executable = executable_path.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="mdkr-frozen-importer-smoke.") as raw:
        temporary = Path(raw)
        character_dir = temporary / "player data"
        character_dir.mkdir()
        model = temporary / "fixture model.glb"
        model_payload = make_animated_glb()
        model.write_bytes(model_payload)
        result_path = character_dir / RESULT_NAME
        index_path = character_dir / INDEX_NAME
        command = [
            str(executable), "--directory", str(character_dir),
            "--result-file", str(result_path), "write-raw-glb-index",
            str(model), str(index_path),
        ]
        try:
            completed = subprocess.run(
                command, cwd=temporary, check=False,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise SmokeError(f"could not execute frozen importer: {exc}") from exc
        if (len(completed.stdout) > MAX_OUTPUT_BYTES or
                len(completed.stderr) > MAX_OUTPUT_BYTES):
            raise SmokeError("frozen importer emitted oversized output")
        if completed.returncode != 0:
            detail = completed.stderr.decode("utf-8", errors="replace").strip()
            raise SmokeError(
                f"frozen importer exited {completed.returncode}: {detail}"
            )
        result = _load_result(result_path)
        expected = {
            "ok": True,
            "schema": "mdkr-character-glb-intake-v2",
            "model_sha256": hashlib.sha256(model_payload).hexdigest(),
            "vertices": 3,
            "triangles": 1,
            "materials": 1,
            "textures": 1,
            "skins": 1,
            "joints": 2,
            "source_height_m": 1.0,
            "mesh_local_bounds": [[-0.5, 0.0, 0.0], [0.5, 1.0, 0.0]],
            "scene_world_bounds": [[-0.5, 0.0, 0.0], [0.5, 1.0, 0.0]],
            "source_animation_count": 1,
            "clips": ["idle"],
            "nodes": ["root", "head", "character"],
            "fallback": "idle",
            "seat": "root",
            "head": "head",
            "index_file": INDEX_NAME,
            "warnings": [],
        }
        for key, value in expected.items():
            if result.get(key) != value:
                raise SmokeError(
                    f"raw intake {key} mismatch: expected {value!r}, "
                    f"found {result.get(key)!r}"
                )
        validation = result.get("validation")
        if not isinstance(validation, dict) or validation.get("errors") != 0:
            raise SmokeError("raw intake did not preserve validator success")
        if (
            validation.get("schema") != "mdkr-gltf-validation-v1"
            or validation.get("source_sha256") != expected["model_sha256"]
            or validation.get("validator_version") != "2.0.0-dev.3.10"
            or validation.get("validator_commit") !=
                "bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1"
            or not isinstance(validation.get("validator_sha256"), str)
            or len(validation["validator_sha256"]) != 64
        ):
            raise SmokeError("raw intake validator provenance is malformed")
        expected_keys = set(expected) | {"model", "validation"}
        if set(result) != expected_keys:
            raise SmokeError("raw intake result has an unexpected schema")
        if Path(result["model"]).resolve(strict=True) != model.resolve(strict=True):
            raise SmokeError("raw intake result identifies the wrong model")
        expected_index = (
            "mdkr-character-glb-intake-v2\t"
            f"{expected['model_sha256']}\t3\t1\t1\t1\t1\t2\t1\t1\t3"
            "\t-0.5\t0\t0\t0.5\t1\t0\t-0.5\t0\t0\t0.5\t1\t0\n"
            "defaults\t69646c65\t726f6f74\t68656164\n"
            "clip\t69646c65\n"
            "node\t726f6f74\n"
            "node\t68656164\n"
            "node\t636861726163746572\n"
        )
        if index_path.read_text(encoding="ascii") != expected_index:
            raise SmokeError("raw intake fixed-field index changed")
        if sorted(path.name for path in character_dir.iterdir()) != [
                INDEX_NAME, RESULT_NAME]:
            raise SmokeError("raw inventory smoke published unexpected files")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args(argv)
    try:
        check(args.executable)
    except (OSError, SmokeError) as exc:
        print(f"frozen character importer smoke failed: {exc}", file=sys.stderr)
        return 2
    print("frozen character importer raw-GLB smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
