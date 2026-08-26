#!/usr/bin/env python3
"""ROM-free rendered and accessibility proof for Portrait Studio tools."""

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

from check_character_workshop_history_ui import install_fixture  # noqa: E402

PACKAGE_ID = "org.mdkr.history-proof"


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def environment(root: Path, characters: Path, shot: Path, *,
                compact: bool, accessible: bool) -> dict[str, str]:
    prefs = root / "prefs"
    saves = root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=identity\n"
        + ("ui_scale=2.00\n" if compact else ""),
        encoding="utf-8",
    )
    video = root / "video.ini"
    if accessible:
        video.write_text("[Accessibility]\nSpeech=1\n", encoding="utf-8")
    result = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    result.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "520" if accessible else "16",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720" if accessible
        else "640x480",
        "MDKR_APP_SMOKE_SHOT": str(shot),
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(video),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tools" / "character_package_manager.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if compact:
        result.update({
            "MDKR_APP_SMOKE_TOUCH_SCROLL": "1",
            "MDKR_APP_SMOKE_TOUCH_TOKEN": "mdkr64-app-touch-v1",
        })
    if accessible:
        result.update({
            "MDKR_APP_SMOKE_A11Y_WALK": "1",
            "MDKR_APP_SMOKE_INPUT": "keyboard",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_A11Y_TRACE": "1",
        })
    return result


def run(binary: Path, root: Path, env: dict[str, str],
        markers: tuple[str, ...]) -> str:
    completed = subprocess.run(
        [str(binary)], cwd=root, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"Portrait Studio exited {completed.returncode}\n"
            f"{completed.stdout[-12000:]}"
        )
    for marker in markers:
        if marker not in completed.stdout:
            raise RuntimeError(
                f"Portrait Studio output omitted {marker!r}\n"
                f"{completed.stdout[-12000:]}"
            )
    return completed.stdout


def check_bmp(path: Path, minimum_width: int, minimum_height: int) -> None:
    payload = path.read_bytes()
    if len(payload) < 54 or payload[:2] != b"BM":
        raise RuntimeError("Portrait Studio did not produce a BMP capture")
    width, height = struct.unpack_from("<ii", payload, 18)
    if width < minimum_width or abs(height) < minimum_height:
        raise RuntimeError(
            f"Portrait Studio capture is undersized: {width}x{height}"
        )
    pixel_offset = struct.unpack_from("<I", payload, 10)[0]
    if len(set(payload[pixel_offset:])) < 16:
        raise RuntimeError("Portrait Studio capture is visually empty")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr-portrait-studio-ui-") as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            before = inventory(characters)

            compact = root / "compact"
            compact.mkdir()
            compact_shot = compact / "portrait-studio-compact.bmp"
            run(
                binary, root,
                environment(compact, characters, compact_shot,
                            compact=True, accessible=False),
                ("active-panel=Character Workshop",
                 "compact-layout dense=1 contained=1 "),
            )
            check_bmp(compact_shot, 640, 480)

            accessible = root / "accessible"
            accessible.mkdir()
            accessible_shot = accessible / "portrait-studio-a11y.bmp"
            run(
                binary, root,
                environment(accessible, characters, accessible_shot,
                            compact=False, accessible=True),
                ("character-portrait-style package=" + PACKAGE_ID,
                 "palette=32",
                 "text=Framing zoom",
                 "text=Palette target",
                 "text=Apply styled result to pixel canvas",
                 "text=Selection x, y, width, height",
                 "text=Replace matching colours with paint colour"),
            )
            check_bmp(accessible_shot, 1280, 720)
            if inventory(characters) != before:
                raise RuntimeError(
                    "rendering or keyboard-walking Portrait Studio mutated "
                    "installed package bytes"
                )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_portrait_studio_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_portrait_studio_ui: PASS -- deterministic style "
          "lab, advanced pixel tools, 200% compact rendering, keyboard speech, "
          "and installed-byte purity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
