"""Build the unsigned NoTrpDrm-compatible Vita trophy archive.

The format is TRP v2 (the PS3/Vita variant).  It intentionally contains raw
TROP.SFM XML: NoTrpDrm supplies the homebrew signature bypass on the console.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import zlib
from pathlib import Path
from xml.sax.saxutils import escape

COMM_ID = "GBLN00001_00"

MAIN_TROPHIES = [
    (0, "P", -1, None, "Golden Balloon", "Earn every main-game trophy."),
    (1, "B", 0, None, "Collectibles, In a Racing Game", "Obtain your first Balloon."),
    (2, "G", 0, None, "Race Against a Giant Pig", "Collect all four pieces of the Wizpig Amulet."),
    (3, "B", 0, None, "The Ancient Key", "Find the key in Ancient Lake."),
    (4, "B", 0, None, "In the Crescent Alcove", "Find the key in Crescent Island."),
    (5, "B", 0, None, "Hidden Snowball Key", "Find the key in Snowball Valley."),
    (6, "B", 0, None, "Up the Canyon Bridge", "Find the key in Boulder Canyon."),
    (7, "S", 0, None, "Not Quite Completion", "Collect all 39 balloons in Adventure."),
    (8, "G", 0, None, "Racing, In Space!", "Collect all 47 balloons in the game."),
    (9, "S", 0, None, "Dino Domain Trophy", "Complete the Trophy Race in Dino Domain."),
    (10, "S", 0, None, "Sherbet Island Trophy", "Complete the Trophy Race in Sherbet Island."),
    (11, "S", 0, None, "Snowflake Mountain Trophy", "Complete the Trophy Race in Snowflake Mountain."),
    (12, "S", 0, None, "Dragon Forest Trophy", "Complete the Trophy Race in Dragon Forest."),
    (13, "S", 0, None, "Future Funland Trophy", "Complete the Trophy Race in Future Funland."),
    (14, "G", 0, None, "Race Against a Running Pig", "Defeat Wizpig in a race."),
    (15, "G", 0, None, "Race Against an Angry Pig. In Space!", "Win against Wizpig again."),
    (16, "B", 0, None, "Extra Speed", "Collect 10 Bananas on any track."),
    (17, "S", 0, None, "Battle Mode, Activate", "Complete the four battle challenges in each area."),
]

TRACKS = [
    ("Ancient Lake", "Car"), ("Fossil Canyon", "Car"), ("Jungle Falls", "Car"),
    ("Hot Top Volcano", "Plane"), ("Everfrost Peak", "Plane"), ("Walrus Cove", "Car"),
    ("Snowball Valley", "Car"), ("Frosty Village", "Car"), ("Whale Bay", "Hovercraft"),
    ("Crescent Island", "Car"), ("Pirate Lagoon", "Hovercraft"), ("Treasure Caves", "Car"),
    ("Windmill Plains", "Plane"), ("Greenwood Village", "Car"), ("Boulder Canyon", "Hovercraft"),
    ("Haunted Woods", "Car"), ("Spacedust Alley", "Plane"), ("Darkmoon Caverns", "Car"),
    ("Spaceport Alpha", "Plane"), ("Star City", "Car"),
]
ADVENTURE_TWO = [(18 + i, "B", -1, 1, f"Silver Coins in Mirrored {track}",
                  f"Collect all 8 Silver Coins in {track} Adventure 2, and finish first.")
                 for i, (track, _) in enumerate(TRACKS)] + [
    (38, "G", -1, 1, "Mirrored Mode", "Collect all 47 Balloons in Adventure 2."),
    (39, "S", -1, 1, "In Space AND It's Mirrored", "Win against Wizpig the second time in Adventure 2."),
]
TIME_TRIALS = [(40 + i, "S", -1, 2, f"{track} T.T.'s Challenge",
                f"Beat T.T.'s time for {track}, {vehicle} only.")
               for i, (track, vehicle) in enumerate(TRACKS)] + [
    (60 + i, "G", -1, 2, f"{track} Rare Challenge",
     f"Beat the developer's time for {track}, {vehicle} only.")
    for i, (track, vehicle) in enumerate(TRACKS)
]
CHARACTERS = ["Krunch the Kremling", "The Star of the Game", "Bumper the Badger", "Banjo the Bear",
              "Conker the Squirrel", "Tiptup the Turtle", "Pipsy the Mouse", "Timber the Tiger",
              "Drumstick the Chicken", "TT the Clock"]
MAIN_TROPHIES += [(80 + i, "B", 0, None, name, "Collect any 5 Balloons with this character in one Adventure session.")
                  for i, name in enumerate(CHARACTERS)]
MAIN_TROPHIES += [(90 + i, "B", 0, None, f"Max Power-up {name}", f"Obtain the highest leveled {name} power-up.")
                  for i, name in enumerate(("Rocket", "Boost", "Mine", "Shield", "Magnet"))]
TROPHIES = MAIN_TROPHIES + ADVENTURE_TWO + TIME_TRIALS


def png(width: int, height: int, pixels: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def decode_png_rgba(path: Path) -> tuple[int, int, bytes]:
    """Decode the small non-interlaced 8-bit PNGs used by the Vita assets."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos, payloads = 8, {}
    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        payloads[kind] = payloads.get(kind, b"") + payload
        pos += 12 + length
    width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payloads[b"IHDR"])
    if depth != 8 or compression or filtering or interlace or color not in (2, 3, 6):
        raise ValueError(f"unsupported PNG format in {path}")
    bpp = 1 if color == 3 else 3 if color == 2 else 4
    packed = zlib.decompress(payloads[b"IDAT"])
    rows, cursor, previous = [], 0, bytearray(width * bpp)
    for _ in range(height):
        filter_type, scanline = packed[cursor], bytearray(packed[cursor + 1:cursor + 1 + width * bpp])
        cursor += 1 + width * bpp
        for i, value in enumerate(scanline):
            left = scanline[i - bpp] if i >= bpp else 0
            up = previous[i]
            upper_left = previous[i - bpp] if i >= bpp else 0
            if filter_type == 1:
                scanline[i] = (value + left) & 0xff
            elif filter_type == 2:
                scanline[i] = (value + up) & 0xff
            elif filter_type == 3:
                scanline[i] = (value + ((left + up) >> 1)) & 0xff
            elif filter_type == 4:
                p, pa, pb, pc = left + up - upper_left, abs(up - upper_left), abs(left - upper_left), abs(left + up - 2 * upper_left)
                scanline[i] = (value + (left if pa <= pb and pa <= pc else up if pb <= pc else upper_left)) & 0xff
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter in {path}")
        rows.append(scanline)
        previous = scanline
    if color == 6:
        return width, height, bytes().join(rows)
    if color == 2:
        rgb = bytes().join(rows)
        pixels = bytearray(width * height * 4)
        for i in range(width * height):
            pixels[i * 4:i * 4 + 3] = rgb[i * 3:i * 3 + 3]
            pixels[i * 4 + 3] = 255
        return width, height, bytes(pixels)
    palette, alpha = payloads[b"PLTE"], payloads.get(b"tRNS", b"")
    pixels = bytearray(width * height * 4)
    for pixel_index, palette_index in enumerate(bytes().join(rows)):
        source, target = palette_index * 3, pixel_index * 4
        pixels[target:target + 4] = palette[source:source + 3] + bytes((alpha[palette_index] if palette_index < len(alpha) else 255,))
    return width, height, bytes(pixels)


