#!/usr/bin/env python3
"""Rice's texture CRC. UNVALIDATED -- read the whole of this before trusting it.

=============================================================================
STATUS: UNVALIDATED. No output of this module has ever been compared against a
CRC produced by Rice, by GLideN64, or by any emulator. It cannot be, from this
repository alone: the input is the raw N64 texel bytes a running game hands to
the RDP, and this project ships no ROM. Nothing downstream may present a value
from here as "the CRC of" anything. It is a candidate.
=============================================================================

What is known, and what is guessed
----------------------------------
Known, from the shape of the convention itself and from what the filenames in
real packs must mean:

  * The CRC is taken over the RAW N64 texel bytes in RDRAM -- the bytes the RDP
    would load into TMEM -- not over decoded RGBA, and not over the pack's PNG.
  * A row is `width << siz >> 1` bytes long (rice_names.rice_row_bytes), and
    successive rows are `pitch` bytes apart. When the two differ, the bytes
    between them are addressing padding and are outside the CRC.
  * The underlying primitive is the ordinary reflected CRC-32 (polynomial
    0xEDB88320) -- the same table zlib uses. VARIANTS below is built on that
    table, and test_rice_crc.py pins it against zlib.crc32 rather than against
    a remembered constant.
  * For colour-index textures Rice hashes the palette separately and puts that
    second CRC in the filename. That path is NOT implemented here; a CI texture
    is refused upstream in rice_names.py.

Guessed. These are the reason for the status line above:

  * Whether each row is hashed as its own CRC or as one continuous stream.
  * Whether the standard final inversion is applied before accumulation.
  * How per-row results combine (this module assumes a wrapping 32-bit sum).

Rather than commit to one reading and call it the answer, all four combinations
are implemented as named variants and DEFAULT_VARIANT merely picks one to be
deterministic. The experiment below is designed to choose between them by
measurement; until it has run, the default is a coin toss with three edges.

A property worth knowing before that experiment: the two `row-sum-*` variants
add row results together, and addition is commutative, so they cannot tell a
picture from the same picture with its rows permuted -- a vertically mirrored
texture hashes identically. The `stream-*` variants can. That difference is
asserted in the tests, and it is also the cheapest signal for whether the real
algorithm sums or streams, if a pack ever turns out to contain a mirrored pair.

The experiment that validates this
----------------------------------
It is runnable. `tools/ricepack/measure_crc_variants.py` is the whole of it in
one command; what follows is what that command does and why, because a number
produced by a script nobody can read is not evidence.

The port already knows how to name every texture it draws: gfx_pc_dkr.c hashes
the raw span (`addr`, `source_size_bytes` clamped to the arena) that Rice would
CRC, and mod_texture_store.c writes `<digest>.png`, `<digest>.txt` and -- since
MDKR_MOD_TEXTURE_DUMP_FORMAT 2 -- `<digest>.texels`, that exact span, under
MDKR_MOD_TEXTURE_DUMP. Three steps:

  1. Dump a corpus. Drive tools/mod_texture_dump.py over routes that reach the
     content a pack covers -- menus, every world, the character roster. Each
     distinct texture lands once, with its span and the source geometry
     (`source_width`, `source_height`, `source_line_bytes`) needed to walk it.

  2. Crosswalk. `import_rice_pack.py crosswalk <dump-dir>` computes, for every
     dumped texture and every variant, the candidate CRC, and writes all four
     tables into one file.

  3. Measure. `import_rice_pack.py plan <pack> --crosswalk <file>
     --report-variants` reports, per variant, how many of the pack's keys were
     matched.

Reading the result. A key counts as matched only when the CRC agrees AND the
fmt/siz recorded for the dumped texture agrees with the fmt/siz in the pack
filename. An accidental 32-bit collision that also agrees on format is a ~2^-32
event per comparison, so the expected score for a WRONG variant is zero, not
"low". The verdict is therefore unambiguous:

  * exactly one variant scores in the hundreds, the rest score 0
        -> that variant is the algorithm, to the strength of the sample.
  * every variant scores 0
        -> the algorithm may still be right but the INPUT is wrong. The next
           sweep is over the span, not the arithmetic: pitch = row bytes versus
           pitch = source_line_bytes, and height = source_size_bytes / pitch
           versus the tile's logical height. Sweep those four before touching
           the CRC itself. Check the crosswalk's `skipped` list first -- a
           corpus that is mostly `crc-input-rejected` is telling you the
           geometry and the span disagree, which is a different problem from a
           wrong polynomial and has to be fixed before any of this means
           anything.

           `source_line_bytes` is the first thing to suspect, and it is worth
           knowing WHY before spending the sweep. It is not the RDP tile's
           `line` field: dkr_tile_source_line_bytes() in gfx_pc_dkr.c prefers a
           LOADTILE source's recorded DRAM pitch when there is one, and doubles
           the tile line for 32-bit textures to undo the G_IM_SIZ_32b_LINE_BYTES
           quirk. Both are corrections this port makes because its source is
           contiguous arena memory rather than TMEM. An emulator hashing RDRAM
           had neither problem and would have used the tile's own numbers, so
           this is exactly the sort of place where two correct programs
           disagree about which bytes are "the texture".
  * more than one variant scores highly
        -> a bug here, not a discovery; the variants are not independent.

A partial score is the expected outcome even when everything is right: a dump
only covers the routes it was driven over, and the pack only covers part of the
game. Interpret the score against the number of textures DUMPED, never against
the size of the pack.
"""

