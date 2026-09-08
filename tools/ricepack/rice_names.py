#!/usr/bin/env python3
"""The strict filename allowlist for a Rice/GLideN64 high-resolution pack.

A Rice pack has no manifest. The filename *is* the entire contract, so this
module is the only place that decides what a file in such a pack claims to be:

    <ROM name>#<CRC>#<fmt>#<siz>[#<palette CRC>]_<part>.png

`fmt`/`siz` are the RDP format and size codes of the texture being replaced,
`CRC` is Rice's own CRC over the raw N64 texel bytes (see rice_crc.py), and
`part` says whether the file carries the whole picture (`all`) or one half of a
colour/coverage split (`rgb` + `a`).

The allowlist is deliberately an allowlist. A pack directory is somebody's
working folder: it collects editor leftovers, thumbnail databases and renamed
exports, and none of those may be guessed at. Anything the pattern does not
accept is rejected *by the pattern*, with the reason naming the class of
deviation -- never by a special case for a known junk filename, which would
only work until the next pack arrived with different junk.

Everything here is pure: no filesystem, no decoding, no policy. The caller
decides what to do with a verdict.
"""

from __future__ import annotations

from dataclasses import dataclass
import re


# The parts of a split Rice texture this importer understands. `all` is the
# complete picture; `rgb`/`a` are colour and coverage carried separately.
SUPPORTED_PARTS = ("all", "rgb", "a")

# Rice also emits colour-index variants for CI textures, whose second component
# is a palette CRC rather than texel data. They are *recognised* so a pack that
# contains them gets a reason naming the format instead of the generic "not a
# Rice filename", which would read like a corrupt file rather than an
# unimplemented one.
RECOGNISED_UNSUPPORTED_PARTS = ("ciByRGBA", "allciByRGBA", "ciByHiRes")

# Anchored end to end, with no tolerance anywhere: no surrounding whitespace, no
# case variation in the part suffix, exactly eight CRC hex digits, exactly one
# decimal digit for each of fmt and siz, and `.png` in lower case. Rice writes
# upper-case CRCs; readers in the wild also accept lower case, so both are
# taken and normalised, which is the only normalisation this pattern performs.
RICE_NAME_RE = re.compile(
    r"^(?P<rom>[^#]+)"
    r"#(?P<crc>[0-9A-Fa-f]{8})"
    r"#(?P<fmt>[0-9])"
    r"#(?P<siz>[0-9])"
    r"(?:#(?P<palette>[0-9A-Fa-f]{8}))?"
    r"_(?P<part>" + "|".join(SUPPORTED_PARTS) + r")\.png$"
)

# The same shape, but accepting any part suffix, used only to tell "a Rice name
# whose part this importer does not implement" apart from "not a Rice name".
RICE_SHAPE_RE = re.compile(
    r"^(?P<rom>[^#]+)#(?P<crc>[0-9A-Fa-f]{8})#(?P<fmt>[0-9])#(?P<siz>[0-9])"
    r"(?:#(?P<palette>[0-9A-Fa-f]{8}))?_(?P<part>[A-Za-z]+)\.png$"
)

# Stable reason codes. The manifest records these rather than prose, so a
# consumer can count them and a test can assert one without matching wording.
REASON_NOT_RICE_NAME = "name-not-rice-convention"
REASON_UNSUPPORTED_PART = "name-rice-part-unsupported"

RDP_FORMAT_NAMES = {0: "RGBA", 1: "YUV", 2: "CI", 3: "IA", 4: "I"}
RDP_SIZE_BITS = {0: 4, 1: 8, 2: 16, 3: 32}

# How a replacement's alpha channel relates to the texture it replaces. This
# drives the orphan-`_rgb` rule and nothing else.
#
#   independent -- the source format carries an alpha value that cannot be
#                  recovered from its colour, so a colour-only replacement
#                  loses information the game reads.
#   luminance   -- the source has no separate alpha; the port's decoder sets
#                  alpha equal to intensity (platform/fast3d/gfx_pc_dkr.c,
#                  the G_IM_FMT_I arms), so a colour-only replacement *could*
#                  be reconstructed -- but reconstructing it is a guess about
#                  the author's intent, not a fact about the format.
#   none        -- the format has no alpha component at all.
ALPHA_INDEPENDENT = "independent"
ALPHA_LUMINANCE = "luminance"
ALPHA_NONE = "none"

