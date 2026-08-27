#!/usr/bin/env python3
"""Rendered lifecycle proof for durable exact-test evidence and baselines."""

from __future__ import annotations

import argparse
import hashlib
import json
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

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    make_portrait_png,
)


PACKAGE_ID = "org.mdkr.test-evidence-proof"
TOKEN = "mdkr64-character-test-evidence-v1"


def install_fixture(root: Path) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    portrait = source / "portrait.png"
    manifest_path = source / "manifest.json"
    license_path = source / "LICENSE.txt"
    package = source / "test-evidence-proof.mdkrchar"
    model.write_bytes(make_animated_glb(with_lod=True))
    portrait.write_bytes(make_portrait_png(40))
    manifest, _ = wizard.build_manifest(
        model,
        PACKAGE_ID,
        "Test Evidence Proof",
        "CC0-1.0",
        "Generated MDKR fixture",
        "https://example.invalid/test-evidence-proof",
        "bumper",
        ["car", "hovercraft", "plane"],
        portrait=portrait,
        minimap_rgb=[80, 210, 150],
    )
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    probe.build_package(
        model, manifest_path, license_path, package, portrait_path=portrait
    )
    manager.install(package, characters)
    return characters


def file_inventory(directory: Path) -> dict[str, str]:
    return {
        str(path.relative_to(directory)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def evidence_rows(root: Path) -> list[list[str]]:
    path = root / "saves" / "character_test_evidence-v1.tsv"
    lines = path.read_text(encoding="ascii").splitlines()
    header = lines[0].split("\t")
    if (
        len(header) != 3
        or header[0] != "mdkr-character-test-evidence-v3"
        or int(header[1]) != len(lines) - 1
        or len(header[2]) != 64
    ):
        raise RuntimeError("test evidence inventory header is malformed")
    rows = [line.split("\t") for line in lines[1:]]
    if any(len(row) != 105 for row in rows):
        raise RuntimeError("test evidence inventory row is malformed")
    return rows


def check_bmp(path: Path, width: int, height: int) -> None:
    payload = path.read_bytes()
    if len(payload) < 54 or payload[:2] != b"BM":
        raise RuntimeError("test evidence compact capture is not a BMP")
    actual_width, actual_height = struct.unpack_from("<ii", payload, 18)
    if actual_width < width or abs(actual_height) < height:
        raise RuntimeError(
            f"test evidence capture is too small: "
            f"{actual_width}x{actual_height}"
        )


def run(
    binary: Path,
    root: Path,
    characters: Path,
    expected: tuple[str, ...],
    *,
    action: str | None = None,
    compact: bool = False,
    accessible: bool = False,
    inspection_capture: Path | None = None,
    visual_report: Path | None = None,
) -> str:
    prefs = root / "prefs"
    saves = root / "saves"
    prefs.mkdir(exist_ok=True)
    saves.mkdir(exist_ok=True)
    preferences = (
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=test\n"
    )
    if compact:
        preferences += "ui_scale=2.0\n"
    (prefs / "mdkr64_app.ini").write_text(preferences, encoding="utf-8")
    if accessible:
        (root / "video.ini").write_text(
            "[Accessibility]\nSpeech=1\n", encoding="utf-8"
        )
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    shot = root / ("evidence-compact.bmp" if compact else "evidence.bmp")
    environment.update(
        {
            "LC_ALL": "C",
            "MDKR_APP_SMOKE_FRAMES": "520" if accessible else "12",
            "MDKR_APP_SMOKE_WINDOW_SIZE": (
                "640x480" if compact else "1280x720"
            ),
            "MDKR_APP_SMOKE_SHOT": str(shot),
            "MDKR_APP_PANEL": "Character Workshop",
            "MDKR_APP_UI_TRACE": "1",
            "MDKR_APP_PREFS_DIR": str(prefs),
            "MDKR_VIDEO_CONFIG_PATH": str(root / "video.ini"),
            "MDKR_SAVE_DIR": str(saves),
            "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
            "MDKR_CHARACTER_MANAGER": str(
                ROOT / "tools" / "character_package_manager.py"
            ),
            "MDKR_NO_CRASH_HANDLER": "1",
            "MDKR64_HIDDEN": "1",
            "MDKR_AUDIO": "0",
        }
    )
    if action is not None:
        environment.update(
            {
                "MDKR_APP_SMOKE_CHARACTER_TEST_EVIDENCE_ACTION": action,
                "MDKR_APP_SMOKE_CHARACTER_TEST_EVIDENCE_TOKEN": TOKEN,
            }
        )
    if inspection_capture is not None:
        environment["MDKR_APP_SMOKE_CHARACTER_INSPECTION_CAPTURE"] = str(
            inspection_capture
        )
    if visual_report is not None:
        environment["MDKR_APP_SMOKE_CHARACTER_VISUAL_REPORT"] = str(
            visual_report
        )
    if compact and not accessible:
        environment.update(
            {
                "MDKR_APP_SMOKE_TOUCH_SCROLL": "1",
                "MDKR_APP_SMOKE_TOUCH_TOKEN": "mdkr64-app-touch-v1",
            }
        )
    if accessible:
        environment.update(
            {
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            }
        )
    completed = subprocess.run(
        [str(binary)],
        cwd=root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=240,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"test evidence app exited {completed.returncode}\n"
            f"{completed.stdout[-12000:]}"
        )
    for marker in expected:
        if marker not in completed.stdout:
            raise RuntimeError(
                f"missing test evidence marker {marker!r}\n"
                f"{completed.stdout[-12000:]}"
            )
    if compact:
        check_bmp(shot, 640, 480)
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument(
        "--rom", type=Path, help="accepted for run_checks.py compatibility"
    )
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    try:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-test-evidence-"
        ) as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            installed_before = file_inventory(characters)

            run(
                binary,
                root,
                characters,
                (
                    "character-test-evidence-action "
                    "action=publish-qualified applied=1 records=1 baselines=0",
                    "character-test-evidence saved=1",
                    "character-test-matrix package=" + PACKAGE_ID,
                    "current=1 required=16 selected=2:4 state=Qualified "
                    "latest=1 baseline=0",
                    "character-contact-proof context=2 mask=f views=3 "
                    "coordinate-space=donor-target "
                    "errorsUm=1000,1500,2000,2500 accessible=numeric-table",
                ),
                action="publish-qualified",
            )
            rows = evidence_rows(root)
            if (
                len(rows) != 1
                or rows[0][0] != "0"
                or rows[0][1] != PACKAGE_ID
                or (rows[0][7], rows[0][8]) != ("2", "4")
                or rows[0][9] != "10"
                or bytes.fromhex(rows[0][29]).decode("utf-8")
                != "webgpu-test"
                or bytes.fromhex(rows[0][30]).decode("utf-8")
                != "Rendered evidence fixture GPU"
                or tuple(map(int, rows[0][34:38]))
                != (1280, 960, 2560, 1920)
                or rows[0][38] != "1"
                or tuple(map(int, rows[0][39:42]))
                != (-400000, -600000, -300000)
                or tuple(map(int, rows[0][42:45]))
                != (400000, 900000, 300000)
                or tuple(map(int, rows[0][45:48]))
                != (10000, 20000, -30000)
                or tuple(map(int, rows[0][48:51])) != (0, 0, 1000)
                or rows[0][51] != "15"
                or tuple(map(int, rows[0][100:104]))
                != (1000, 1500, 2000, 2500)
            ):
                raise RuntimeError(
                    "qualified exact result did not persist exact device and renderer-fit fields"
                )

            run(
                binary,
                root,
                characters,
                (
                    "action=pin-car-4p applied=1 records=2 baselines=1",
                    "state=Qualified latest=1 baseline=1 comparable=1",
                    "comparable=1 fit=1 "
                    "fitAnchorUm=10000,20000,-30000 "
                    "fitBoundsYUm=-600000,900000 "
                    "fitForwardMilli=0,0,1000",
                ),
                action="pin-car-4p",
            )
            rows = evidence_rows(root)
            if len(rows) != 2 or {row[0] for row in rows} != {"0", "1"}:
                raise RuntimeError(
                    "pinning did not preserve latest evidence beside one baseline"
                )

            run(
                binary,
                root,
                characters,
                (
                    "state=Qualified latest=1 baseline=1 comparable=1",
                    "text=Car, 4 players, Qualified",
                    "text=Pin as comparison baseline",
                    "text=Clear pinned baseline",
                    "text=Semantic pose",
                    "text=Normalized phase",
                    "text=Vehicle camera yaw",
                    "text=Vehicle camera pitch",
                    "text=Gameplay view",
                    "text=Front view",
                    "text=Left view",
                    "text=Right view",
                    "text=Character lighting",
                    "text=Save stabilized PNG during next inspection",
                    "text=Model only · transparent",
                    "text=Capture PNG path",
                    "character-capture-thumbnail package=" + PACKAGE_ID
                    + " product=model-alpha source=40x40 preview=40x40",
                    "text=Captured renderer preview",
                    "text=Use capture for portrait",
                    "text=Remove from report",
                    "text=Visual report path",
                    "text=Export self-contained report",
                    "text=Clear capture list",
                    "text=Inspect character select",
                    "text=Inspect car",
                    "text=Inspect hovercraft",
                    "text=Inspect plane",
                ),
                compact=True,
                accessible=True,
                action="publish-inspection-capture",
                inspection_capture=root / "source" / "portrait.png",
                visual_report=root / "visual-report.html",
            )

            run(
                binary,
                root,
                characters,
                (
                    "action=clear-car-4p-baseline applied=1 "
                    "records=1 baselines=0",
                    "state=Qualified latest=1 baseline=0 comparable=0",
                ),
                action="clear-car-4p-baseline",
            )
            rows = evidence_rows(root)
            if len(rows) != 1 or rows[0][0] != "0":
                raise RuntimeError(
                    "clearing the baseline did not preserve the latest result"
                )

            run(
                binary,
                root,
                characters,
                (
                    "action=clear-package applied=1 records=0 baselines=0",
                    "current=0 required=16",
                ),
                action="clear-package",
            )
            if evidence_rows(root):
                raise RuntimeError("package evidence cleanup retained a record")

            empty_evidence = (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes()
            run(
                binary,
                root,
                characters,
                (
                    "character-pose-inspection session-only=1",
                    "action=publish-inspection applied=1 "
                    "records=0 baselines=0",
                    "current=0 required=16",
                ),
                action="publish-inspection",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "session-only pose inspection mutated durable evidence"
                )
            run(
                binary,
                root,
                characters,
                (
                    "character-pose-inspection session-only=1",
                    "action=publish-inspection-fallback applied=1 "
                    "records=0 baselines=0",
                ),
                action="publish-inspection-fallback",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "fallback pose inspection mutated durable evidence"
                )
            run(
                binary,
                root,
                characters,
                (
                    "character-preview-result rejected-evidence=mixed-mode",
                    "action=publish-mixed-mode applied=1 "
                    "records=0 baselines=0",
                ),
                action="publish-mixed-mode",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "mixed-mode result contaminated durable performance evidence"
                )
            run(
                binary,
                root,
                characters,
                (
                    "character-preview-result rejected-evidence=fit-contact-contract",
                    "action=publish-invalid-fit applied=1 "
                    "records=0 baselines=0",
                ),
                action="publish-invalid-fit",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "invalid fit contract contaminated durable performance evidence"
                )
            run(
                binary,
                root,
                characters,
                (
                    "character-preview-result rejected-evidence=fit-contact-contract",
                    "contactMask=3",
                    "action=publish-invalid-contact applied=1 "
                    "records=0 baselines=0",
                ),
                action="publish-invalid-contact",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "invalid contact contract contaminated durable performance evidence"
                )

            run(
                binary,
                root,
                characters,
                (
                    "action=publish-stale-fit-session applied=1 "
                    "records=1 baselines=0",
                    "state=Stale latest=1 baseline=0 comparable=0",
                ),
                action="publish-stale-fit-session",
            )
            run(
                binary,
                root,
                characters,
                (
                    "action=publish-stale-lod-session applied=1 "
                    "records=1 baselines=0",
                    "state=Stale latest=1 baseline=0 comparable=0",
                ),
                action="publish-stale-lod-session",
            )
            run(
                binary,
                root,
                characters,
                (
                    "action=clear-package applied=1 records=0 baselines=0",
                ),
                action="clear-package",
            )
            if evidence_rows(root):
                raise RuntimeError(
                    "stale-session regression cleanup retained a record"
                )

            evidence_path = (
                root / "saves" / "character_test_evidence-v1.tsv"
            )
            corrupt = bytearray(evidence_path.read_bytes())
            corrupt[len(corrupt) - 2] = (
                ord("0") if corrupt[len(corrupt) - 2] != ord("0") else ord("1")
            )
            evidence_path.write_bytes(corrupt)
            corrupt_before = evidence_path.read_bytes()
            run(
                binary,
                root,
                characters,
                ("character-test-evidence writable=0 error=",),
            )
            if evidence_path.read_bytes() != corrupt_before:
                raise RuntimeError(
                    "corrupt test evidence was partially loaded or overwritten"
                )
            if file_inventory(characters) != installed_before:
                raise RuntimeError(
                    "test evidence lifecycle mutated installed character bytes"
                )
    except (
        OSError,
        RuntimeError,
        subprocess.SubprocessError,
        probe.ProbeError,
        manager.ManagerError,
    ) as error:
        print(
            f"check_character_test_evidence_ui: FAIL -- {error}",
            file=sys.stderr,
        )
        return 1
    print(
        "check_character_test_evidence_ui: PASS -- durable source/fit/device-"
        "bound 4x4 matrix with signed renderer-fit and hand/foot contact diagnostics, same-"
        "environment baseline lifecycle, corruption "
        "and invalid-fit/contact refusal, pose-inspection exclusion, keyboard speech, "
        "200% rendering, and package-byte purity"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