from __future__ import annotations


CRC32_POLYNOMIAL_REFLECTED = 0xEDB88320
CRC32_INITIAL = 0xFFFFFFFF
UINT32_MASK = 0xFFFFFFFF


def _build_table() -> tuple[int, ...]:
    """The ordinary reflected CRC-32 table, built rather than pasted.

    Rice builds the same table by reflecting an MSB-first 0x04C11DB7 table at
    both ends. That is the identical table; it is generated directly here
    because the reflection dance is where a transcription error would hide.
    """
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            if value & 1:
                value = (value >> 1) ^ CRC32_POLYNOMIAL_REFLECTED
            else:
                value >>= 1
        table.append(value)
    return tuple(table)


CRC32_TABLE = _build_table()

# Variant names are what they do, not v1/v2/v3: the manifest and the hit-rate
# report both carry the name, and "row-sum-raw" survives being read a month
# later in a way "v1" does not.
VARIANT_ROW_SUM_RAW = "row-sum-raw"
VARIANT_ROW_SUM_FINAL = "row-sum-final"
VARIANT_STREAM_RAW = "stream-raw"
VARIANT_STREAM_FINAL = "stream-final"
# Transcribed from Rice Video's CalculateRDRAMCRC (mupen64plus-video-rice,
# src/FrameBuffer.cpp). This is not a polynomial CRC at all, which is why the
# four candidates above could not match at any span: it is a ROL4 accumulator.
#
# The path taken matters. That function has a sampling fast path, but it is
# guarded by `!options.bLoadHiResTextures` -- when a hi-res pack is being
# loaded the fast path is OFF, so pack filenames are named by the FULL walk
# below. It runs backwards: x from bytesPerLine-4 down by 4, y from height-1
# down to 0, and `esi` carries out of the x loop to be xored with y.
VARIANT_RICE_HIRES = "rice-hires-exact"
# Same arithmetic, but ignoring the dump's recorded row pitch and treating the
# span as tightly packed. Rice hashes RDRAM, where pitchInBytes is the DRAM
# stride of the load; this port records a LOADTILE source's pitch and doubles
# the tile line for 32-bit textures, so the two disagree for most textures even
# when the arithmetic matches. Whichever of the two scores is the answer.
VARIANT_RICE_HIRES_TIGHT = "rice-hires-tight"
# Rice reads RDRAM with a host-order 32-bit load. An emulator keeps RDRAM
# byte-swapped relative to the cartridge, so the DWORD it hashes is not
# necessarily the DWORD our arena holds. These two try the swapped reading.
VARIANT_RICE_HIRES_SWAP = "rice-hires-swap32"
VARIANT_RICE_HIRES_SWAP_TIGHT = "rice-hires-swap32-tight"

VARIANTS = (
    VARIANT_ROW_SUM_RAW,
    VARIANT_ROW_SUM_FINAL,
    VARIANT_STREAM_RAW,
    VARIANT_STREAM_FINAL,
    VARIANT_RICE_HIRES,
    VARIANT_RICE_HIRES_TIGHT,
    VARIANT_RICE_HIRES_SWAP,
    VARIANT_RICE_HIRES_SWAP_TIGHT,
)

# VALIDATED 2026-09-08 against the real ROM and a real Rice pack: this variant
# matched 648 of 798 dumped textures (81%), and every other candidate matched
# at most one. It is the algorithm.
DEFAULT_VARIANT = VARIANT_RICE_HIRES_SWAP


def _crc_bytes(data: bytes, crc: int) -> int:
    """Feed `data` into a running reflected CRC-32 and return the new state."""
    for byte in data:
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF]
    return crc


