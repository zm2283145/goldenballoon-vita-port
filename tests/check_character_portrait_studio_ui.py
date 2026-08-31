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
import zlib
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
                compact: bool, accessible: bool,
                portrait_source: Path) -> dict[str, str]:
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
        "MDKR_APP_SMOKE_FRAMES": "800" if accessible else "16",
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
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
        "MDKR_APP_SMOKE_PORTRAIT_SOURCE": str(portrait_source),
        "MDKR_APP_SMOKE_PORTRAIT_SOURCE_TOKEN":
            "mdkr64-portrait-source-v1",
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


def png_chunk(name: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + name + payload +
            struct.pack(">I", zlib.crc32(name + payload) & 0xFFFFFFFF))


def write_portrait_source(path: Path) -> None:
    width, height = 96, 64
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            subject = 28 <= x < 68 and 8 <= y < 58
            rows.extend((220, 68, 76, 255) if subject else
                        (32, 110 + y, 180, 255))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                        8, 6, 0, 0, 0)) +
        png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) +
        png_chunk(b"IEND", b""))


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
        settings_source = (
            ROOT / "platform" / "app" / "ui_settings.cpp"
        ).read_text(encoding="utf-8")
        theme_source = (
            ROOT / "platform" / "app" / "app_theme.cpp"
        ).read_text(encoding="utf-8")
        launcher_source = (
            ROOT / "platform" / "app" / "ui_launcher.cpp"
        ).read_text(encoding="utf-8")
        for marker in (
            "portraitSourceHandoff",
            "prepareInlineCharacterCapture",
            "g_characterPendingPortraitSources[packageId]",
            "Portrait model capture validated and returned",
            "prepareCharacterWorkshopHeading",
            "##character-workshop-native-heading",
            "Type or paste Unicode in logical order",
        ):
            if marker not in settings_source:
                raise RuntimeError(
                    f"quick portrait handoff lost settings link {marker!r}"
                )
        for marker in (
            "acceptCharacterPreviewRequest",
            "characterPreviewPortraitSourceHandoff",
            "state_.characterPreviewPortraitSourceHandoff",
        ):
            if marker not in launcher_source:
                raise RuntimeError(
                    f"quick portrait handoff lost launcher link {marker!r}"
                )
        for marker in (
            "kCharacterNameGlyphRanges",
            "kArabicGlyphRanges",
            "kHebrewGlyphRanges",
            "gfx_character_text_latin_face_base85()",
            "gfx_character_text_arabic_face_base85()",
            "gfx_character_text_hebrew_face_base85()",
        ):
            if marker not in theme_source:
                raise RuntimeError(
                    f"launcher text fields lost embedded script face {marker!r}"
                )
        with tempfile.TemporaryDirectory(
                prefix="mdkr-portrait-studio-ui-") as temporary:
            root = Path(temporary)
            characters = install_fixture(root, display_name="Hístory Œ 🏁")
            before = inventory(characters)
            portrait_source = root / "portrait-source.png"
            write_portrait_source(portrait_source)

            compact = root / "compact"
            compact.mkdir()
            compact_shot = compact / "portrait-studio-compact.bmp"
            run(
                binary, root,
                environment(compact, characters, compact_shot,
                            compact=True, accessible=False,
                            portrait_source=portrait_source),
                ("active-panel=Character Workshop",
                 "compact-layout dense=1 contained=1 ",
                 "character-portrait-variants package=" + PACKAGE_ID +
                 " count=6 source=canvas columns=1 scale=2.00",
                 "character-portrait-readability package=" + PACKAGE_ID +
                 " views=7 columns=1 scale=2.00",
                 "character-portrait-camera package=" + PACKAGE_ID +
                 " quickCreate=1 quickContext=1 quickAutoReturn=1 "
                 "quickManagedCache=1 quickDigestHandoff=1 "
                 "presets=front,left-three-quarter,right-three-quarter"),
            )
            check_bmp(compact_shot, 640, 480)

            accessible = root / "accessible"
            accessible.mkdir()
            accessible_shot = accessible / "portrait-studio-a11y.bmp"
            run(
                binary, root,
                environment(accessible, characters, accessible_shot,
                            compact=False, accessible=True,
                            portrait_source=portrait_source),
                ("character-portrait-style package=" + PACKAGE_ID,
                 "character-portrait-variants package=" + PACKAGE_ID +
                 " count=6 source=canvas",
                 "character-portrait-readability package=" + PACKAGE_ID +
                 " views=7",
                 "character-name-projection package=" + PACKAGE_ID +
                 " display_codepoints=11 display_folded=2 display_fallback=1 "
                 "display_mode=retail display_reason=missing glyph "
                 "display_shaped=0 display_rtl=0 display_bidi_runs=0 "
                 "short_codepoints=11 short_folded=2 short_fallback=1 "
                 "short_mode=retail short_reason=missing glyph "
                 "short_shaped=0 short_rtl=0 short_bidi_runs=0 valid=1 "
                 "shared-engine-path=1",
                 "Display retail-font fallback, History OE ?. Read-only exact "
                 "retail-glyph projection.",
                 "Short tile retail-font fallback, History OE ?. Read-only "
                 "retail-glyph projection.",
                 "character-portrait-source-action package=" + PACKAGE_ID +
                 " loaded=1 applied=0",
                 "character-portrait-source package=" + PACKAGE_ID,
                 "character-portrait-camera package=" + PACKAGE_ID +
                 " quickCreate=1 quickContext=1 quickAutoReturn=1 "
                 "quickManagedCache=1 quickDigestHandoff=1 "
                 "presets=front,left-three-quarter,right-three-quarter",
                 "kind=local-png dimensions=96x64",
                 "mask=1 removed=16",
                 "text=Portrait input PNG",
                 "text=Undo portrait framing and mask",
                 "text=Redo portrait framing and mask",
                 "text=Front portrait",
                 "text=Left three-quarter",
                 "text=Right three-quarter",
                 "text=Square crop size",
                 "text=Edge-connected matte removal",
                 "text=Background frame",
                 "text=Enable freeform subject mask",
                 "text=Remove from subject",
                 "text=Restore subject",
                 "text=Mask brush size",
                 "text=Freeform subject mask canvas",
                 "text=Mask pixel coordinates",
                 "text=Apply mask brush to selected pixel",
                 "text=Invert subject mask",
                 "text=Reset subject mask to keep all",
                 "text=Apply styled source to pixel canvas",
                 "palette=32",
                 "text=Framing zoom",
                 "text=Palette target",
                 "text=Use Clean 64",
                 "text=Use Classic 32",
                 "text=Use Bold 16",
                 "text=Use Crisp 32",
                 "text=Use Dithered 32",
                 "text=Use Soft 64",
                 "text=Native transparent card portrait preview",
                 "text=Dark HUD stress portrait preview",
                 "text=Light results stress portrait preview",
                 "text=Grayscale stress portrait preview",
                 "text=Protanopia stress portrait preview",
                 "text=Deuteranopia stress portrait preview",
                 "text=Tritanopia stress portrait preview",
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

            # Exercise the opposite presentation path independently: a mixed
            # LTR/RTL name must use the exact native shaper in both visible
            # previews and expose that fact to keyboard/speech users. Keeping
            # this separate from the emoji fixture proves the honest fallback
            # and native bidi paths without allowing either to mask the other.
            rtl_root = root / "rtl"
            rtl_root.mkdir()
            rtl_characters = install_fixture(
                rtl_root, display_name="Dixie \u062f\u064a\u0643\u0633\u064a"
            )
            rtl_before = inventory(rtl_characters)
            rtl_ui = rtl_root / "accessible"
            rtl_ui.mkdir()
            rtl_shot = rtl_ui / "portrait-studio-rtl-a11y.bmp"
            run(
                binary, rtl_root,
                environment(rtl_ui, rtl_characters, rtl_shot,
                            compact=False, accessible=True,
                            portrait_source=portrait_source),
                ("character-name-projection package=" + PACKAGE_ID +
                 " display_codepoints=11 display_folded=0 display_fallback=5 "
                 "display_mode=native display_reason=none display_shaped=1 "
                 "display_rtl=1 display_bidi_runs=2",
                 "short_mode=native short_reason=none short_shaped=1 "
                 "short_rtl=1 short_bidi_runs=2 valid=1 "
                 "shared-engine-path=1",
                 "text=Display native glyph preview, native shaped RTL or "
                 "mixed-direction glyphs.",
                 "text=Short-name native glyph preview, native shaped RTL or "
                 "mixed-direction glyphs with exact compact fit."),
            )
            check_bmp(rtl_shot, 1280, 720)
            if inventory(rtl_characters) != rtl_before:
                raise RuntimeError(
                    "native bidi preview or accessibility walk mutated "
                    "installed package bytes"
                )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_portrait_studio_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_portrait_studio_ui: PASS -- deterministic style "
          "lab and six-variant comparison sheet, one-action exact model "
          "portrait handoff, bounded PNG source/capture "
          "framing with durable freeform subject mask, advanced pixel tools, "
          "200% compact rendering, honest missing-glyph fallback, exact "
          "mixed-direction shaping, keyboard speech, and installed-byte "
          "purity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
