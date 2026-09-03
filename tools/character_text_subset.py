#!/usr/bin/env python3
"""Read the glyph repertoire back out of a generated character-text face.

`pyftsubset --unicodes` accepts a range the source font has never carried and
drops it without a word, so a generated header can advertise a repertoire it
does not contain. `U+2190-21FF` rode in this project's pinned list while the
upstream Roboto has no arrow glyph at any codepoint, and the launcher drew two
dozen boxes for it. The generators call `empty_ranges()` on the payload they
are about to write, which turns that class of mistake into a build error naming
the range instead of a defect a player finds.

The decoders here deliberately mirror the pinned Dear ImGui runtime that will
decode the same bytes at startup rather than reaching for a font library: the
generators must keep working with nothing but the standard library, and reading
the bytes the way the game reads them is what makes the answer meaningful.
"""

from __future__ import annotations

import re
import struct


STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
RANGE = re.compile(r"^U\+([0-9A-F]{4,6})(?:-([0-9A-F]{4,6}))?$")


def decode_base85(text: str) -> bytes:
    """Undo Dear ImGui's Encode85, the inverse of its runtime Decode85."""

    out = bytearray()
    for start in range(0, len(text), 5):
        group = text[start:start + 5]
        if len(group) != 5:
            raise ValueError("base85 payload is not a whole number of groups")
        value = 0
        for character in reversed(group):
            code = ord(character)
            value = value * 85 + (code - 36 if character >= "\\" else code - 35)
        if value > 0xFFFFFFFF:
            raise ValueError("base85 group is out of range")
        out += value.to_bytes(4, "little")
    return bytes(out)


def payload_base85(declaration: str) -> str:
    """Join the C string literals of a compressed declaration into one run."""

    return "".join(
        re.sub(r"\\(.)", r"\1", literal.group(1))
        for literal in STRING_LITERAL.finditer(declaration)
    )


def stb_decompress(data: bytes) -> bytes:
    """Decode stb_compress output, matching imgui_draw.cpp's decompressor."""

    def be(offset: int, width: int) -> int:
        return int.from_bytes(data[cursor + offset:cursor + offset + width],
                              "big")

    cursor = 0
    if be(0, 4) != 0x57BC0000 or be(4, 4) != 0:
        raise ValueError("not an stb_compress stream")
    expected = be(8, 4)
    out = bytearray()

    def match(distance: int, length: int) -> None:
        source = len(out) - distance
        if source < 0:
            raise ValueError("compressed stream matches before its start")
        for step in range(length):
            out.append(out[source + step])

    cursor = 16
    while len(out) < expected:
        token = data[cursor]
        if token >= 0x80:
            match(data[cursor + 1] + 1, token - 0x80 + 1)
            cursor += 2
        elif token >= 0x40:
            match(be(0, 2) - 0x4000 + 1, data[cursor + 2] + 1)
            cursor += 3
        elif token >= 0x20:
            length = token - 0x20 + 1
            out += data[cursor + 1:cursor + 1 + length]
            cursor += 1 + length
        elif token >= 0x18:
            match(be(0, 3) - 0x180000 + 1, data[cursor + 3] + 1)
            cursor += 4
        elif token >= 0x10:
            match(be(0, 3) - 0x100000 + 1, be(3, 2) + 1)
            cursor += 5
        elif token >= 0x08:
            length = be(0, 2) - 0x0800 + 1
            out += data[cursor + 2:cursor + 2 + length]
            cursor += 2 + length
        elif token == 0x07:
            length = be(1, 2) + 1
            out += data[cursor + 3:cursor + 3 + length]
            cursor += 3 + length
        elif token == 0x06:
            match(be(1, 3) + 1, data[cursor + 4] + 1)
            cursor += 5
        elif token == 0x04:
            match(be(1, 3) + 1, be(4, 2) + 1)
            cursor += 6
        else:
            raise ValueError(f"unknown stb token 0x{token:02X}")
    if len(out) != expected:
        raise ValueError("decompressed length disagrees with the stream header")
    return bytes(out)


def font_codepoints(font: bytes) -> set[int]:
    """Every codepoint the font's cmap maps to a real glyph.

    Only format 4 is decoded, because that is all the pinned pyftsubset writes
    for these faces. Any other format raises rather than silently reporting an
    empty repertoire, which would make every range look inert.
    """

    count = struct.unpack_from(">H", font, 4)[0]
    tables = {}
    for index in range(count):
        tag, _, offset, _ = struct.unpack_from(">4sIII", font, 12 + index * 16)
        tables[tag] = offset
    if b"cmap" not in tables:
        raise ValueError("font has no cmap table")
    cmap = tables[b"cmap"]

    codepoints: set[int] = set()
    subtables = struct.unpack_from(">H", font, cmap + 2)[0]
    for index in range(subtables):
        offset = struct.unpack_from(">I", font, cmap + 8 + index * 8)[0]
        table = cmap + offset
        fmt = struct.unpack_from(">H", font, table)[0]
        if fmt != 4:
            raise ValueError(f"unsupported cmap subtable format {fmt}")
        segments = struct.unpack_from(">H", font, table + 6)[0] // 2
        ends = table + 14
        starts = ends + segments * 2 + 2
        deltas = starts + segments * 2
        offsets = deltas + segments * 2
        for segment in range(segments):
            end = struct.unpack_from(">H", font, ends + segment * 2)[0]
            start = struct.unpack_from(">H", font, starts + segment * 2)[0]
            delta = struct.unpack_from(">h", font, deltas + segment * 2)[0]
            range_offset = struct.unpack_from(
                ">H", font, offsets + segment * 2)[0]
            if start > end:
                continue
            for codepoint in range(start, min(end, 0xFFFE) + 1):
                if range_offset == 0:
                    glyph = (codepoint + delta) & 0xFFFF
                else:
                    entry = (offsets + segment * 2 + range_offset +
                             (codepoint - start) * 2)
                    glyph = struct.unpack_from(">H", font, entry)[0]
                    if glyph != 0:
                        glyph = (glyph + delta) & 0xFFFF
                if glyph != 0:
                    codepoints.add(codepoint)
    return codepoints


def parse_ranges(unicodes: str) -> list[tuple[str, int, int]]:
    """Split a pyftsubset --unicodes list into (text, first, last) triples."""

    parsed = []
    for item in unicodes.split(","):
        text = item.strip()
        matched = RANGE.match(text)
        if matched is None:
            raise ValueError(f"malformed --unicodes entry {text!r}")
        first = int(matched.group(1), 16)
        last = int(matched.group(2), 16) if matched.group(2) else first
        if last < first:
            raise ValueError(f"reversed --unicodes entry {text!r}")
        parsed.append((text, first, last))
    return parsed


def empty_ranges(codepoints: set[int],
                 ranges: list[tuple[str, int, int]]) -> list[str]:
    """The requested ranges that contributed no glyph to the subset."""

    return [
        text for text, first, last in ranges
        if not any(codepoint in codepoints
                   for codepoint in range(first, last + 1))
    ]


def subset_from_declaration(declaration: str) -> bytes:
    """The .ttf a generated header's compressed declaration carries."""

    return stb_decompress(decode_base85(payload_base85(declaration)))