def resize_png(path: Path, width: int, height: int) -> bytes:
    source_width, source_height, source = decode_png_rgba(path)
    output = bytearray(width * height * 4)
    for y in range(height):
        source_y = y * source_height // height
        for x in range(width):
            source_x = x * source_width // width
            target = (y * width + x) * 4
            start = (source_y * source_width + source_x) * 4
            output[target:target + 4] = source[start:start + 4]
    return png(width, height, bytes(output))


def platinum_icon(path: Path) -> None:
    size = 240
    data = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            dx, dy = x - 119.5, y - 119.5
            radius = (dx * dx + dy * dy) ** 0.5
            offset = (y * size + x) * 4
            if radius < 107:
                edge = max(0, min(255, int((107 - radius) * 14)))
                data[offset:offset + 4] = bytes((90 + edge // 3, 178 + edge // 4, 230 + edge // 10, 255))
            if abs(dx) < 20 and abs(dy) < 82 or abs(dy) < 20 and abs(dx) < 82:
                data[offset:offset + 4] = bytes((244, 209, 71, 255))
    path.write_bytes(png(size, size, bytes(data)))


def xml(configuration_only: bool = False) -> bytes:
    # NoTrpDrm recognizes the conventional 160-byte development placeholder
    # (encoded here as 320 literal `x` characters). This exact representation
    # is used by working unsigned Vita trophy packs; a fabricated hex record
    # is still parsed as a real signature and is rejected by the setup dialog.
    signature = "x" * 320
    lines = [
        f'<!--Sce-Np-Trophy-Signature: {signature}-->',
        '<trophyconf version="1.1" platform="psp2" policy="large">',
        f' <npcommid>{COMM_ID}</npcommid>',
        ' <trophyset-version>01.02</trophyset-version>',
        ' <parental-level license-area="default">0</parental-level>',
    ]
    if not configuration_only:
        lines.extend((
            ' <title-name>Diddy Kong Racing</title-name>',
            ' <title-detail>Diddy Kong Racing is a kart racing adventure game developed by Rare and released for the Nintendo 64 in 1997.</title-detail>',
        ))
    if configuration_only:
        lines.extend((' <group id="001"/>', ' <group id="002"/>'))
    else:
        lines.extend((
            ' <group id="001">',
            '  <name>Adventure 2</name>',
            '  <detail>Optional mirrored Adventure 2 challenges. These trophies are not required for the platinum.</detail>',
            ' </group>',
            ' <group id="002">',
            '  <name>Time Trial Challenges</name>',
            '  <detail>Optional T.T. and developer time-trial challenges. These trophies are not required for the platinum.</detail>',
            ' </group>',
        ))
    for tid, grade, parent, group, name, detail in TROPHIES:
        attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="{parent:03d}"'
        if parent < 0:
            attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="-1"'
        if group is not None:
            attrs += f' gid="{group:03d}"'
        if configuration_only:
            lines.append(f' <trophy {attrs}/>')
        else:
            lines.extend((f' <trophy {attrs}>', f'  <name>{escape(name)}</name>', f'  <detail>{escape(detail)}</detail>', ' </trophy>'))
    lines.append('</trophyconf>')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def trp(files: dict[str, bytes]) -> bytes:
    # PS Vita uses the big-endian v2 header, 0x40 bytes, with 0x40-byte TOC rows.
    # The final 16 bytes of the packed header above are reserved; they are part
    # of the header, not a prefix before the table of contents.
    header_size, entry_size = 0x40, 0x40
    # Keep the conventional Vita order: the compact configuration manifest
    # must lead the archive, immediately followed by its localized metadata.
    # Working NoTrpDrm packs use this order; alphabetical sorting would put
    # TROPCONF.SFM last after the image assets.
    entries = list(files.items())
    offset = header_size + len(entries) * entry_size
    table, bodies = [], []
    for name, payload in entries:
        offset = (offset + 15) & ~15
        bodies.append((offset, payload))
        table.append(name.encode('ascii').ljust(32, b'\0') + struct.pack('>QQI12x', offset, len(payload), 0))
        offset += len(payload)
    image = bytearray(offset)
    image[:header_size] = struct.pack('>IIQIII20s16x', 0xDCA24D00, 2, offset, len(entries), entry_size, 0, b'\0' * 20)
    image[header_size:header_size + len(entries) * entry_size] = b''.join(table)
    for position, payload in bodies:
        image[position:position + len(payload)] = payload
    digest = hashlib.sha1(image).digest()
    image[0x1c:0x30] = digest
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--livearea-icon', type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    work = args.out.parent / 'generated_trophy_assets'
    work.mkdir(exist_ok=True)
    platinum = work / 'TROP000.PNG'
    platinum_icon(platinum)
    # Vita's setup dialog first consumes the compact configuration manifest,
    # then reads the localized trophy metadata. Without TROPCONF.SFM it reports
    # NP-6182-7 even when the archive itself is present at the correct path.
    files = {
        'TROPCONF.SFM': xml(configuration_only=True),
        'TROP.SFM': xml(),
        # Trophy assets have fixed Vita dimensions: title/group images are
        # 320x176 and individual trophy images are 240x240. LiveArea's icon
        # is only 128x128, so never insert it into the archive verbatim.
        'ICON0.PNG': resize_png(args.livearea_icon, 320, 176),
        # Group art is sourced from the authorized RetroAchievements badges.
        'GR001.PNG': resize_png(Path(__file__).parent.parent / 'vita' / 'trophy' / 'adventure2.png', 320, 176),
        'GR002.PNG': resize_png(Path(__file__).parent.parent / 'vita' / 'trophy' / 'time_trials.png', 320, 176),
    }
    for tid, *_ in TROPHIES:
        files[f'TROP{tid:03d}.PNG'] = platinum.read_bytes() if tid == 0 else resize_png(args.livearea_icon, 240, 240)
    args.out.write_bytes(trp(files))
    shutil.rmtree(work)
    print(f'wrote {args.out} ({args.out.stat().st_size} bytes, {len(TROPHIES)} trophies, {COMM_ID})')


if __name__ == '__main__':
    main()