# Every (fmt, siz) pair the RDP defines, and only those. A combination outside
# this table is not "an exotic texture", it is a filename asserting a format
# that cannot exist, and is rejected rather than carried into a manifest.
#
# CI is listed as `independent` because its TLUT entries are RGBA16 or IA16;
# either way the palette supplies an alpha the colour does not.
RDP_ALPHA_CLASS: dict[tuple[int, int], str] = {
    (0, 2): ALPHA_INDEPENDENT,  # RGBA16, one bit of coverage
    (0, 3): ALPHA_INDEPENDENT,  # RGBA32
    (1, 2): ALPHA_NONE,         # YUV16
    (2, 0): ALPHA_INDEPENDENT,  # CI4  through a TLUT
    (2, 1): ALPHA_INDEPENDENT,  # CI8  through a TLUT
    (3, 0): ALPHA_INDEPENDENT,  # IA4
    (3, 1): ALPHA_INDEPENDENT,  # IA8
    (3, 2): ALPHA_INDEPENDENT,  # IA16
    (4, 0): ALPHA_LUMINANCE,    # I4
    (4, 1): ALPHA_LUMINANCE,    # I8
}


@dataclass(frozen=True)
class RiceName:
    """One accepted Rice filename, normalised."""

    rom_name: str
    crc: str            # eight upper-case hex digits
    fmt: int
    siz: int
    palette_crc: str | None
    part: str           # one of SUPPORTED_PARTS

    @property
    def key(self) -> str:
        """The texture this file claims to replace.

        Two files name the same texture when, and only when, this string
        matches: the CRC alone is not enough, because Rice's CRC is taken over
        texel bytes whose meaning depends on the format they are read as.
        """
        palette = self.palette_crc or ""
        return f"{self.crc}#{self.fmt}#{self.siz}#{palette}"

    @property
    def format_label(self) -> str:
        """`RGBA16`-style label for a human reading the manifest."""
        name = RDP_FORMAT_NAMES.get(self.fmt)
        bits = RDP_SIZE_BITS.get(self.siz)
        if name is None or bits is None:
            return f"fmt{self.fmt}/siz{self.siz}"
        return f"{name}{bits}"

    @property
    def alpha_class(self) -> str | None:
        """None when the (fmt, siz) pair is not one the RDP defines."""
        return RDP_ALPHA_CLASS.get((self.fmt, self.siz))


@dataclass(frozen=True)
class NameVerdict:
    """Accepted name, or the reason the filename was refused."""

    name: RiceName | None
    reason: str | None
    detail: str | None


def classify_filename(basename: str) -> NameVerdict:
    """Apply the allowlist to one file's name.

    Only the name is consulted. A file that passes here has still not been
    shown to be a PNG, to be within any size limit, or to name a format the
    RDP defines -- those are separate stages, kept separate so a manifest can
    say which one refused a file.
    """
    match = RICE_NAME_RE.match(basename)
    if match is not None:
        return NameVerdict(
            RiceName(
                rom_name=match.group("rom"),
                crc=match.group("crc").upper(),
                fmt=int(match.group("fmt")),
                siz=int(match.group("siz")),
                palette_crc=(match.group("palette") or "").upper() or None,
                part=match.group("part"),
            ),
            None,
            None,
        )

    shaped = RICE_SHAPE_RE.match(basename)
    if shaped is not None:
        part = shaped.group("part")
        if part in RECOGNISED_UNSUPPORTED_PARTS:
            return NameVerdict(
                None, REASON_UNSUPPORTED_PART,
                f"Rice part '_{part}' is a colour-index variant this importer "
                "does not implement")
        return NameVerdict(
            None, REASON_UNSUPPORTED_PART,
            f"Rice part '_{part}' is not one of "
            + ", ".join(f"_{p}" for p in SUPPORTED_PARTS))

    return NameVerdict(
        None, REASON_NOT_RICE_NAME,
        "filename is not <ROM name>#<CRC>#<fmt>#<siz>[#<palette CRC>]_"
        + "{" + ",".join(SUPPORTED_PARTS) + "}.png")


def rice_row_bytes(width: int, siz: int) -> int:
    """Bytes one texel row occupies in RDRAM, as Rice computes it.

    `width << siz >> 1` in the original, which is exact for every defined size
    code: 4bpp halves the width, 8bpp matches it, 16bpp doubles it, 32bpp
    quadruples it. A 4bpp row of odd width therefore truncates, exactly as the
    integer shift does -- that is the behaviour being reproduced, not a bug
    being introduced here.
    """
    if siz not in RDP_SIZE_BITS:
        raise ValueError(f"undefined RDP size code: {siz}")
    if width < 0:
        raise ValueError(f"negative width: {width}")
    return (width << siz) >> 1
