#!/usr/bin/env python3
"""Check a full asset-free Vita VPK before uploading it as a build artifact."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile


FILES = {
    "eboot.bin",
    "sce_sys/param.sfo",
    "sce_sys/icon0.png",
    "sce_sys/livearea/contents/bg.png",
    "sce_sys/livearea/contents/startup.png",
    "sce_sys/livearea/contents/template.xml",
    "sce_sys/trophy/GBLN00001_00/TROPHY.TRP",
}


def app_version(version: str) -> str:
    match = re.fullmatch(r"(\d{1,2})\.(\d)\.(\d)", version)
    if match is None:
        raise ValueError("Version must fit Vita APP_VER: major 0-99, minor/patch 0-9")
    major, minor, patch = map(int, match.groups())
    return f"{major:02d}.{minor}{patch}"


def sfo_strings(data: bytes) -> dict[str, str]:
    magic, _, keys, values, count = struct.unpack_from("<5I", data)
    if magic != 0x46535000 or not 20 + count * 16 <= keys <= values <= len(data):
        raise ValueError("Invalid SFO header or table bounds")
    result = {}
    for index in range(count):
        key, fmt, size, capacity, offset = struct.unpack_from("<HHIII", data, 20 + index * 16)
        key_start = keys + key
        value_start = values + offset
        if not keys <= key_start < values or size > capacity or value_start + capacity > len(data):
            raise ValueError("SFO entry outside its table")
        name = data[key_start:data.index(b"\0", key_start, values)].decode("utf-8")
        if fmt == 0x0204:
            value = data[value_start:value_start + size]
            if not value.endswith(b"\0") or name in result:
                raise ValueError("Invalid or duplicate SFO string")
            result[name] = value[:-1].decode("utf-8")
    return result


def verify(path: Path, version: str) -> dict:
    expected_version = app_version(version)
    with zipfile.ZipFile(path) as archive:
        names = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
        if len(names) != len(set(names)) or set(names) != FILES:
            raise ValueError("VPK must contain only the executable, SFO, LiveArea and trophy files")
        if archive.testzip() is not None:
            raise ValueError("VPK CRC check failed")
        if any(archive.getinfo(name).file_size == 0 for name in FILES):
            raise ValueError("VPK contains an empty required file")
        metadata = sfo_strings(archive.read("sce_sys/param.sfo"))
        if metadata.get("TITLE_ID") != "GBLN00001":
            raise ValueError("Wrong Vita TITLE_ID")
        if metadata.get("APP_VER") != expected_version:
            raise ValueError(f"Wrong APP_VER: expected {expected_version}")
    return {
        "file": path.name,
        "version": version,
        "title_id": metadata["TITLE_ID"],
        "app_ver": metadata["APP_VER"],
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "files": sorted(names),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vpk", type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        result = verify(args.vpk, args.version)
    except (OSError, ValueError, struct.error, zipfile.BadZipFile) as exc:
        parser.exit(1, f"VPK verification failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