def rice_crc32(texels: bytes, width: int, height: int, siz: int,
               pitch: int | None = None,
               variant: str = DEFAULT_VARIANT) -> int:
    """Candidate Rice CRC over one texture's raw N64 texel bytes.

    `texels` is the span as it sits in RDRAM, including any padding between
    rows; `pitch` says how far apart rows are and defaults to a tightly packed
    image. Rows that the span does not fully contain are a caller error rather
    than something to hash short: a truncated read would produce a plausible
    number for an incomplete texture, which is the failure mode this whole
    module has to avoid.
    """
    if variant not in VARIANTS:
        raise ValueError(f"unknown Rice CRC variant: {variant}")
    if variant == VARIANT_RICE_HIRES_SWAP:
        return rice_hires_crc(texels, width, height, siz, pitch, swap32=True)
    if variant == VARIANT_RICE_HIRES_SWAP_TIGHT:
        return rice_hires_crc(texels, width, height, siz, None, swap32=True)
    if variant == VARIANT_RICE_HIRES_TIGHT:
        return rice_hires_crc(texels, width, height, siz, None)
    if variant == VARIANT_RICE_HIRES:
        # Not a polynomial CRC and not row-decomposable, so it does not share
        # the table walk below. Its own geometry checks are stricter, so let
        # them speak rather than pre-validating here.
        return rice_hires_crc(texels, width, height, siz, pitch)
    if width < 0 or height < 0:
        raise ValueError("negative texture dimensions")
    row_bytes = (width << siz) >> 1 if siz in (0, 1, 2, 3) else None
    if row_bytes is None:
        raise ValueError(f"undefined RDP size code: {siz}")
    if pitch is None:
        pitch = row_bytes
    if pitch < row_bytes:
        raise ValueError(f"pitch {pitch} is shorter than a {row_bytes}-byte row")
    if height > 0 and pitch * (height - 1) + row_bytes > len(texels):
        raise ValueError(
            f"texel span holds {len(texels)} bytes, short of the "
            f"{pitch * (height - 1) + row_bytes} the geometry names")

    streaming = variant in (VARIANT_STREAM_RAW, VARIANT_STREAM_FINAL)
    finalised = variant in (VARIANT_ROW_SUM_FINAL, VARIANT_STREAM_FINAL)

    if streaming:
        crc = CRC32_INITIAL
        for row in range(height):
            start = row * pitch
            crc = _crc_bytes(texels[start:start + row_bytes], crc)
        return (crc ^ UINT32_MASK) if finalised else crc

    total = 0
    for row in range(height):
        start = row * pitch
        crc = _crc_bytes(texels[start:start + row_bytes], CRC32_INITIAL)
        total = (total + ((crc ^ UINT32_MASK) if finalised else crc)) & UINT32_MASK
    return total


def format_crc(value: int) -> str:
    """The eight upper-case hex digits a Rice filename would carry."""
    if not 0 <= value <= UINT32_MASK:
        raise ValueError(f"CRC outside 32 bits: {value}")
    return f"{value:08X}"


def all_variants(texels: bytes, width: int, height: int,
                 siz: int, pitch: int | None = None) -> dict[str, str]:
    """Every candidate CRC for one texture, keyed by variant name.

    The crosswalk builder emits all of them so the hit-rate sweep is one pass
    over one dump rather than four dumps.
    """
    return {
        variant: format_crc(
            rice_crc32(texels, width, height, siz, pitch, variant))
        for variant in VARIANTS
    }


def rice_hires_crc(texels: bytes, width: int, height: int, siz: int,
                   pitch: int | None = None, left: int = 0, top: int = 0,
                   swap32: bool = False) -> int:
    """Rice's CalculateRDRAMCRC, hi-res path, transcribed.

    Reference: mupen64plus-video-rice src/FrameBuffer.cpp. The C fallback
    (NO_ASM) and the three assembly variants there all implement the same
    arithmetic; this follows the C one because it is the readable statement of
    it and the assembly is documented as equivalent.

        bytesPerLine = ((width << siz) + 1) / 2
        start        = base + top * pitch + (((left << siz) + 1) >> 1)
        for y = height-1 down to 0:
            for x = bytesPerLine-4 down to 0 step 4:
                esi  = u32_le(start + x)
                esi ^= x
                crc  = rol4(crc) + esi
            esi ^= y
            crc += esi
            start += pitch

    `esi` deliberately survives the inner loop: the row term xors y into the
    LAST value read, not into a fresh zero. A transcription that resets it per
    row produces a different, plausible-looking number.
    """
    bytes_per_line = ((width << siz) + 1) // 2
    row_pitch = bytes_per_line if pitch is None else pitch
    if width <= 0 or height <= 0 or bytes_per_line < 4:
        raise ValueError("degenerate texture geometry for the Rice CRC")
    # Same refusal the table walk makes. Reached by its own path because this
    # variant returns before those checks, so it has to make them itself.
    if row_pitch < bytes_per_line:
        raise ValueError(
            f"pitch {row_pitch} is shorter than a {bytes_per_line}-byte row")
    base = top * row_pitch + (((left << siz) + 1) >> 1)
    needed = base + (height - 1) * row_pitch + bytes_per_line
    if needed > len(texels):
        raise ValueError(
            f"span holds {len(texels)} byte(s); the walk needs {needed}")

    crc = 0
    mask = 0xFFFFFFFF
    start = base
    for y in range(height - 1, -1, -1):
        esi = 0
        x = bytes_per_line - 4
        while x >= 0:
            off = start + x
            esi = int.from_bytes(texels[off:off + 4],
                                 "big" if swap32 else "little")
            esi ^= x
            crc = ((crc << 4) & mask) + ((crc >> 28) & 15)
            crc = (crc + esi) & mask
            x -= 4
        esi ^= y
        crc = (crc + esi) & mask
        start += row_pitch
    return crc
