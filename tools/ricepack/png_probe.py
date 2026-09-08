#!/usr/bin/env python3
"""Read a pack PNG far enough to judge it, and no further.

Two jobs, deliberately split:

  read_png_header() walks the container to IHDR only. It is what the scanning
  pass uses, because deciding whether 1816 files are acceptable must not cost
  1816 full decompressions -- and because a decompression bomb has to be
  refused BEFORE anything expands it, which is only possible from the header.

  decode_rgba()/compose_split() decompress. They run in the materialising pass,
  over the handful of files that survived scanning, and never over a file whose
  header was not accepted first.

The decoder covers 8-bit non-interlaced greyscale, RGB, greyscale+alpha and
RGBA, and refuses everything else with a reason. That is not the whole PNG
specification and is not meant to be: a pack file outside that subset is far
more likely to be a mistake than an intentional exotic encoding, and a wrong
guess about a 16-bit or palettised image would be silently wrong pixels rather
than a refusal.
"""

from __future__ import annotations

from dataclasses import dataclass
import struct
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Channel count per PNG colour type. Colour type 3 (palette) is listed so the
# header probe can size it honestly before the decoder refuses it.
PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
DECODABLE_COLOUR_TYPES = (0, 2, 4, 6)

# Reason codes, as in rice_names.py: recorded in the manifest, asserted by name.
REASON_NOT_PNG = "png-signature-missing"
REASON_HEADER_UNREADABLE = "png-header-unreadable"
REASON_COLOUR_TYPE = "png-colour-type-unsupported"
REASON_BIT_DEPTH = "png-bit-depth-unsupported"
REASON_INTERLACED = "png-interlace-unsupported"
REASON_DATA_UNREADABLE = "png-data-unreadable"


class PngError(Exception):
    """A refusal carrying a stable reason code plus prose for a human."""

    def __init__(self, reason: str, detail: str):
        super().__init__(detail)
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True)
class PngHeader:
    width: int
    height: int
    bit_depth: int
    colour_type: int
    interlace: int

    @property
    def channels(self) -> int:
        return PNG_CHANNELS.get(self.colour_type, 4)

    @property
    def pixels(self) -> int:
        return self.width * self.height

    @property
    def decoded_bytes(self) -> int:
        """Bytes the image occupies once expanded, as the caps are measured.

        Sampled at the file's own depth and channel count rather than at the
        RGBA8 the game would upload, because this number exists to bound what
        DECOMPRESSING costs, and that is what decompressing produces.
        """
        return self.pixels * self.channels * max(self.bit_depth // 8, 1)


def read_png_header(data: bytes) -> PngHeader:
    """Parse the signature and IHDR. Never decompresses anything."""
    if len(data) < 8 or data[:8] != PNG_SIGNATURE:
        raise PngError(REASON_NOT_PNG, "the file does not begin with a PNG signature")
    if len(data) < 33 or data[12:16] != b"IHDR":
        raise PngError(REASON_HEADER_UNREADABLE, "IHDR is missing or truncated")
    length = struct.unpack(">I", data[8:12])[0]
    if length != 13:
        raise PngError(REASON_HEADER_UNREADABLE,
                       f"IHDR declares {length} bytes rather than 13")
    width, height, depth, colour, compression, filt, interlace = struct.unpack(
        ">IIBBBBB", data[16:29])
    if width == 0 or height == 0:
        raise PngError(REASON_HEADER_UNREADABLE,
                       f"IHDR declares a {width}x{height} image")
    if colour not in PNG_CHANNELS:
        raise PngError(REASON_COLOUR_TYPE,
                       f"colour type {colour} is not a PNG colour type")
    if compression != 0 or filt != 0:
        raise PngError(REASON_HEADER_UNREADABLE,
                       "IHDR names a compression or filter method PNG does not define")
    if interlace not in (0, 1):
        raise PngError(REASON_HEADER_UNREADABLE,
                       f"IHDR declares interlace method {interlace}")
    return PngHeader(width, height, depth, colour, interlace)


def require_decodable(header: PngHeader) -> None:
    """Refuse a header the decoder below would have to guess about."""
    if header.interlace != 0:
        raise PngError(REASON_INTERLACED, "interlaced PNG is not supported")
    if header.bit_depth != 8:
        raise PngError(REASON_BIT_DEPTH,
                       f"bit depth {header.bit_depth} is not supported; 8 only")
    if header.colour_type not in DECODABLE_COLOUR_TYPES:
        raise PngError(
            REASON_COLOUR_TYPE,
            f"colour type {header.colour_type} is not supported; "
            + ", ".join(str(t) for t in DECODABLE_COLOUR_TYPES) + " only")


def _iter_chunks(data: bytes):
    offset = 8
    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 8 + length
        if end + 4 > len(data):
            raise PngError(REASON_DATA_UNREADABLE, "a chunk runs past end of file")
        yield kind, data[offset + 8:end]
        offset = end + 4


def _unfilter(raw: bytes, width: int, height: int, channels: int) -> bytearray:
    """Reverse the five PNG scanline filters into packed 8-bit samples."""
    stride = width * channels
    if len(raw) < (stride + 1) * height:
        raise PngError(REASON_DATA_UNREADABLE,
                       "the decompressed image is shorter than IHDR describes")
    out = bytearray(stride * height)
    previous = bytearray(stride)
    position = 0
    for row in range(height):
        method = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        if method == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif method == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif method == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif method == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upleft = previous[i - channels] if i >= channels else 0
                up = previous[i]
                estimate = left + up - upleft
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upleft))
                if da <= db and da <= dc:
                    predictor = left
                elif db <= dc:
                    predictor = up
                else:
                    predictor = upleft
                line[i] = (line[i] + predictor) & 0xFF
        elif method != 0:
            raise PngError(REASON_DATA_UNREADABLE,
                           f"scanline {row} names filter {method}")
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return out


