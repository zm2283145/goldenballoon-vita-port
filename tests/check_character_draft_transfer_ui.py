#!/usr/bin/env python3
"""Rendered, ROM-free proof of private and additive named-draft transfer."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
from character_validation_fixture import accepted_character_validation  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_humanoid_glb,
    make_portrait_png,
)

PACKAGE_ID = "org.mdkr.draft-transfer-proof"
TOKEN = "mdkr64-app-draft-transfer-v1"


def install_fixture(root: Path, *, with_lod: bool) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir(parents=True)
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "transfer-proof.mdkrchar"
    model.write_bytes(make_humanoid_glb(with_lod=with_lod))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model, PACKAGE_ID, "Draft Transfer Proof", "CC0-1.0",
        "Generated MDKR fixture",
        "https://example.invalid/draft-transfer-proof",
        "diddy", ["car", "hovercraft", "plane"], portrait=portrait,
        minimap_rgb=[100, 180, 240], rig_mode="humanoid-retarget-v1",
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    probe.build_package(
        model, manifest_path, license_path, package,
        portrait_path=portrait,
    )
    with accepted_character_validation(manager):
        manager.install(package, characters)
    return characters


def inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def check_bmp(path: Path, width: int, height: int) -> None:
    data = path.read_bytes()
    captured_width = struct.unpack_from("<i", data, 18)[0] if len(data) >= 54 else 0
    captured_height = abs(
        struct.unpack_from("<i", data, 22)[0]
    ) if len(data) >= 54 else 0
    scale = captured_width // width if width != 0 else 0
    if (
        len(data) < 54 or data[:2] != b"BM"
        or scale < 1 or scale > 3
        or captured_width != width * scale
        or captured_height != height * scale
    ):
        raise RuntimeError(f"invalid {width}x{height} evidence capture {path}")


def run_app(binary: Path, root: Path, characters: Path, bundle: Path,
            action: str, expected: tuple[str, ...], *, compact: bool = False,
            accessible: bool = False, shot: Path | None = None) -> str:
    prefs = root / "prefs"
    saves = root / "saves"
    prefs.mkdir(parents=True, exist_ok=True)
    saves.mkdir(parents=True, exist_ok=True)
    (prefs / "mdkr64_app.ini").write_text(
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=package\n" +
        ("ui_scale=2.00\n" if compact else ""),
        encoding="utf-8",
    )
    video = root / "video.ini"
    if accessible:
        video.write_text("[Accessibility]\nSpeech=1\n", encoding="utf-8")
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        "LC_ALL": "C",
        "MDKR_APP_SMOKE_FRAMES": "520" if accessible else "320",
        "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480" if compact else "1280x720",
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(video),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_APP_SMOKE_DRAFT_TRANSFER_ACTION": action,
        "MDKR_APP_SMOKE_DRAFT_TRANSFER_TOKEN": TOKEN,
        "MDKR_APP_SMOKE_DRAFT_TRANSFER_PATH": str(bundle),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if shot is not None:
        environment["MDKR_APP_SMOKE_SHOT"] = str(shot)
    if accessible:
        environment.update({
            "MDKR_APP_SMOKE_A11Y_WALK": "1",
            "MDKR_APP_SMOKE_INPUT": "keyboard",
            "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
            "MDKR_A11Y_TRACE": "1",
        })
    process = subprocess.run(
        [str(binary)], cwd=characters.parent, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{action} exited {process.returncode}\n{process.stdout[-12000:]}"
        )
    for marker in expected:
        if marker not in process.stdout:
            raise RuntimeError(
                f"{action} omitted {marker!r}\n{process.stdout[-12000:]}"
            )
    return process.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    try:
        with tempfile.TemporaryDirectory(
                prefix="mdkr-draft-transfer-ui-") as temporary:
            root = Path(temporary)
            author_fixture = root / "author-fixture"
            author_fixture.mkdir()
            author_characters = install_fixture(
                author_fixture, with_lod=True
            )
            installed_before = inventory(author_characters)
            bundle = root / "shared-drafts.mdkrdrafts"
            author = root / "author"
            author.mkdir()
            run_app(
                binary, author, author_characters, bundle, "seed-export",
                (
                    "character-draft-transfer-action action=seed-export "
                    "applied=1 error=none",
                ),
            )
            if not bundle.is_file():
                raise RuntimeError("draft export did not create its bundle")
            bundle_before = hashlib.sha256(bundle.read_bytes()).hexdigest()
            bundle_bytes = bundle.read_bytes()
            if (
                not bundle_bytes.startswith(
                    b"mdkr-character-draft-bundle-v1\t"
                )
                or str(root).encode("utf-8") in bundle_bytes
                or str(root).encode("utf-8").hex().encode("ascii")
                    in bundle_bytes
            ):
                raise RuntimeError(
                    "draft export leaked a local path or missed its format"
                )
            overwrite = root / "overwrite-attempt"
            overwrite.mkdir()
            run_app(
                binary, overwrite, author_characters, bundle, "seed-export",
                (
                    "character-draft-transfer-action action=seed-export "
                    "applied=0",
                ),
            )
            if hashlib.sha256(bundle.read_bytes()).hexdigest() != bundle_before:
                raise RuntimeError("repeat export replaced an existing bundle")

            recipient_runtime = root / "recipient-runtime"
            recipient_runtime.mkdir()
            recipient_characters = recipient_runtime / "characters"
            shutil.copytree(author_characters, recipient_characters)
            recipient_before = inventory(recipient_characters)
            recipient = root / "recipient"
            recipient.mkdir()
            shot = recipient / "draft-review.bmp"
            run_app(
                binary, recipient, recipient_characters, bundle,
                "review-import",
                (
                    "character-draft-transfer-action action=review-import "
                    "applied=1 error=none",
                    "character-draft-transfer-review "
                    f"package={PACKAGE_ID} compatible=1 drafts=1 "
                    "additions=1 duplicates=0 renamed=0 paths=0 model=0 "
                    "rom=0 mutation=0",
                    "text=Draft bundle compatibility summary. "
                    "Integrity-checked and schema-validated draft bundle. "
                    "It matches the selected "
                    "character and exact source revision.",
                    "compact-layout dense=1 contained=1 overlap=0 ",
                ),
                compact=True, accessible=True, shot=shot,
            )
            check_bmp(shot, 640, 480)
            if (recipient / "saves" /
                    "character_workshop_drafts-v1.tsv").exists():
                raise RuntimeError("review imported draft bytes before consent")
            if inventory(recipient_characters) != recipient_before:
                raise RuntimeError("review changed installed character bytes")

            # Reuse the recipient save directory so the second process proves
            # a real review-to-durable-import lifecycle.
            run_app(
                binary, recipient, recipient_characters, bundle,
                "confirm-import",
                (
                    "character-draft-transfer-action action=confirm-import "
                    "applied=1 error=none",
                    "character-draft-transfer-review "
                    f"package={PACKAGE_ID} compatible=1 drafts=1 "
                    "additions=0 duplicates=1 renamed=0 paths=0 model=0 "
                    "rom=0 mutation=0",
                ),
            )
            draft_inventory = (
                recipient / "saves" / "character_workshop_drafts-v1.tsv"
            )
            if not draft_inventory.is_file():
                raise RuntimeError("confirmed import did not persist drafts")
            durable_before = hashlib.sha256(
                draft_inventory.read_bytes()
            ).hexdigest()
            run_app(
                binary, recipient, recipient_characters, bundle,
                "review-import",
                (
                    "character-draft-transfer-review "
                    f"package={PACKAGE_ID} compatible=1 drafts=1 "
                    "additions=0 duplicates=1 renamed=0 paths=0 model=0 "
                    "rom=0 mutation=0",
                ),
            )
            if hashlib.sha256(
                    draft_inventory.read_bytes()).hexdigest() != durable_before:
                raise RuntimeError("duplicate review rewrote local drafts")

            incompatible_fixture = root / "incompatible-fixture"
            incompatible_fixture.mkdir()
            incompatible_characters = install_fixture(
                incompatible_fixture, with_lod=False
            )
            incompatible_before = inventory(incompatible_characters)
            incompatible = root / "incompatible"
            incompatible.mkdir()
            run_app(
                binary, incompatible, incompatible_characters, bundle,
                "review-import",
                (
                    "character-draft-transfer-action action=review-import "
                    "applied=1 error=none",
                    "character-draft-transfer-review "
                    f"package={PACKAGE_ID} compatible=0 drafts=1 "
                    "additions=0 duplicates=0 renamed=0 paths=0 model=0 "
                    "rom=0 mutation=0",
                ),
            )
            if (
                inventory(incompatible_characters) != incompatible_before
                or (incompatible / "saves" /
                    "character_workshop_drafts-v1.tsv").exists()
            ):
                raise RuntimeError(
                    "incompatible review changed package or draft state"
                )
            if inventory(author_characters) != installed_before:
                raise RuntimeError("transfer lifecycle changed author package")
    except (OSError, RuntimeError, subprocess.SubprocessError,
            probe.ProbeError, manager.ManagerError) as error:
        print(f"check_character_draft_transfer_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print(
        "check_character_draft_transfer_ui: PASS -- exclusive path-private "
        "exact-source export, mutation-free responsive accessible review, "
        "additive atomic import, duplicate idempotence, and incompatible-source "
        "refusal"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
