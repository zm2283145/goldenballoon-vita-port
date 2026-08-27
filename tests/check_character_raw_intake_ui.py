#!/usr/bin/env python3
"""ROM-free rendered proof for resumable raw-GLB Workshop intake."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

from test_character_asset_probe import make_animated_glb  # noqa: E402
from test_collada_to_glb import DAE  # noqa: E402


def isolated_environment(root: Path, model: Path, shot: Path, *,
                         compact: bool, drop: bool) -> dict[str, str]:
    prefs = root / "prefs"
    saves = root / "saves"
    characters = root / "characters"
    prefs.mkdir(parents=True, exist_ok=True)
    saves.mkdir(parents=True, exist_ok=True)
    characters.mkdir(parents=True, exist_ok=True)
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "MDKR_APP_SMOKE_FRAMES": "12" if compact else "8",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480" if compact else "1280x720",
        "MDKR_APP_SMOKE_SHOT": str(shot),
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tools" / "character_package_manager.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if drop:
        environment["MDKR_APP_SMOKE_DROP"] = str(model)
        environment["MDKR_APP_SMOKE_DROP_FRAME"] = "1"
    if compact:
        environment.update({
            "MDKR_APP_SMOKE_TOUCH_SCROLL": "1",
            "MDKR_APP_SMOKE_TOUCH_TOKEN": "mdkr64-app-touch-v1",
        })
    return environment


def run(binary: Path, root: Path, environment: dict[str, str],
        expected: tuple[str, ...]) -> str:
    completed = subprocess.run(
        [str(binary)], cwd=root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"raw intake app exited {completed.returncode}\n"
            f"{completed.stdout[-8000:]}"
        )
    for marker in expected:
        if marker not in completed.stdout:
            raise RuntimeError(
                f"missing raw intake marker {marker!r}\n"
                f"{completed.stdout[-8000:]}"
            )
    return completed.stdout


def check_bmp(path: Path, logical_width: int, logical_height: int) -> None:
    payload = path.read_bytes()
    if len(payload) < 54 or payload[:2] != b"BM":
        raise RuntimeError(f"raw intake capture is not a BMP: {path}")
    width, height = struct.unpack_from("<ii", payload, 18)
    if width < logical_width or abs(height) < logical_height:
        raise RuntimeError(
            f"raw intake capture is undersized: {width}x{height}"
        )


def raw_inventory(root: Path) -> tuple[str, list[list[str]]]:
    path = root / "saves" / "character_raw_drafts-v1.tsv"
    lines = path.read_text(encoding="ascii").splitlines()
    header = lines[0].split("\t")
    if len(header) != 4 or header[0] != "mdkr-character-raw-drafts-v1":
        raise RuntimeError("raw draft inventory header is malformed")
    count = int(header[1])
    rows = [line.split("\t") for line in lines[1:]]
    if count != len(rows) or any(len(row) != 18 for row in rows):
        raise RuntimeError("raw draft inventory rows are malformed")
    return header[2], rows


def row_text(row: list[str], field: int) -> str:
    return bytes.fromhex(row[field]).decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr-raw-intake-ui-") as temporary:
            root = Path(temporary)
            model = root / "author-model.glb"
            model.write_bytes(make_animated_glb())
            model_b = root / "author-model-b.glb"
            changed_fixture = make_animated_glb().replace(
                b"mdkr-test-fixture", b"mdkr-test-fixturE"
            )
            if changed_fixture == model.read_bytes():
                raise RuntimeError("second GLB fixture did not change")
            model_b.write_bytes(changed_fixture)
            source_before = {
                model: hashlib.sha256(model.read_bytes()).hexdigest(),
                model_b: hashlib.sha256(model_b.read_bytes()).hexdigest(),
            }

            guidance_cases = (
                ("author-model.FBX", "FBX"),
                ("author-model.obj", "OBJ"),
                ("author-model.blend", "Blender scene"),
                ("author-model.gltf", "glTF JSON"),
                ("author-model.usdz", "USD"),
                ("author-model.ma", "native DCC scene"),
            )
            for index, (filename, format_name) in enumerate(guidance_cases):
                guidance_root = root / f"guidance-{index}"
                guidance_root.mkdir()
                source = guidance_root / filename
                source.write_bytes(
                    b"untrusted authoring source must remain byte exact\x00" +
                    bytes([index])
                )
                source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
                guidance_environment = isolated_environment(
                    guidance_root, source,
                    guidance_root / "guidance.bmp",
                    compact=False, drop=True,
                )
                expected = [
                    "active-panel=Character Workshop",
                    f"character-source-guidance kind={format_name} "
                    "direct_import=0 mutated=0",
                ]
                if index == 0:
                    (guidance_root / "video.ini").write_text(
                        "[Accessibility]\nSpeech=1\n", encoding="utf-8"
                    )
                    guidance_environment.update({
                        "MDKR_APP_SMOKE_FRAMES": "220",
                        "MDKR_APP_SMOKE_A11Y_WALK": "1",
                        "MDKR_APP_SMOKE_INPUT": "keyboard",
                        "MDKR_APP_SMOKE_INPUT_TOKEN":
                            "mdkr64-app-ui-input-v1",
                        "MDKR_A11Y_TRACE": "1",
                    })
                    expected.append("text=Copy GLB export checklist")
                run(
                    binary, guidance_root, guidance_environment,
                    tuple(expected),
                )
                if (
                    hashlib.sha256(source.read_bytes()).hexdigest()
                        != source_digest
                    or (guidance_root / "saves" /
                        "character_raw_drafts-v1.tsv").exists()
                    or any((guidance_root / "characters").iterdir())
                ):
                    raise RuntimeError(
                        f"{format_name} guidance mutated the source, created "
                        "a raw draft, or published character state"
                    )

            conversion = root / "conversion"
            conversion.mkdir()
            nested_bytes = io.BytesIO()
            with zipfile.ZipFile(nested_bytes, "w") as nested:
                nested.writestr("Character/model.dae", DAE)
            archive = conversion / "author-download.zip"
            with zipfile.ZipFile(archive, "w") as outer:
                outer.writestr(
                    "source/model-files.zip", nested_bytes.getvalue()
                )
            archive_digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            converted = conversion / "converted.glb"
            conversion_environment = isolated_environment(
                conversion, archive,
                conversion / "raw-intake-conversion.bmp",
                compact=False, drop=True,
            )
            conversion_environment.update({
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT":
                    str(converted),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN":
                    "mdkr64-character-conversion-v1",
            })
            run(
                binary, conversion, conversion_environment,
                ("character-source-conversion kind=zip converted=1 "
                 "inspected=1 missing_license=1",
                 "raw-intake resumed=1 inspected=1 mappings=1 drafts=1"),
            )
            _, conversion_rows = raw_inventory(conversion)
            if (
                not converted.is_file()
                or len(conversion_rows) != 1
                or row_text(conversion_rows[0], 2) != str(converted)
                or conversion_rows[0][13] != hashlib.sha256(
                    converted.read_bytes()
                ).hexdigest()
                or hashlib.sha256(archive.read_bytes()).hexdigest()
                    != archive_digest
                or list((conversion / "characters").glob("*.mdkc"))
            ):
                raise RuntimeError(
                    "ZIP conversion did not produce one source-bound raw "
                    "draft while preserving the archive and installed state"
                )

            conversion_accessible = root / "conversion-accessible"
            conversion_accessible.mkdir()
            (conversion_accessible / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            conversion_a11y_environment = isolated_environment(
                conversion_accessible, archive,
                conversion_accessible / "conversion-a11y.bmp",
                compact=False, drop=False,
            )
            conversion_a11y_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "260",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_SOURCE": str(archive),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT": str(
                    conversion_accessible / "converted.glb"
                ),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN":
                    "mdkr64-character-conversion-v1",
            })
            run(
                binary, conversion_accessible,
                conversion_a11y_environment,
                ("text=Converted GLB destination",
                 "text=Convert, inspect, and continue"),
            )

            wide = root / "wide"
            wide.mkdir()
            wide_shot = wide / "raw-intake-wide.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, wide_shot, compact=False, drop=True
                ),
                ("active-panel=Character Workshop",
                 "raw-intake resumed=1 inspected=1 mappings=1 drafts=1"),
            )
            check_bmp(wide_shot, 1280, 720)
            selected_a, rows = raw_inventory(wide)
            if (
                len(rows) != 1
                or rows[0][0] != selected_a
                or row_text(rows[0], 2) != str(model)
                or rows[0][13] != source_before[model]
                or tuple(row_text(rows[0], field) for field in (14, 15, 16))
                != ("idle", "root", "head")
            ):
                raise RuntimeError(
                    "first source-bound raw draft was not persisted exactly"
                )
            if (
                list((wide / "characters").glob("*.mdkc"))
                or (
                    wide / "characters" /
                    ".launcher-character-raw-candidate.mdkrchar"
                ).exists()
            ):
                raise RuntimeError(
                    "inspection published a cache or package candidate"
                )

            resume_shot = wide / "raw-intake-resume.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, resume_shot, compact=False, drop=False
                ),
                ("raw-intake resumed=1 inspected=0 mappings=0",),
            )
            check_bmp(resume_shot, 1280, 720)

            close_environment = isolated_environment(
                wide, model, wide / "raw-intake-close.bmp",
                compact=False, drop=False
            )
            close_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "close-editor",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, close_environment,
                ("raw-draft-action action=close-editor applied=1",),
            )
            close_preferences = (
                wide / "prefs" / "mdkr64_app.ini"
            ).read_text(encoding="utf-8")
            if "character_raw_editor_open=0" not in close_preferences:
                raise RuntimeError(
                    "closing raw authoring was not durable navigation state"
                )
            closed_shot = wide / "raw-intake-closed.bmp"
            closed_output = run(
                binary, wide,
                isolated_environment(
                    wide, model, closed_shot, compact=False, drop=False
                ),
                ("raw-draft-library open=0 drafts=1",),
            )
            if "raw-intake resumed=1" in closed_output:
                raise RuntimeError(
                    "a closed raw draft still monopolized the installed editor"
                )
            resume_environment = isolated_environment(
                wide, model, wide / "raw-intake-explicit-resume.bmp",
                compact=False, drop=False
            )
            resume_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "resume-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, resume_environment,
                ("raw-draft-action action=resume-selected applied=1",
                 "raw-intake resumed=1 inspected=0 mappings=0 drafts=1"),
            )

            duplicate_shot = wide / "raw-intake-duplicate.bmp"
            duplicate_environment = isolated_environment(
                wide, model, duplicate_shot, compact=False, drop=False
            )
            duplicate_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "duplicate-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, duplicate_environment,
                ("raw-draft-action action=duplicate-selected applied=1 drafts=2",),
            )
            duplicate_selected, duplicate_rows = raw_inventory(wide)
            if (
                len(duplicate_rows) != 2
                or {row_text(row, 2) for row in duplicate_rows} != {str(model)}
                or len({row[0] for row in duplicate_rows}) != 2
                or duplicate_selected == selected_a
                or any(row[13] != source_before[model]
                       for row in duplicate_rows)
            ):
                raise RuntimeError(
                    "same-source authoring branch was not independent and "
                    "source-bound"
                )
            delete_duplicate_environment = isolated_environment(
                wide, model, wide / "raw-intake-delete-duplicate.bmp",
                compact=False, drop=False
            )
            delete_duplicate_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "delete-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, delete_duplicate_environment,
                ("raw-draft-action action=delete-selected applied=1 drafts=1",),
            )
            selected_after_branch_delete, branch_rows = raw_inventory(wide)
            if (
                len(branch_rows) != 1
                or branch_rows[0][0] != selected_after_branch_delete
                or row_text(branch_rows[0], 2) != str(model)
            ):
                raise RuntimeError(
                    "deleting a same-source branch did not preserve its peer"
                )

            second_shot = wide / "raw-intake-second.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model_b, second_shot, compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            check_bmp(second_shot, 1280, 720)
            selected_b, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if (
                len(rows) != 2
                or set(by_path) != {str(model), str(model_b)}
                or by_path[str(model)][13] != source_before[model]
                or by_path[str(model_b)][13] != source_before[model_b]
                or selected_b != by_path[str(model_b)][0]
            ):
                raise RuntimeError(
                    "two raw sources did not retain independent fingerprints"
                )

            switch_shot = wide / "raw-intake-switch.bmp"
            switch_environment = isolated_environment(
                wide, model, switch_shot, compact=False, drop=False
            )
            switch_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "select-sha:" + source_before[model],
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, switch_environment,
                ("raw-draft-action action=select-sha:",
                 "applied=1 drafts=2", "raw-intake resumed=1 inspected=0"),
            )
            selected_after_switch, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if selected_after_switch != by_path[str(model)][0]:
                raise RuntimeError("explicit raw draft switch was not durable")

            restore_shot = wide / "raw-intake-mapping-restore.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, restore_shot, compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            _, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if tuple(row_text(by_path[str(model)], field)
                     for field in (14, 15, 16)) != ("idle", "root", "head"):
                raise RuntimeError(
                    "reinspection did not restore the exact source-bound mappings"
                )

            # Re-select B through the ordinary GLB source path, then exercise
            # the exact UI deletion action. Only the selected authoring record
            # may disappear; neither external source is mutable.
            run(
                binary, wide,
                isolated_environment(
                    wide, model_b, wide / "raw-intake-reselect-b.bmp",
                    compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            delete_environment = isolated_environment(
                wide, model_b, wide / "raw-intake-delete.bmp",
                compact=False, drop=False
            )
            delete_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "delete-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, delete_environment,
                ("raw-draft-action action=delete-selected applied=1 drafts=1",),
            )
            selected_after_delete, rows = raw_inventory(wide)
            if (
                len(rows) != 1
                or row_text(rows[0], 2) != str(model)
                or selected_after_delete != rows[0][0]
            ):
                raise RuntimeError(
                    "deleting the selected raw draft did not preserve its peer"
                )
            for source, digest in source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        f"raw draft lifecycle changed external source {source}"
                    )

            legacy = root / "legacy"
            legacy.mkdir()
            legacy_prefs = legacy / "prefs"
            legacy_prefs.mkdir()
            (legacy_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            run(
                binary, legacy,
                isolated_environment(
                    legacy, model, legacy / "raw-intake-legacy.bmp",
                    compact=False, drop=False
                ),
                ("raw-intake resumed=1 inspected=0 mappings=0 drafts=1",),
            )
            _, legacy_rows = raw_inventory(legacy)
            migrated_preferences = (
                legacy / "prefs" / "mdkr64_app.ini"
            ).read_text(encoding="utf-8")
            if (
                len(legacy_rows) != 1
                or row_text(legacy_rows[0], 2) != str(model)
                or "character_raw_intake_" in migrated_preferences
            ):
                raise RuntimeError(
                    "legacy singleton draft did not migrate transactionally"
                )
            (legacy / "prefs" / "mdkr64_app.ini").write_text(
                migrated_preferences + "character_raw_editor_open=0\n",
                encoding="utf-8",
            )
            (legacy / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            closed_a11y_environment = isolated_environment(
                legacy, model, legacy / "raw-intake-closed-a11y.bmp",
                compact=False, drop=False
            )
            closed_a11y_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "220",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, legacy, closed_a11y_environment,
                ("raw-draft-library open=0 drafts=1",
                 "text=Resume raw authoring draft"),
            )
            legacy_store = (
                legacy / "saves" / "character_raw_drafts-v1.tsv"
            )
            corrupt_bytes = bytearray(legacy_store.read_bytes())
            checksum_byte = len(corrupt_bytes) - 2
            corrupt_bytes[checksum_byte] = (
                ord("0") if corrupt_bytes[checksum_byte] != ord("0")
                else ord("1")
            )
            legacy_store.write_bytes(corrupt_bytes)
            corrupt_before = legacy_store.read_bytes()
            corrupt_shot = legacy / "raw-intake-corrupt.bmp"
            run(
                binary, legacy,
                isolated_environment(
                    legacy, model, corrupt_shot, compact=False, drop=False
                ),
                ("raw-draft-store writable=0 error=",),
            )
            check_bmp(corrupt_shot, 1280, 720)
            if legacy_store.read_bytes() != corrupt_before:
                raise RuntimeError(
                    "malformed raw draft inventory was partially loaded or overwritten"
                )

            invalid_spdx = root / "invalid-spdx"
            invalid_spdx.mkdir()
            invalid_spdx_prefs = invalid_spdx / "prefs"
            invalid_spdx_prefs.mkdir()
            invalid_spdx_license = invalid_spdx / "LICENSE.txt"
            invalid_spdx_license.write_text(
                "Fixture notice\n", encoding="utf-8"
            )
            (invalid_spdx_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                f"character_raw_intake_license={invalid_spdx_license}\n"
                "character_raw_intake_id=org.example.invalid-spdx\n"
                "character_raw_intake_display_name=Invalid SPDX Proof\n"
                "character_raw_intake_spdx=MIT Or Apache-2.0\n"
                "character_raw_intake_attribution=Generated test fixture\n"
                "character_raw_intake_source_url=https://example.invalid/spdx\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            invalid_spdx_environment = isolated_environment(
                invalid_spdx, model, invalid_spdx / "invalid-spdx.bmp",
                compact=False, drop=True,
            )
            invalid_spdx_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "16",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "build-reviewed-install",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            invalid_spdx_log = run(
                binary, invalid_spdx, invalid_spdx_environment,
                ("raw-spdx valid=0 build-ready=0 "
                 "error=Unexpected SPDX token 'Or' at byte 4.",),
            )
            if "raw-draft-action action=build-reviewed-install applied=1" in invalid_spdx_log:
                raise RuntimeError(
                    "invalid SPDX expression reached source-package build"
                )
            if (
                invalid_spdx / "characters" /
                ".launcher-character-raw-candidate.mdkrchar"
            ).exists():
                raise RuntimeError(
                    "invalid SPDX expression produced a review candidate"
                )

            install = root / "install"
            install.mkdir()
            install_prefs = install / "prefs"
            install_prefs.mkdir()
            license_file = install / "LICENSE.txt"
            license_file.write_text(
                "CC0 1.0 Universal test fixture\n", encoding="utf-8"
            )
            install_source_before = {
                model: hashlib.sha256(model.read_bytes()).hexdigest(),
                license_file: hashlib.sha256(
                    license_file.read_bytes()
                ).hexdigest(),
            }
            (install_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                f"character_raw_intake_license={license_file}\n"
                "character_raw_intake_id=org.example.raw-install\n"
                "character_raw_intake_display_name=Raw Install Proof\n"
                "character_raw_intake_spdx=CC0-1.0\n"
                "character_raw_intake_attribution=Generated test fixture\n"
                "character_raw_intake_source_url=https://example.invalid/raw-install\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            install_environment = isolated_environment(
                install, model, install / "raw-intake-install.bmp",
                compact=False, drop=True
            )
            install_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "16",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "build-reviewed-install",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, install, install_environment,
                ("raw-draft-action action=build-reviewed-install applied=1",
                 "raw-reviewed-install reviewed=1 installed=1",
                 "remaining=0"),
            )
            selected_after_install, install_rows = raw_inventory(install)
            if selected_after_install != "-" or install_rows:
                raise RuntimeError(
                    "successful reviewed install did not remove its exact "
                    "completed raw draft"
                )
            if not (
                install / "characters" / "org.example.raw-install.mdkc"
            ).is_file():
                raise RuntimeError(
                    "reviewed raw-source install did not publish its cache"
                )
            if (
                install / "characters" /
                ".launcher-character-raw-candidate.mdkrchar"
            ).exists():
                raise RuntimeError(
                    "reviewed raw-source install retained its disposable candidate"
                )
            for source, digest in install_source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        f"reviewed install changed external source {source}"
                    )

            accessible = root / "accessible"
            accessible.mkdir()
            (accessible / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            accessible_shot = accessible / "raw-intake-a11y.bmp"
            accessible_environment = isolated_environment(
                accessible, model, accessible_shot, compact=False, drop=True
            )
            accessible_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "220",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, accessible, accessible_environment,
                ("text=Package ID", "text=SPDX license expression",
                 "text=Raw authoring draft",
                 "text=Duplicate as a new raw draft",
                 "text=Close raw editor",
                 "text=Gameplay donor",
                 "text=Standing height in metres",
                 "text=Delete raw authoring draft"),
            )

            compact = root / "compact"
            compact.mkdir()
            compact_prefs = compact / "prefs"
            compact_prefs.mkdir()
            (compact_prefs / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            compact_shot = compact / "raw-intake-compact.bmp"
            run(
                binary, compact,
                isolated_environment(
                    compact, model, compact_shot, compact=True, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=1",
                 "compact-layout dense=1 contained=1 overlap=0 "),
            )
            check_bmp(compact_shot, 640, 480)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_raw_intake_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_raw_intake_ui: PASS -- bounded DAE/ZIP conversion, "
          "ZIP-bomb and invalid-SPDX refusal, "
          "mutation-free FBX/OBJ/BLEND/glTF/USD/DCC export guidance, "
          "multi-draft GLB intake, "
          "same-source branching, source-bound mapping restore, exact "
          "switch/delete/install cleanup, "
          "legacy migration, corruption fail-closed behavior, source-byte "
          "purity, keyboard speech, and 200% compact rendering")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