def decode_rgba(data: bytes) -> tuple[PngHeader, bytearray]:
    """Decode to tightly packed RGBA8. The header is returned alongside."""
    header = read_png_header(data)
    require_decodable(header)
    compressed = b"".join(
        payload for kind, payload in _iter_chunks(data) if kind == b"IDAT")
    if not compressed:
        raise PngError(REASON_DATA_UNREADABLE, "the file carries no IDAT data")
    try:
        raw = zlib.decompress(compressed)
    except zlib.error as error:
        raise PngError(REASON_DATA_UNREADABLE,
                       f"the image data would not inflate: {error}") from error

    channels = header.channels
    samples = _unfilter(raw, header.width, header.height, channels)
    if channels == 4:
        return header, samples

    rgba = bytearray(header.pixels * 4)
    for index in range(header.pixels):
        source = index * channels
        target = index * 4
        if channels == 1:            # greyscale
            grey = samples[source]
            rgba[target] = rgba[target + 1] = rgba[target + 2] = grey
            rgba[target + 3] = 0xFF
        elif channels == 2:          # greyscale + alpha
            grey = samples[source]
            rgba[target] = rgba[target + 1] = rgba[target + 2] = grey
            rgba[target + 3] = samples[source + 1]
        else:                        # RGB
            rgba[target:target + 3] = samples[source:source + 3]
            rgba[target + 3] = 0xFF
    return header, rgba


def encode_rgba(width: int, height: int, rgba: bytes) -> bytes:
    """Write tightly packed RGBA8 as a filter-0 PNG.

    Deliberately the simplest possible encoder. The compressed bytes depend on
    the host zlib and so are NOT reproducible across machines; nothing in the
    manifest is derived from them for exactly that reason -- the recorded
    digest of a composed texture is taken over the pixels, above this line.
    """
    if len(rgba) != width * height * 4:
        raise PngError(REASON_DATA_UNREADABLE,
                       "pixel buffer does not match the declared dimensions")

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    stride = width * 4
    scanlines = bytearray()
    for row in range(height):
        scanlines.append(0)
        scanlines += rgba[row * stride:(row + 1) * stride]
    return (PNG_SIGNATURE
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
            + chunk(b"IEND", b""))


def compose_split(rgb_png: bytes, alpha_png: bytes) -> tuple[int, int, bytearray]:
    """Join a Rice `_rgb`/`_a` pair into one RGBA image.

    Alpha is taken from the `_a` image's RED channel, not from its own alpha
    channel. That is a measurement, not a convention borrowed on faith: in the
    sample pack the `_a` files are greyscale carried in RGB (red == green ==
    blue everywhere sampled), and the ones that have an alpha channel at all
    have it uniformly opaque. Reading their alpha channel would therefore
    produce a fully opaque texture -- the exact bug this note exists to
    prevent someone from reintroducing.
    """
    rgb_header, rgb = decode_rgba(rgb_png)
    alpha_header, alpha = decode_rgba(alpha_png)
    if (rgb_header.width, rgb_header.height) != (alpha_header.width,
                                                 alpha_header.height):
        raise PngError(
            REASON_DATA_UNREADABLE,
            f"the colour half is {rgb_header.width}x{rgb_header.height} and the "
            f"coverage half is {alpha_header.width}x{alpha_header.height}")
    for index in range(rgb_header.pixels):
        rgb[index * 4 + 3] = alpha[index * 4]
    return rgb_header.width, rgb_header.height, rgb
