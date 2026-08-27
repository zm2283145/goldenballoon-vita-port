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
from character_validation_fixture import accepted_character_validation  # noqa: E402


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
    with accepted_character_validation(manager):
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
        or header[0] != "mdkr-character-test-evidence-v4"
        or int(header[1]) != len(lines) - 1
        or len(header[2]) != 64
    ):
        raise RuntimeError("test evidence inventory header is malformed")
    rows = [line.split("\t") for line in lines[1:]]
    if any(len(row) != 125 for row in rows):
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


def check_fit_overlay_bmp(path: Path) -> None:
    """Require the three independent renderer-fit visual encodings."""

    payload = path.read_bytes()
    if len(payload) < 54 or payload[:2] != b"BM":
        raise RuntimeError("fit-overlay evidence is not a BMP")
    pixel_offset = struct.unpack_from("<I", payload, 10)[0]
    width, signed_height = struct.unpack_from("<ii", payload, 18)
    bits_per_pixel = struct.unpack_from("<H", payload, 28)[0]
    compression = struct.unpack_from("<I", payload, 30)[0]
    height = abs(signed_height)
    if (
        width <= 0
        or height <= 0
        or bits_per_pixel not in (24, 32)
        or compression != 0
    ):
        raise RuntimeError(
            "fit-overlay BMP uses an unsupported pixel layout"
        )
    bytes_per_pixel = bits_per_pixel // 8
    row_bytes = ((width * bits_per_pixel + 31) // 32) * 4
    if pixel_offset + row_bytes * height > len(payload):
        raise RuntimeError("fit-overlay BMP pixel payload is truncated")
    blue: list[tuple[int, int]] = []
    pixels: list[tuple[int, int, int, int, int]] = []
    for row in range(height):
        base = pixel_offset + row * row_bytes
        for x in range(width):
            offset = base + x * bytes_per_pixel
            b, g, r = payload[offset:offset + 3]
            pixels.append((x, row, r, g, b))
            if 45 <= r <= 115 and 155 <= g <= 225 and b >= 215:
                blue.append((x, row))
    if len(blue) < 24:
        raise RuntimeError(
            f"fit overlay has no visible measured-forward vector ({len(blue)} blue pixels)"
        )
    minimum_x = max(0, min(x for x, _ in blue) - 140)
    maximum_x = min(width - 1, max(x for x, _ in blue) + 140)
    minimum_y = max(0, min(y for _, y in blue) - 140)
    maximum_y = min(height - 1, max(y for _, y in blue) + 140)
    nearby = [
        (r, g, b)
        for x, y, r, g, b in pixels
        if minimum_x <= x <= maximum_x and minimum_y <= y <= maximum_y
    ]
    gold = sum(r >= 210 and 155 <= g <= 220 and b <= 125
               for r, g, b in nearby)
    bounds = sum(135 <= r <= 190 and 155 <= g <= 210 and
                 175 <= b <= 235 for r, g, b in nearby)
    if gold < 24 or bounds < 24:
        raise RuntimeError(
            "fit overlay is missing its anchor/datum or calibrated-bounds "
            f"encoding near the forward vector (gold={gold}, bounds={bounds})"
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
    focus_fit_overlay: bool = False,
) -> str:
    prefs = root / "prefs"
    saves = root / "saves"
    prefs.mkdir(exist_ok=True)
    saves.mkdir(exist_ok=True)
    preferences_path = prefs / "mdkr64_app.ini"
    remembered_rom = ""
    if preferences_path.is_file():
        for line in preferences_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("rom_path=") and len(line) > len("rom_path="):
                remembered_rom = line + "\n"
                break
    preferences = remembered_rom + (
        f"character_workshop_last_selected={PACKAGE_ID}\n"
        "character_workshop_last_tab=test\n"
    )
    if compact:
        preferences += "ui_scale=2.0\n"
    preferences_path.write_text(preferences, encoding="utf-8")
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
            "MDKR_APP_SMOKE_FRAMES": "640" if accessible else "12",
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
                ROOT / "tests" / "run_character_manager_fixture.py"
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
    if focus_fit_overlay:
        environment[
            "MDKR_APP_SMOKE_CHARACTER_FIT_OVERLAY_FOCUS"
        ] = "mdkr64-character-fit-overlay-v1"
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
        "--rom", type=Path,
        help="optional verified ROM used to remove the launcher readiness overlay from rendered evidence",
    )
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    if args.rom is not None and not args.rom.resolve().is_file():
        print(
            "check_character_test_evidence_ui: FAIL -- supplied ROM is missing",
            file=sys.stderr,
        )
        return 2
    try:
        with tempfile.TemporaryDirectory(
            prefix="mdkr-character-test-evidence-"
        ) as temporary:
            root = Path(temporary)
            characters = install_fixture(root)
            if args.rom is not None:
                prefs = root / "prefs"
                prefs.mkdir()
                (prefs / "mdkr64_app.ini").write_text(
                    f"rom_path={args.rom.resolve()}\n", encoding="utf-8"
                )
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
                    "character-fit-overlay context=2 views=3 "
                    "coordinate-space=donor-target bounds=calibrated "
                    "anchor=ground-or-seat forward=measured "
                    "accessible=focusable-plots-plus-numeric",
                    "character-fit-overlay-focus applied=1 context=2 views=3",
                ),
                action="publish-qualified",
                focus_fit_overlay=True,
            )
            check_fit_overlay_bmp(root / "evidence.bmp")
            rows = evidence_rows(root)
            if (
                len(rows) != 1
                or rows[0][0] != "0"
                or rows[0][1] != PACKAGE_ID
                or (rows[0][7], rows[0][8]) != ("2", "4")
                or rows[0][9] != "11"
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
                or tuple(map(int, rows[0][104:117]))
                != (
                    1, 3, 3, 1, 1, 2,
                    176, 176, 3000000, 4000000, 5000000,
                    3500000, 6000000,
                )
                or tuple(map(int, rows[0][117:124]))
                != (176, 176, 200000, 300000, 400000, 250000, 500000)
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
                    "fitForwardMilli=0,0,1000 "
                    "gpuStatus=3 gpuScopes=3 "
                    "sceneGpuSamples=176 sceneGpuP50Ns=3000000 "
                    "characterGpuSamples=176 characterGpuP50Ns=200000",
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
                    "text=Saved GPU timestamp status, Exact timestamp samples captured.",
                    "text=Saved scene-pass GPU timestamps, 3.000 ms median · 4.000 ms p95 · 176 samples.",
                    "text=Saved character-draw GPU timestamps, 0.200 ms median · 0.300 ms p95 · 176 samples.",
                    "text=Saved excluded GPU timestamp frames, 1 pending · 1 ring-full · 2 invalid.",
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
                    "text=Front · X/Y",
                    "text=Side · Z/Y",
                    "text=Top · X/Z",
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
                    "character-preview-result rejected-evidence=gpu-timing-contract",
                    "timingVersion=1 status=99 scopes=3",
                    "action=publish-invalid-gpu applied=1 "
                    "records=0 baselines=0",
                ),
                action="publish-invalid-gpu",
            )
            if (
                root / "saves" / "character_test_evidence-v1.tsv"
            ).read_bytes() != empty_evidence:
                raise RuntimeError(
                    "invalid GPU timing contract contaminated durable performance evidence"
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
        "bound 4x4 matrix with focusable three-view bounds/anchor/facing overlays, signed renderer-fit and hand/foot contact diagnostics, same-"
        "environment wall/scene/character GPU baseline lifecycle, corruption "
        "and invalid-fit/contact/GPU refusal, pose-inspection exclusion, keyboard speech, "
        "200% rendering, and package-byte purity"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
