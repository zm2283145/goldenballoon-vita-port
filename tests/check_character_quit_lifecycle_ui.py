#!/usr/bin/env python3
"""Rendered proof that quit remains visible and safe during character work."""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from test_character_asset_probe import make_humanoid_glb  # noqa: E402


MARKERS = (
    "[app-ui] character-workshop-quit request=deferred",
    "[app-ui] character-workshop-quit progress-visible=1 cancel-visible=1",
    "[app-ui] character-workshop-lifecycle primary-gated=1 play-gated=1 "
    "import-gated=1",
    "[app-ui] character-workshop-quit completed=1 pending=0",
    "[app-ui-test] character quit lifecycle requested=1 published=1 "
    "pending-at-publication=0 settled=1",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    # The unified native runner supplies its ordinary ROM argument to every
    # native-shaped check. This lifecycle is deliberately ROM-free.
    parser.add_argument("--rom", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()

    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr-character-quit-lifecycle-") as temporary:
            root = Path(temporary)
            source = root / "slow-review.glb"
            source.write_bytes(make_humanoid_glb(with_lod=True))
            source_before = digest(source)
            prefs = root / "prefs"
            saves = root / "saves"
            characters = root / "characters"
            prefs.mkdir()
            saves.mkdir()
            characters.mkdir()
            environment = {
                key: value for key, value in os.environ.items()
                if not key.startswith(("MDKR", "GE007_"))
            }
            environment.update({
                "LC_ALL": "C",
                "MDKR_APP_SMOKE_FRAMES": "12",
                "MDKR_APP_SMOKE_WINDOW_SIZE": "1280x720",
                "MDKR_APP_PANEL": "Character Workshop",
                "MDKR_APP_UI_TRACE": "1",
                "MDKR_APP_SMOKE_DROP": str(source),
                "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS": "1",
                "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS_TOKEN":
                    "mdkr64-character-jobs-v1",
                "MDKR_APP_SMOKE_QUIT_DURING_CHARACTER_WORK": "1",
                "MDKR_APP_SMOKE_QUIT_DURING_CHARACTER_WORK_TOKEN":
                    "mdkr64-character-quit-v1",
                "MDKR_CHARACTER_MANAGER_FIXTURE_DELAY_MS": "250",
                "MDKR_CHARACTER_MANAGER": str(
                    ROOT / "tests" / "run_character_manager_fixture.py"
                ),
                "MDKR_APP_PREFS_DIR": str(prefs),
                "MDKR_VIDEO_CONFIG_PATH": str(root / "video.ini"),
                "MDKR_SAVE_DIR": str(saves),
                "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
                "MDKR_NO_CRASH_HANDLER": "1",
                "MDKR64_HIDDEN": "1",
                "MDKR_AUDIO": "0",
            })
            completed = subprocess.run(
                [str(binary)], cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=60, check=False,
            )
            if completed.returncode != 0:
                raise RuntimeError(
                    f"launcher exited {completed.returncode}\n"
                    f"{completed.stdout[-12000:]}"
                )
            for marker in MARKERS:
                if marker not in completed.stdout:
                    raise RuntimeError(
                        f"missing lifecycle marker {marker!r}\n"
                        f"{completed.stdout[-12000:]}"
                    )
            if digest(source) != source_before:
                raise RuntimeError("quit lifecycle changed the external GLB")
            leftovers = [
                path.name for path in characters.iterdir()
                if path.name.startswith(".launcher-character-")
            ]
            if leftovers:
                raise RuntimeError(
                    "quit lifecycle stranded launcher result files: "
                    + ", ".join(sorted(leftovers))
                )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_quit_lifecycle_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1

    print(
        "check_character_quit_lifecycle_ui: PASS -- close request stayed "
        "visible and cancellable, Play/import stayed gated, the background "
        "review published on the UI thread, and Quit appeared only after "
        "settlement without changing the external source"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
