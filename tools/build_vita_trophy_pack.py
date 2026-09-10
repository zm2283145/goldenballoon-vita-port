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

COMM_ID = "GBLN00001_01"

TROPHIES = [
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
    # Bonus group: these deliberately have no platinum parent.
    (16, "S", -1, 1, "T.T. Time Trial Champion", "Complete every T.T. time-trial challenge."),
    (17, "G", -1, 1, "Developer Time Trial Champion", "Beat every developer time trial."),
]


def png(width: int, height: int, pixels: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


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


def xml() -> bytes:
    lines = [
        '<trophyconf version="1.1">', f' <npcommid>{COMM_ID}</npcommid>',
        ' <trophyset-version>01.00</trophyset-version>',
        ' <parental-level license-area="default">0</parental-level>',
        ' <title-name>Golden Balloon DKR</title-name>',
        ' <title-detail>Diddy Kong Racing Vita trophy set</title-detail>',
        ' <group id="001"><name>Time Trial Challenges</name><detail>Optional T.T. and developer time trials.</detail></group>',
    ]
    for tid, grade, parent, group, name, detail in TROPHIES:
        attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="{parent:03d}"'
        if parent < 0:
            attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="-1"'
        if group is not None:
            attrs += f' gid="{group:03d}"'
        lines.extend((f' <trophy {attrs}>', f'  <name>{escape(name)}</name>', f'  <detail>{escape(detail)}</detail>', ' </trophy>'))
    lines.append('</trophyconf>')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def trp(files: dict[str, bytes]) -> bytes:
    # PS Vita uses the big-endian v2 header, 0x60 bytes, with 0x40-byte TOC rows.
    header_size, entry_size = 0x60, 0x40
    entries = sorted(files.items())
    offset = header_size + len(entries) * entry_size
    table, bodies = [], []
    for name, payload in entries:
        offset = (offset + 15) & ~15
        bodies.append((offset, payload))
        table.append(name.encode('ascii').ljust(32, b'\0') + struct.pack('>QQI12x', offset, len(payload), 0))
        offset += len(payload)
    image = bytearray(offset)
    image[:header_size] = struct.pack('>IIQIII20s16x', 0xDCA24D00, 2, offset, len(entries), entry_size, 1, b'\0' * 20)
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
    files = {'TROP.SFM': xml(), 'ICON0.PNG': args.livearea_icon.read_bytes(), 'GR001.PNG': args.livearea_icon.read_bytes()}
    for tid, *_ in TROPHIES:
        files[f'TROP{tid:03d}.PNG'] = platinum.read_bytes() if tid == 0 else args.livearea_icon.read_bytes()
    args.out.write_bytes(trp(files))
    shutil.rmtree(work)
    print(f'wrote {args.out} ({args.out.stat().st_size} bytes, {len(TROPHIES)} trophies)')


if __name__ == '__main__':
    main()
