#!/usr/bin/env python3
"""ROM-free rendered proof for resumable raw-GLB Workshop intake."""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

from test_character_asset_probe import make_animated_glb  # noqa: E402


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

            wide = root / "wide"
            wide.mkdir()
            wide_shot = wide / "raw-intake-wide.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, wide_shot, compact=False, drop=True
                ),
                ("active-panel=Character Workshop",
                 "raw-intake resumed=1 inspected=1 mappings=1"),
            )
            check_bmp(wide_shot, 1280, 720)
            preferences = (wide / "prefs" / "mdkr64_app.ini").read_text(
                encoding="utf-8"
            )
            for exact in (
                f"character_raw_intake_model={model}",
                "character_raw_intake_mapping_sha256="
                + hashlib.sha256(model.read_bytes()).hexdigest(),
                "character_raw_intake_fallback=idle",
                "character_raw_intake_seat=root",
                "character_raw_intake_head=head",
            ):
                if exact not in preferences:
                    raise RuntimeError(
                        f"raw intake preference was not persisted: {exact}"
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
                 "text=Gameplay donor",
                 "text=Standing height in metres"),
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
                ("raw-intake resumed=1 inspected=1 mappings=1",
                 "compact-layout dense=1 contained=1 overlap=0 "),
            )
            check_bmp(compact_shot, 640, 480)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_raw_intake_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_raw_intake_ui: PASS -- GLB drop, bounded inspection, "
          "mapping persistence, restart resume, keyboard speech, and 200% "
          "compact rendering")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
