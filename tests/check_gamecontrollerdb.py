#!/usr/bin/env python3
"""Lint the shipped SDL controller-mapping database.

The port layers ``lib/sdl_gamecontrollerdb/gamecontrollerdb.txt`` over SDL's
built-in database at boot (platform/platform_sdl_min.c, platform_input_init).
Two things can silently break a player's pad and nothing in the build would
notice:

1. A malformed line. SDL skips lines it cannot parse without failing the
   load, so a typo in a curated entry demotes the pad to SDL's internal
   fallback mapping -- exactly the failure class of issue #55.
2. A refresh from upstream (github.com/mdqinc/SDL_GameControllerDB) that
   drops the curated additions kept below the upstream snapshot.

What this gate checks
---------------------
* Every mapping line parses: 32-hex-digit GUID (or SDL's literal ``xinput``),
  ``key:value`` fields drawn from SDL 2.32.10's accepted output names, values
  matching SDL's input grammar (``bN``, ``hN.N``, ``[+-]aN``, ``aN~``), and a
  ``platform`` field naming a platform SDL knows.
* No two lines share a (GUID, platform) pair. SDL replaces an existing
  mapping when the same GUID is added again, so a duplicate means one of the
  two curated lines silently never applies.
* The curated NSO N64 HIDAPI entries exist and are behaviorally equivalent
  to the upstream raw-DirectInput entry for the same pad
  (030000007e0500001920000000000000): same set of SDL outputs, C buttons on
  the right-stick half-axes, Z the only lefttrigger, and nothing bound to
  BACK (the overlay's default menu toggle). Issue #55: over Bluetooth SDL's
  HIDAPI Nintendo-Classic driver claims the pad under a GUID the DirectInput
  entry never matches, and SDL's internal fallback sends C-Right to BACK and
  C-Down to a second Z.

Derivation of the pinned HIDAPI mapping (SDL 2.32.10 sources, the exact
version the release links):

* GUID: bus(03 USB / 05 Bluetooth) + 0000 crc + 7e05 vendor + 0000 + 1920
  product + 0000 + 0000 version + 68 ('h' HIDAPI signature) + 0c
  (k_eSwitchDeviceInfoControllerType_N64, SDL_hidapi_nintendo.h; written to
  guid.data[15] in SDL_hidapi_switch.c). CRC is always masked during
  matching and version is masked on the retry pass (SDL_gamecontroller.c,
  SDL_PrivateMatchControllerMappingForGUID), so zeros match every unit.
* Joystick indices: the driver reports fixed SDL controller numbering for
  non-JoyCon units (SDL_hidapi_switch.c HandleFullControllerState /
  HandleSimpleControllerState; RemapButton is identity because
  AlwaysUsesLabels() returns true for the N64 type): A bit->b0, B->b1,
  X->b2, Y->b3, Minus->b4, Home->b5, Plus->b6, LStick->b7, L->b9, R->b10,
  dpad->b11..b14, Capture->b15, ZL->axis a4 (digital min/max), ZR->axis a5,
  stick->a0/a1.
* Pad firmware (Linux hid-nintendo n64con ground truth, quoted in the issue
  #55 investigation): C-Up=Y bit, C-Down=ZR, C-Left=X, C-Right=Minus, Z=ZL,
  rear ZR=LStick.

The one form still awaiting on-hardware confirmation from the issue #55
reporter is ``+righty:a5`` (C-Down driven by the digital ZR trigger axis:
rest -32768 scales to 0, pressed +32767 scales to full). Everything else is
button-for-button from the driver source.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DB = ROOT / "lib" / "sdl_gamecontrollerdb" / "gamecontrollerdb.txt"

# SDL 2.32.10 output names: map_StringForControllerAxis /
# map_StringForControllerButton (src/joystick/SDL_gamecontroller.c), plus the
# forward-compatible misc2..misc5 upstream already ships for SDL3 consumers
# (SDL2 ignores fields it does not know).
OUTPUT_KEYS = {
    "a", "b", "x", "y", "back", "guide", "start",
    "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright",
    "misc1", "misc2", "misc3", "misc4", "misc5",
    "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
}
HALF_AXIS_OUTPUTS = {"leftx", "lefty", "rightx", "righty"}
SPECIAL_KEYS = {"platform", "crc", "hint", "sdk>=", "sdk<="}
PLATFORMS = {"Windows", "Mac OS X", "Linux", "Android", "iOS", "tvOS"}

GUID_RE = re.compile(r"^[0-9a-f]{32}$")
# SDL_PrivateGameControllerParseElement input grammar: button bN, hat hN.N,
# axis with optional +/- half prefix and optional ~ inversion suffix.
VALUE_RE = re.compile(r"^(b\d+|h\d+\.\d+|[+-]?a\d+~?)$")

NSO_N64_HIDAPI_GUIDS = (
    "030000007e050000192000000000680c",  # HIDAPI GUID, USB bus byte
    "050000007e050000192000000000680c",  # HIDAPI GUID, Bluetooth bus byte
)
NSO_N64_HIDAPI_PLATFORMS = ("Windows", "Mac OS X", "Linux")
NSO_N64_DINPUT_GUID = "030000007e0500001920000000000000"

# The full pinned mapping, from the derivation in the module docstring.
NSO_N64_HIDAPI_EXPECTED = {
    "+rightx": "b4",   # C-Right (Minus bit) -- the button SDL's fallback sent to BACK
    "+righty": "a5",   # C-Down (ZR digital trigger axis) -- hardware-confirmation ask
    "-rightx": "b2",   # C-Left (X bit)
    "-righty": "b3",   # C-Up (Y bit)
    "a": "b0",
    "b": "b1",
    "dpdown": "b12",
    "dpleft": "b13",
    "dpright": "b14",
    "dpup": "b11",
    "guide": "b5",
    "leftshoulder": "b9",
    "lefttrigger": "a4",   # Z (ZL digital trigger axis)
    "leftx": "a0",
    "lefty": "a1",
    "misc1": "b15",
    "rightshoulder": "b10",
    "righttrigger": "b7",  # rear ZR (LStick bit)
    "start": "b6",
}


def parse_line(line: str) -> tuple[str, str, dict[str, str], str | None]:
    """Split one mapping line into (guid, name, fields, platform)."""
    parts = [p for p in line.split(",") if p != ""]
    if len(parts) < 3:
        raise ValueError("fewer than three comma-separated fields")
    guid, name = parts[0], parts[1]
    fields: dict[str, str] = {}
    platform: str | None = None
    for field in parts[2:]:
        if ":" not in field:
            raise ValueError(f"field without a colon: {field!r}")
        key, value = field.split(":", 1)
        if key == "platform":
            platform = value
            continue
        if key in fields:
            raise ValueError(f"repeated field: {key!r}")
        fields[key] = value
    return guid, name, fields, platform


def output_name(key: str) -> str:
    """The SDL output a field drives, with any half-axis prefix stripped."""
    return key[1:] if key[:1] in "+-" else key


def main() -> int:
    errors: list[str] = []
    seen: dict[tuple[str, str], int] = {}
    mappings: dict[tuple[str, str], dict[str, str]] = {}

    if not DB.is_file():
        print(f"check_gamecontrollerdb: FAIL: missing {DB}", file=sys.stderr)
        return 1

    for lineno, raw in enumerate(DB.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            guid, _name, fields, platform = parse_line(line)
        except ValueError as exc:
            errors.append(f"line {lineno}: {exc}")
            continue
        if guid != "xinput" and not GUID_RE.match(guid):
            errors.append(f"line {lineno}: bad GUID {guid!r}")
            continue
        if platform is None or platform not in PLATFORMS:
            errors.append(f"line {lineno}: missing or unknown platform field")
            continue
        for key, value in fields.items():
            if key in SPECIAL_KEYS:
                continue
            base = output_name(key)
            if base not in OUTPUT_KEYS:
                errors.append(f"line {lineno}: unknown output {key!r}")
            elif key[:1] in "+-" and base not in HALF_AXIS_OUTPUTS:
                errors.append(f"line {lineno}: {key!r} is not a half-axis output")
            if not VALUE_RE.match(value):
                errors.append(f"line {lineno}: bad input spec {key}:{value}")
        pair = (guid, platform)
        if pair in seen:
            errors.append(
                f"line {lineno}: duplicate GUID {guid} for platform {platform} "
                f"(first at line {seen[pair]}); SDL keeps only one, so one of "
                "the two curated lines never applies")
        else:
            seen[pair] = lineno
            mappings[pair] = fields

    # --- Curated NSO N64 HIDAPI entries (issue #55) -------------------------
    dinput = mappings.get((NSO_N64_DINPUT_GUID, "Windows"))
    if dinput is None:
        errors.append(
            f"upstream raw-DirectInput NSO N64 entry {NSO_N64_DINPUT_GUID} "
            "(platform Windows) is gone; the HIDAPI entries below are pinned "
            "as behaviorally equivalent to it")

    for guid in NSO_N64_HIDAPI_GUIDS:
        for platform in NSO_N64_HIDAPI_PLATFORMS:
            entry = mappings.get((guid, platform))
            if entry is None:
                errors.append(
                    f"missing curated NSO N64 HIDAPI entry {guid} for "
                    f"platform {platform} (issue #55: without it SDL's "
                    "internal fallback sends C-Right to BACK and C-Down to a "
                    "second Z)")
                continue
            if entry != NSO_N64_HIDAPI_EXPECTED:
                for key in sorted(set(entry) | set(NSO_N64_HIDAPI_EXPECTED)):
                    got, want = entry.get(key), NSO_N64_HIDAPI_EXPECTED.get(key)
                    if got != want:
                        errors.append(
                            f"NSO N64 HIDAPI entry {guid} ({platform}): "
                            f"{key} is {got!r}, pinned derivation says {want!r}")
            # The behavioral contract with the DirectInput entry, stated
            # directly so a future edit to EITHER line trips something legible:
            # same SDL outputs driven, C buttons on right-stick half-axes,
            # and no BACK binding to collide with the overlay toggle.
            if dinput is not None:
                got_outputs = {output_name(k) for k in entry}
                want_outputs = {output_name(k) for k in dinput}
                if got_outputs != want_outputs:
                    errors.append(
                        f"NSO N64 HIDAPI entry {guid} ({platform}) drives "
                        f"outputs {sorted(got_outputs)} but the DirectInput "
                        f"entry drives {sorted(want_outputs)}")
            if "back" in entry:
                errors.append(
                    f"NSO N64 HIDAPI entry {guid} ({platform}) binds BACK; "
                    "that re-creates the issue #55 menu-toggle collision")

    if errors:
        for error in errors:
            print(f"check_gamecontrollerdb: {error}", file=sys.stderr)
        print(f"check_gamecontrollerdb: FAIL ({len(errors)} problem(s))",
              file=sys.stderr)
        return 1

    print(f"check_gamecontrollerdb: PASS ({len(seen)} mapping lines, "
          f"{len(NSO_N64_HIDAPI_GUIDS) * len(NSO_N64_HIDAPI_PLATFORMS)} "
          "curated NSO N64 HIDAPI entries verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
