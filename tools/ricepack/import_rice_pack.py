#!/usr/bin/env python3
"""Convert a Rice/GLideN64 high-resolution texture pack into a content pack.

    tools/ricepack/import_rice_pack.py plan  <rice-pack-dir> --manifest out.json
    tools/ricepack/import_rice_pack.py build <rice-pack-dir> --out <pack-dir> \
        --crosswalk crosswalk.json --manifest out.json
    tools/ricepack/import_rice_pack.py crosswalk <dump-dir> --out crosswalk.json

The gap this tool does not close
--------------------------------
A Rice pack names a texture by a CRC-32 over the raw N64 texel bytes. This port
names one by the content digest in platform/mod_texture_key.h -- a SHA-256 over
the same raw texel bytes plus the format fields, truncated to 32 hex
characters. Both name the same picture; neither can be computed from the other.
Converting one to the other needs the bytes, and the bytes come from a ROM that
this repository does not have and will never contain.

So `plan` deliberately maps nothing. It reads a pack, decides what every single
file in it is, and writes an auditable record of those decisions -- which is
the part that can be got right offline, and the part that is worth reviewing
before any pixel is copied anywhere. `build` needs a crosswalk (Rice key ->
content digest) produced from a dump of a running game; `crosswalk` builds one
from that dump. Without it, coverage is honestly reported as zero rather than
quietly reported as something else.

The crosswalk reads a MDKR_MOD_TEXTURE_DUMP corpus at dump format 2 or later:
`<digest>.png`, `<digest>.txt` and the `<digest>.texels` companion holding the
exact raw span the engine hashed. The span is the whole point -- the decoded
PNG cannot stand in for it, because the decode is lossy in the direction that
matters and a CRC over decoded pixels is a CRC of a different picture. An
older corpus, with no `.texels` files, is reported as skipped per texture
rather than mapped to nothing.

Whether the CRC ITSELF is Rice's remains unproven; see rice_crc.py, and
tools/ricepack/measure_crc_variants.py for the experiment that settles it.

The decision order, which is the whole design
---------------------------------------------
Per file, each stage refusing before the next one runs, so a manifest can say
which stage refused it:

    1. filename allowlist        (rice_names.py)
    2. ROM name agreement
    3. PNG header and, for a half of a split pair, decodability
    4. size caps and the expansion ceiling
    5. the (fmt, siz) pair being one the RDP defines

Then per texture key, over the files that survived:

    6. an `_all` present anywhere in the pack wins, and every `_rgb`/`_a` for
       that key is skipped with a reason. Directory position is not consulted:
       the sample pack has halves sitting three directories away from the
       `_all` that beats them, so a per-directory or first-seen rule would give
       a different answer depending on where a walk started. Nothing here may
       depend on load order.
    7. otherwise `_rgb` + `_a` together are composed into one RGBA texture.
    8. otherwise a lone `_rgb` is accepted as opaque ONLY when the format it
       replaces has no alpha at all; a lone `_a` is always refused.
    9. two files claiming the same key and the same part are BOTH refused. A
       tiebreak by path would be a silent choice between two authors' files;
       there is no principled winner, so the pack has to say which it meant.

Nothing here copies pack content into this repository, and nothing here needs
a ROM.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import png_probe                                              # noqa: E402
import rice_crc                                               # noqa: E402
import rice_names                                             # noqa: E402


ROOT = Path(__file__).resolve().parents[2]
DIGEST_HEADER = ROOT / "platform" / "mod_texture_key.h"

IMPORTER_NAME = "ricepack"
IMPORTER_VERSION = "0.2.0"
MANIFEST_SCHEMA_VERSION = 1
CROSSWALK_SCHEMA_VERSION = 1

# The `<digest>.txt` dump record this reads (MDKR_MOD_TEXTURE_DUMP_FORMAT in
# platform/mod_texture_store.h).
#
# 1 -- width, height, fmt, siz, first_seen, and no `dump_format` line at all.
#      A record with no version IS version 1; that is what keeps a corpus
#      dumped before the field existed readable here.
# 2 -- adds source_width/source_height/source_line_bytes/source_size_bytes and
#      the `<digest>.texels` companion holding the raw hashed span. Only a
#      version-2 record can produce a Rice CRC.
DUMP_FORMAT_UNVERSIONED = 1
DUMP_FORMAT_SUPPORTED = 2

# A content-pack texture is named by 32 lower-case hex characters; see
# docs/MODDING.md. Anything else in a crosswalk is a corrupt crosswalk.
CONTENT_DIGEST_RE = re.compile(r"^[0-9a-f]{32}$")

# --- caps ------------------------------------------------------------------
#
# Every number below admits the sample pack with room to spare and was chosen
# against its measured extremes, not picked round: 1983x1600 largest image,
# 2.7 MiB largest file, 133 MiB of source, 315 MiB once expanded, 195:1 worst
# expansion. A cap nobody has ever been near is a cap nobody has tested.
MAX_TEXTURE_DIMENSION = 4096
MAX_TEXTURE_PIXELS = 4096 * 4096
MAX_TEXTURE_SOURCE_BYTES = 16 * 1024 * 1024
MAX_TEXTURE_DECODED_BYTES = 64 * 1024 * 1024
MAX_PACK_SOURCE_BYTES = 512 * 1024 * 1024
# Matches the runtime's own documented ceiling on decoded pack textures
# (docs/MODDING.md, "Limits"). Past it the game starts evicting and re-decoding,
# so a pack that cannot fit is worth saying so at import rather than at play.
MAX_PACK_DECODED_BYTES = 512 * 1024 * 1024
# The project's published intake ceiling for expanding untrusted archives, used
# here for the same reason: expanded bytes must stay proportional to the bytes
# that were actually supplied, with a floor so small well-compressed images are
# not mistaken for bombs.
EXPANSION_RATIO_LIMIT = 200
EXPANSION_FLOOR_BYTES = 1024 * 1024

MAX_PACK_NAME_CHARS = 63
MAX_PACK_AUTHOR_CHARS = 63
MAX_PACK_VERSION_CHARS = 31

VERDICT_SELECTED = "selected"
VERDICT_SKIPPED = "skipped"
VERDICT_REJECTED = "rejected"

REASON_ROM_MISMATCH = "rom-name-mismatch"
REASON_UNDEFINED_FORMAT = "rdp-format-undefined"
REASON_DIMENSION_CAP = "cap-dimension"
REASON_PIXEL_CAP = "cap-pixels"
REASON_SOURCE_BYTES_CAP = "cap-source-bytes"
REASON_DECODED_BYTES_CAP = "cap-decoded-bytes"
REASON_EXPANSION_CAP = "cap-expansion-ratio"
REASON_PACK_SOURCE_CAP = "cap-pack-source-bytes"
REASON_PACK_DECODED_CAP = "cap-pack-decoded-bytes"
REASON_DUPLICATE = "duplicate-key-part"
REASON_SHADOWED = "shadowed-by-all"
REASON_ORPHAN_ALPHA = "orphan-coverage-half"
REASON_ORPHAN_RGB_ALPHA_FORMAT = "orphan-colour-half-format-has-alpha"
REASON_ORPHAN_RGB_LUMINANCE = "orphan-colour-half-alpha-is-luminance"
REASON_SELECTED_ALL = "complete-image"
REASON_SELECTED_PAIR = "colour-and-coverage-pair"
REASON_SELECTED_OPAQUE = "colour-half-opaque-format"

# Cheap read: a manifest that hard-coded the digest version would keep saying
# "1" through a bump, and the packs it produced would be wrong in a way nothing
# reported. Read the constant from the contract it belongs to instead.
DIGEST_VERSION_RE = re.compile(
    r"^#define\s+MDKR_MOD_TEXTURE_DIGEST_VERSION\s+(\d+)u?\s*$", re.MULTILINE)


def read_digest_contract_version(header: Path = DIGEST_HEADER) -> int:
    text = header.read_text(encoding="utf-8")
    match = DIGEST_VERSION_RE.search(text)
    if match is None:
        raise SystemExit(
            f"FAIL: MDKR_MOD_TEXTURE_DIGEST_VERSION not found in {header}")
    return int(match.group(1))


@dataclass
class FileRecord:
    """One input file and everything decided about it."""

    path: str                       # POSIX, relative to the pack root
    size_bytes: int
    sha256: str
    name: rice_names.RiceName | None = None
    header: png_probe.PngHeader | None = None
    verdict: str = VERDICT_REJECTED
    reason: str = ""
    detail: str = ""

    @property
    def key(self) -> str | None:
        return self.name.key if self.name is not None else None

    def refuse(self, reason: str, detail: str) -> None:
        self.verdict = VERDICT_REJECTED
        self.reason = reason
        self.detail = detail

    def to_json(self) -> dict:
        entry: dict = {
            "path": self.path,
            "sha256": self.sha256,
            "sourceBytes": self.size_bytes,
            "decision": self.verdict,
            "reason": self.reason,
            "detail": self.detail,
        }
        if self.name is not None:
            entry["key"] = self.name.key
            entry["part"] = self.name.part
            entry["format"] = self.name.format_label
        if self.header is not None:
            entry["width"] = self.header.width
            entry["height"] = self.header.height
            entry["colourType"] = self.header.colour_type
            entry["bitDepth"] = self.header.bit_depth
            entry["decodedBytes"] = self.header.decoded_bytes
        return entry


@dataclass
class KeyRecord:
    """One texture key and how it was resolved."""

    key: str
    fmt: int
    siz: int
    format_label: str
    verdict: str = VERDICT_REJECTED
    reason: str = ""
    detail: str = ""
    sources: list[str] = field(default_factory=list)
    content_digest: str | None = None

    def to_json(self) -> dict:
        entry: dict = {
            "key": self.key,
            "format": self.format_label,
            "decision": self.verdict,
            "reason": self.reason,
            "detail": self.detail,
            "sources": sorted(self.sources),
        }
        if self.content_digest is not None:
            entry["contentDigest"] = self.content_digest
        return entry


@dataclass
class ScanResult:
    files: list[FileRecord]
    keys: list[KeyRecord]
    rom_names: list[str]
    root_name: str
    total_source_bytes: int
    total_decoded_bytes: int


def sha256_file(path: Path) -> tuple[str, int, bytes]:
    """Digest and size, plus the leading bytes the header probe needs."""
    digest = hashlib.sha256()
    head = b""
    size = 0
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            if not head:
                head = block[:64]
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size, head


def scan_pack(root: Path, rom_name: str | None = None) -> ScanResult:
    """Stages 1-9 of the decision order in the module docstring."""
    files: list[FileRecord] = []

    # os.walk order is filesystem order. Sorting here is what makes every later
    # stage -- and therefore the manifest -- independent of it.
    for path in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        if not path.is_file() or path.is_symlink():
            continue
        digest, size, head = sha256_file(path)
        record = FileRecord(path.relative_to(root).as_posix(), size, digest)
        files.append(record)

        # Best-effort, before any decision: a manifest is more useful when it
        # can say how big the image somebody misnamed actually was. This never
        # changes a verdict -- the refusal below still comes from the stage
        # that owns it -- so a file with a readable header and an unreadable
        # name is still refused for its name.
        try:
            record.header = png_probe.read_png_header(head)
        except png_probe.PngError:
            record.header = None

        verdict = rice_names.classify_filename(path.name)
        if verdict.name is None:
            record.refuse(verdict.reason or "", verdict.detail or "")
            continue
        record.name = verdict.name

        if rom_name is not None and verdict.name.rom_name != rom_name:
            record.refuse(
                REASON_ROM_MISMATCH,
                f"names ROM '{verdict.name.rom_name}', not '{rom_name}'")
            continue

        # A well-formed IHDR is inside the first 33 bytes, so the head the
        # digest pass already held is normally enough. Anything that failed on
        # it is re-read in full, so a refusal is never an artefact of how much
        # of the file was looked at.
        header = record.header
        if header is None:
            try:
                header = png_probe.read_png_header(path.read_bytes())
            except png_probe.PngError as error:
                record.refuse(error.reason, error.detail)
                continue
            record.header = header

        # Only the halves of a split pair are ever decompressed by this tool;
        # an `_all` file is copied through byte for byte and is read by the
        # game's own decoder, which accepts more than png_probe does. Applying
        # the decoder's limits to it would refuse files that work.
        if verdict.name.part in ("rgb", "a"):
            try:
                png_probe.require_decodable(header)
            except png_probe.PngError as error:
                record.refuse(error.reason, error.detail)
                continue

        capped = check_caps(record, header)
        if capped is not None:
            record.refuse(*capped)
            continue

        if verdict.name.alpha_class is None:
            record.refuse(
                REASON_UNDEFINED_FORMAT,
                f"fmt {verdict.name.fmt} / siz {verdict.name.siz} is not a "
                "format the RDP defines")
            continue

        record.verdict = VERDICT_SELECTED
        record.reason = ""

    keys = resolve_keys(files)
    enforce_pack_caps(files, keys)

    rom_names = sorted({f.name.rom_name for f in files if f.name is not None})
    total_source = sum(f.size_bytes for f in files
                       if f.verdict == VERDICT_SELECTED)
    total_decoded = sum(f.header.decoded_bytes for f in files
                        if f.verdict == VERDICT_SELECTED and f.header is not None)
    return ScanResult(files, keys, rom_names, root.name, total_source, total_decoded)


def check_caps(record: FileRecord,
               header: png_probe.PngHeader) -> tuple[str, str] | None:
    """Per-file limits, including the decompression-bomb ceiling."""
    if header.width > MAX_TEXTURE_DIMENSION or header.height > MAX_TEXTURE_DIMENSION:
        return (REASON_DIMENSION_CAP,
                f"{header.width}x{header.height} exceeds the "
                f"{MAX_TEXTURE_DIMENSION}px per-side limit")
    if header.pixels > MAX_TEXTURE_PIXELS:
        return (REASON_PIXEL_CAP,
                f"{header.pixels} pixels exceeds the {MAX_TEXTURE_PIXELS} limit")
    if record.size_bytes > MAX_TEXTURE_SOURCE_BYTES:
        return (REASON_SOURCE_BYTES_CAP,
                f"{record.size_bytes} bytes exceeds the "
                f"{MAX_TEXTURE_SOURCE_BYTES}-byte per-file limit")
    if header.decoded_bytes > MAX_TEXTURE_DECODED_BYTES:
        return (REASON_DECODED_BYTES_CAP,
                f"{header.decoded_bytes} decoded bytes exceeds the "
                f"{MAX_TEXTURE_DECODED_BYTES}-byte limit")
    # The bomb test. Read from the header, so it refuses before anything is
    # inflated -- a file that only reveals its size by being decompressed has
    # already cost what the cap exists to prevent.
    ceiling = EXPANSION_RATIO_LIMIT * record.size_bytes + EXPANSION_FLOOR_BYTES
    if header.decoded_bytes > ceiling:
        return (REASON_EXPANSION_CAP,
                f"{header.decoded_bytes} decoded bytes from {record.size_bytes} "
                f"stored exceeds {EXPANSION_RATIO_LIMIT}x + "
                f"{EXPANSION_FLOOR_BYTES}")
    return None


def enforce_pack_caps(files: list[FileRecord], keys: list[KeyRecord]) -> None:
    """Whole-pack totals, applied after per-file decisions.

    Deterministic by construction: files are walked in sorted path order and
    the first one that crosses a total is the one refused, so the same pack
    always loses the same files rather than whichever the filesystem returned
    last.
    """
    by_key = {record.key: record for record in keys}
    source_total = 0
    decoded_total = 0
    for record in files:
        if record.verdict != VERDICT_SELECTED or record.header is None:
            continue
        if source_total + record.size_bytes > MAX_PACK_SOURCE_BYTES:
            record.refuse(
                REASON_PACK_SOURCE_CAP,
                f"the pack passes {MAX_PACK_SOURCE_BYTES} source bytes here")
        elif decoded_total + record.header.decoded_bytes > MAX_PACK_DECODED_BYTES:
            record.refuse(
                REASON_PACK_DECODED_CAP,
                f"the pack passes {MAX_PACK_DECODED_BYTES} decoded bytes here")
        else:
            source_total += record.size_bytes
            decoded_total += record.header.decoded_bytes
            continue
        key_record = by_key.get(record.key or "")
        if key_record is None or key_record.verdict == VERDICT_REJECTED:
            continue
        key_record.verdict = VERDICT_REJECTED
        key_record.reason = record.reason
        key_record.detail = record.detail
        # The other half of a pair is now unusable too. Leaving it marked
        # selected would put a file in the manifest that claims to be in a pack
        # it is not in, which is the kind of small lie an audit record cannot
        # afford.
        for sibling in files:
            if (sibling is not record and sibling.key == key_record.key
                    and sibling.verdict == VERDICT_SELECTED):
                sibling.verdict = VERDICT_SKIPPED
                sibling.reason = key_record.reason
                sibling.detail = ("the other half of this texture crossed a "
                                  "whole-pack limit, so neither is used")


def resolve_keys(files: list[FileRecord]) -> list[KeyRecord]:
    """Stages 6-9: decide which files supply each texture key."""
    grouped: dict[str, dict[str, list[FileRecord]]] = defaultdict(
        lambda: defaultdict(list))
    for record in files:
        if record.verdict != VERDICT_SELECTED or record.name is None:
            continue
        grouped[record.name.key][record.name.part].append(record)

    keys: list[KeyRecord] = []
    for key in sorted(grouped):
        parts = grouped[key]
        sample = next(iter(next(iter(parts.values()))))
        assert sample.name is not None
        entry = KeyRecord(key, sample.name.fmt, sample.name.siz,
                          sample.name.format_label)
        keys.append(entry)

        duplicated = sorted(part for part, records in parts.items()
                            if len(records) > 1)
        if duplicated:
            # No principled winner exists. Picking one by path would be a
            # silent choice between two authors' files that reads as a bug the
            # first time someone notices only one of them took effect.
            for part in duplicated:
                competitors = sorted(r.path for r in parts[part])
                for record in parts[part]:
                    record.refuse(
                        REASON_DUPLICATE,
                        f"_{part} for this texture is supplied by "
                        + " and ".join(competitors))
            entry.verdict = VERDICT_REJECTED
            entry.reason = REASON_DUPLICATE
            entry.detail = ("more than one file claims the same part of this "
                            "texture; the pack has to say which")
            entry.sources = [r.path for records in parts.values() for r in records]
            continue

        entry.sources = [r.path for records in parts.values() for r in records]

        if "all" in parts:
            for part in ("rgb", "a"):
                for record in parts.get(part, []):
                    record.verdict = VERDICT_SKIPPED
                    record.reason = REASON_SHADOWED
                    record.detail = (
                        f"{parts['all'][0].path} supplies the whole image for "
                        "this texture")
            entry.verdict = VERDICT_SELECTED
            entry.reason = REASON_SELECTED_ALL
            entry.detail = f"taken from {parts['all'][0].path}"
            continue

        if "rgb" in parts and "a" in parts:
            entry.verdict = VERDICT_SELECTED
            entry.reason = REASON_SELECTED_PAIR
            entry.detail = (f"composed from {parts['rgb'][0].path} and "
                            f"{parts['a'][0].path}")
            continue

        if "a" in parts:
            for record in parts["a"]:
                record.refuse(
                    REASON_ORPHAN_ALPHA,
                    "coverage was supplied without the colour it belongs to")
            entry.verdict = VERDICT_REJECTED
            entry.reason = REASON_ORPHAN_ALPHA
            entry.detail = "only the coverage half of this texture is present"
            continue

        alpha_class = sample.name.alpha_class
        if alpha_class == rice_names.ALPHA_NONE:
            entry.verdict = VERDICT_SELECTED
            entry.reason = REASON_SELECTED_OPAQUE
            entry.detail = (f"{entry.format_label} carries no alpha, so "
                            f"{parts['rgb'][0].path} is complete on its own")
            continue

        if alpha_class == rice_names.ALPHA_LUMINANCE:
            reason = REASON_ORPHAN_RGB_LUMINANCE
            detail = (f"{entry.format_label} has no separate alpha, but the "
                      "port's decoder sets alpha from intensity; treating this "
                      "half as opaque would change coverage the game reads. "
                      "Supply the _a half, or an _all image.")
        else:
            reason = REASON_ORPHAN_RGB_ALPHA_FORMAT
            detail = (f"{entry.format_label} carries alpha this colour-only "
                      "half cannot supply")
        for record in parts["rgb"]:
            record.refuse(reason, detail)
        entry.verdict = VERDICT_REJECTED
        entry.reason = reason
        entry.detail = detail

    return keys


# --- crosswalk -------------------------------------------------------------


def load_crosswalk(path: Path, variant: str) -> dict[str, str]:
    """Rice key -> content digest, for one CRC variant."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schemaVersion") != CROSSWALK_SCHEMA_VERSION:
        raise SystemExit(
            f"FAIL: {path} is crosswalk schema "
            f"{data.get('schemaVersion')}, not {CROSSWALK_SCHEMA_VERSION}")
    variants = data.get("variants", {})
    if variant not in variants:
        raise SystemExit(
            f"FAIL: {path} carries no '{variant}' table; it has "
            + ", ".join(sorted(variants)))
    table = variants[variant]
    for key, digest in table.items():
        if not CONTENT_DIGEST_RE.match(str(digest)):
            raise SystemExit(
                f"FAIL: {path} maps {key} to '{digest}', which is not a "
                "32-character lower-case content digest")
    return dict(table)


def parse_dump_record(text: str) -> dict[str, str]:
    """`key=value` lines, as mod_texture_store.c writes them."""
    fields: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            name, _, value = line.partition("=")
            fields[name.strip()] = value.strip()
    return fields


def dump_record_geometry(fields: dict[str, str]) -> tuple[int, int, int, int, int | None]:
    """(fmt, siz, width, height, pitch) for the SOURCE texels of one record.

    Two versions of the same record are read here, and the difference is not
    cosmetic. A version-2 record states the source tile's geometry outright;
    a version-1 one has only `width`/`height`, which describe the PICTURE THAT
    WAS DUMPED -- for a run with a pack installed, the replacement PNG's own
    size, which has nothing to do with the ROM bytes. The fallback is therefore
    correct only for a dump taken with no pack installed, which is the only way
    a version-1 corpus could carry a span at all, so nothing is lost by it.

    Raises KeyError or ValueError; the caller turns either into a skip.
    """
    fmt = int(fields["fmt"])
    siz = int(fields["siz"])
    width = int(fields.get("source_width") or fields["width"])
    height = int(fields.get("source_height") or fields["height"])
    pitch = int(fields.get("source_line_bytes", "0")) or None
    return fmt, siz, width, height, pitch


def build_crosswalk(dump_dir: Path) -> dict:
    """Compute every candidate Rice CRC for a dumped texture corpus.

    The dump this reads is the widened one: `<digest>.txt` carrying the source
    geometry and `<digest>.texels` carrying the raw span the engine hashed.
    A corpus dumped before that existed has no span, and every texture in it is
    reported as skipped with that reason rather than silently contributing
    nothing.

    Nothing here decides which CRC variant is right. Every variant gets its own
    table over the same bytes, in one pass, so the hit-rate sweep that does
    decide needs one dump rather than four.
    """
    variants: dict[str, dict[str, str]] = {name: {} for name in rice_crc.VARIANTS}
    # Two dumped textures landing on one Rice key. For the right algorithm this
    # is a birthday event across a few thousand 32-bit values and should be 0 or
    # 1; for a degenerate variant it is the tell -- a table that collapses many
    # textures onto few keys can score hits without knowing anything. Counted
    # rather than resolved, because there is no basis for preferring either
    # claimant, and the last writer winning is at least deterministic given a
    # sorted walk.
    collisions: dict[str, int] = {name: 0 for name in rice_crc.VARIANTS}
    skipped: list[dict[str, str]] = []
    considered = 0
    formats: set[int] = set()

    for txt in sorted(dump_dir.glob("*.txt")):
        digest = txt.stem
        considered += 1
        if not CONTENT_DIGEST_RE.match(digest):
            skipped.append({"digest": digest, "reason": "not-a-content-digest"})
            continue
        fields = parse_dump_record(txt.read_text(encoding="utf-8"))
        try:
            record_format = int(fields.get("dump_format", DUMP_FORMAT_UNVERSIONED))
        except ValueError:
            skipped.append({"digest": digest, "reason": "dump-record-unreadable"})
            continue
        formats.add(record_format)
        if record_format > DUMP_FORMAT_SUPPORTED:
            # Refused rather than read hopefully. A later format may reinterpret
            # a field this one thinks it understands, and a crosswalk built on
            # that guess would be wrong in a way no downstream check could see.
            skipped.append({
                "digest": digest,
                "reason": f"dump-format-unsupported: {record_format}"})
            continue
        texels = txt.with_suffix(".texels")
        if not texels.is_file():
            skipped.append({"digest": digest, "reason": "dump-lacks-texel-span"})
            continue
        try:
            fmt, siz, width, height, pitch = dump_record_geometry(fields)
        except (KeyError, ValueError):
            skipped.append({"digest": digest, "reason": "dump-record-unreadable"})
            continue
        if width <= 0 or height <= 0:
            # A tile whose geometry the engine could not resolve. Refused
            # rather than hashed, because rice_crc walks zero rows for it and
            # returns the SAME constant for every such texture -- 00000000 in
            # three of the four variants. Those constants would land in the
            # table, collide with each other, and could even match a pack file
            # that happens to be named 00000000, which is a hit that measures
            # nothing at all.
            skipped.append({"digest": digest, "reason": "dump-geometry-degenerate"})
            continue
        try:
            candidates = rice_crc.all_variants(
                texels.read_bytes(), width, height, siz, pitch)
        except ValueError as error:
            # Overwhelmingly: the span was clamped at the edge of the game's
            # arena and no longer covers the geometry the tile declared
            # (compare source_texel_bytes against source_size_bytes in the
            # record). Refused loudly, because a CRC over fewer rows than the
            # emulator hashed is a plausible number that can only ever miss.
            skipped.append({"digest": digest, "reason": f"crc-input-rejected: {error}"})
            continue
        for name, value in candidates.items():
            key = f"{value}#{fmt}#{siz}#"
            existing = variants[name].get(key)
            if existing is not None and existing != digest:
                collisions[name] += 1
            variants[name][key] = digest

    return {
        "schemaVersion": CROSSWALK_SCHEMA_VERSION,
        "producer": f"{IMPORTER_NAME} {IMPORTER_VERSION}",
        "crcStatus": "unvalidated",
        "digestContractVersion": read_digest_contract_version(),
        "dumpFormatsRead": sorted(formats),
        "keyCollisions": collisions,
        "dumpFormatSupported": DUMP_FORMAT_SUPPORTED,
        "texturesConsidered": considered,
        "texturesMapped": len(variants[rice_crc.DEFAULT_VARIANT]),
        "skipped": sorted(skipped, key=lambda entry: entry["digest"]),
        "variants": variants,
    }


# --- manifest --------------------------------------------------------------


def pack_content_digest(files: list[FileRecord]) -> str:
    """An identity for the source pack, since Rice packs declare no version.

    Covers every file the walk saw, junk included: two directories that differ
    only by a stray thumbnail database are not the same pack, and a manifest
    that said they were would be worse than having no identity at all.
    """
    digest = hashlib.sha256()
    for record in sorted(files, key=lambda r: r.path):
        digest.update(record.path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(record.sha256.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def coverage_report(keys: list[KeyRecord], files: list[FileRecord],
                    mapped: int) -> dict:
    """N of M, and never a claim of completeness that is not one.

    `complete` is true only when every key was mappable AND every key was
    mapped AND no input was refused. A partial import is the normal outcome --
    a pack covers part of a game and a crosswalk covers part of a pack -- so
    the field exists to be false, and the statement spells out why.
    """
    discovered = len(keys)
    mappable = sum(1 for key in keys if key.verdict == VERDICT_SELECTED)
    refused_files = sum(1 for f in files if f.verdict == VERDICT_REJECTED)
    complete = (discovered > 0 and mapped == discovered
                and mappable == discovered and refused_files == 0)
    percent = (100.0 * mapped / mappable) if mappable else 0.0
    # A key carrying a palette CRC cannot be mapped by construction, not by
    # circumstance: build_crosswalk() emits `CRC#fmt#siz#` with the palette
    # component always empty, while RiceName.key fills it in whenever the
    # filename has one, so the two can never meet. Counting those inside the
    # ordinary "no content digest in the crosswalk" line would tell an author
    # their dump route never visited the texture, and they would widen the
    # route forever chasing coverage that is unreachable. Say which it is.
    palette_keyed = sum(1 for key in keys
                        if key.verdict == VERDICT_SELECTED
                        and key.key.rsplit("#", 1)[-1] != "")
    reasons = []
    if mapped < mappable:
        unmapped = mappable - mapped
        if palette_keyed:
            reasons.append(
                f"{unmapped} mappable key(s) have no content digest in the "
                f"crosswalk, of which {palette_keyed} carry a palette CRC and "
                "are unmappable by construction (the crosswalk emits no "
                "palette component; widening the dump route cannot reach them)")
        else:
            reasons.append(f"{unmapped} mappable key(s) have no content "
                           "digest in the crosswalk")
    if mappable < discovered:
        reasons.append(f"{discovered - mappable} key(s) were refused")
    if refused_files:
        reasons.append(f"{refused_files} input file(s) were refused")
    statement = (f"{mapped} of {mappable} mappable key(s) mapped "
                 f"({percent:.1f}%), from {discovered} key(s) discovered")
    if reasons:
        statement += "; coverage is partial because " + "; ".join(reasons)
    return {
        "keysDiscovered": discovered,
        "keysMappable": mappable,
        "keysMapped": mapped,
        "keysUnmapped": mappable - mapped,
        "keysPaletteKeyedUnmappable": palette_keyed,
        "filesRefused": refused_files,
        "percentOfMappable": round(percent, 4),
        "complete": complete,
        "statement": statement,
    }


def build_manifest(scan: ScanResult, mapped: int, crc_variant: str,
                   source_version: str | None,
                   crosswalk_name: str | None) -> dict:
    """The audit record. Byte-identical for the same pack, on any machine.

    Two rules keep that true. Every path is relative to the pack root and
    written POSIX-style, so a manifest carries no absolute path (which would
    also make it unfit to commit). And nothing is derived from the clock, the
    walk order, or the host's zlib.
    """
    by_reason = Counter(f"{f.verdict}:{f.reason}" if f.reason else f.verdict
                        for f in scan.files)
    return {
        "schemaVersion": MANIFEST_SCHEMA_VERSION,
        "importer": {"name": IMPORTER_NAME, "version": IMPORTER_VERSION},
        "digestContract": {
            "version": read_digest_contract_version(),
            "definedBy": "platform/mod_texture_key.h",
        },
        "riceCrc": {
            "variant": crc_variant,
            "status": "unvalidated",
            "note": "no output of tools/ricepack/rice_crc.py has been compared "
                    "against an emulator; see that module for the experiment "
                    "that would settle it",
        },
        "sourcePack": {
            "rootName": scan.root_name,
            "declaredVersion": source_version,
            "declaredVersionNote": (
                "supplied by --source-pack-version" if source_version
                else "Rice packs carry no version field and none was supplied"),
            "contentDigest": pack_content_digest(scan.files),
            "romNames": scan.rom_names,
            "fileCount": len(scan.files),
            "selectedSourceBytes": scan.total_source_bytes,
            "selectedDecodedBytes": scan.total_decoded_bytes,
        },
        "crosswalk": crosswalk_name,
        "limits": {
            "maxTextureDimension": MAX_TEXTURE_DIMENSION,
            "maxTexturePixels": MAX_TEXTURE_PIXELS,
            "maxTextureSourceBytes": MAX_TEXTURE_SOURCE_BYTES,
            "maxTextureDecodedBytes": MAX_TEXTURE_DECODED_BYTES,
            "maxPackSourceBytes": MAX_PACK_SOURCE_BYTES,
            "maxPackDecodedBytes": MAX_PACK_DECODED_BYTES,
            "expansionRatioLimit": EXPANSION_RATIO_LIMIT,
            "expansionFloorBytes": EXPANSION_FLOOR_BYTES,
        },
        "coverage": coverage_report(scan.keys, scan.files, mapped),
        "decisionCounts": dict(sorted(by_reason.items())),
        "keys": [key.to_json() for key in scan.keys],
        "files": [record.to_json() for record in scan.files],
    }


def write_manifest(path: Path, manifest: dict) -> None:
    """One writer, so `plan` and `build` cannot drift in formatting."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="utf-8")


# --- materialising ---------------------------------------------------------


def write_pack_ini(path: Path, name: str, author: str | None,
                   version: str | None, priority: int) -> None:
    lines = ["[pack]", f"name = {name}"]
    if author:
        lines.append(f"author = {author}")
    if version:
        lines.append(f"version = {version}")
    lines.append(f"priority = {priority}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def materialise(scan: ScanResult, source_root: Path, out_dir: Path,
                crosswalk: dict[str, str]) -> int:
    """Write `textures/<digest>.png` for every mapped key. Returns the count.

    An `_all` image is copied byte for byte: re-encoding it would cost quality
    for nothing, and the game's decoder accepts more formats than this tool's
    does. A split pair has to be composed, and that is the only place pixels
    are touched.
    """
    textures = out_dir / "textures"
    textures.mkdir(parents=True, exist_ok=True)
    by_path = {record.path: record for record in scan.files}
    written = 0

    for key in scan.keys:
        if key.verdict != VERDICT_SELECTED:
            continue
        digest = crosswalk.get(key.key)
        if digest is None:
            continue
        parts = {by_path[path].name.part: path for path in key.sources
                 if by_path[path].name is not None
                 and by_path[path].verdict == VERDICT_SELECTED}
        target = textures / f"{digest}.png"
        if "all" in parts:
            shutil.copyfile(source_root / parts["all"], target)
        elif "a" not in parts:
            # A lone colour half on a format with no alpha component at all.
            # resolve_keys() selects exactly this shape as REASON_SELECTED_OPAQUE
            # ("carries no alpha, so the _rgb file is complete on its own"), and
            # the split branch below would then read parts["a"] and raise
            # KeyError -- which the PngError handler does not catch, so `build`
            # died partway through textures/ and never reached write_pack_ini(),
            # leaving a directory with textures and no pack.ini. The half IS the
            # texture here, so copy it the way an `all` file is copied.
            shutil.copyfile(source_root / parts["rgb"], target)
        else:
            try:
                width, height, rgba = png_probe.compose_split(
                    (source_root / parts["rgb"]).read_bytes(),
                    (source_root / parts["a"]).read_bytes())
            except png_probe.PngError as error:
                key.verdict = VERDICT_REJECTED
                key.reason = error.reason
                key.detail = error.detail
                continue
            target.write_bytes(png_probe.encode_rgba(width, height, bytes(rgba)))
        key.content_digest = digest
        written += 1
    return written


# --- command line ----------------------------------------------------------


def count_mapped(scan: ScanResult, crosswalk: dict[str, str]) -> int:
    return sum(1 for key in scan.keys
               if key.verdict == VERDICT_SELECTED and key.key in crosswalk)


def report_variants(scan: ScanResult, path: Path) -> None:
    """The hit-rate sweep rice_crc.py's experiment section describes.

    Prints one row per variant and nothing else: no verdict, no "looks right".
    Reading the table is a judgement about what the numbers mean, and
    rice_crc.py states the three readings it can have.
    """
    data = json.loads(path.read_text(encoding="utf-8"))
    dumped = data.get("texturesMapped", 0)
    collisions = data.get("keyCollisions", {})
    mappable = sum(1 for key in scan.keys if key.verdict == VERDICT_SELECTED)
    print("Rice CRC variant hit-rate (UNVALIDATED; a wrong variant should score 0):")
    print(f"  {dumped} dumped texture(s) carried a texel span; "
          f"{mappable} pack key(s) are mappable")
    for variant in rice_crc.VARIANTS:
        table = data.get("variants", {}).get(variant, {})
        hits = count_mapped(scan, table)
        percent = (100.0 * hits / mappable) if mappable else 0.0
        collided = collisions.get(variant, 0)
        note = f"  [{collided} key collision(s) in the dump]" if collided else ""
        print(f"  {variant:<16} {hits:>6} of {mappable} mappable key(s) "
              f"({percent:5.1f}%){note}")


def summarise(scan: ScanResult, manifest: dict) -> None:
    print(f"scanned {len(scan.files)} file(s) under {scan.root_name}")
    for label, count in manifest["decisionCounts"].items():
        print(f"  {label}: {count}")
    print(f"keys: {manifest['coverage']['statement']}")
    if scan.rom_names and len(scan.rom_names) > 1:
        print("  WARNING: the pack names more than one ROM: "
              + ", ".join(scan.rom_names))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def add_scan_arguments(target: argparse.ArgumentParser) -> None:
        target.add_argument("pack", help="the Rice pack directory to read")
        target.add_argument("--manifest", required=True,
                            help="where to write the decision record")
        target.add_argument("--rom-name",
                            help="refuse files naming any other ROM; by default "
                                 "every ROM name found is recorded instead")
        target.add_argument("--source-pack-version",
                            help="the pack author's own version, if they gave "
                                 "one; Rice packs carry no version field")
        target.add_argument("--crosswalk",
                            help="Rice key -> content digest, from the "
                                 "`crosswalk` command")
        target.add_argument("--crc-variant", default=rice_crc.DEFAULT_VARIANT,
                            choices=rice_crc.VARIANTS)

    plan = sub.add_parser("plan", help="decide, record, map nothing")
    add_scan_arguments(plan)
    plan.add_argument("--report-variants", action="store_true",
                      help="print the hit-rate of every CRC variant against "
                           "--crosswalk")

    build = sub.add_parser("build", help="write a content pack")
    add_scan_arguments(build)
    build.add_argument("--out", required=True, help="content pack directory")
    build.add_argument("--name", required=True, help="pack.ini name")
    build.add_argument("--author")
    build.add_argument("--version", dest="pack_version")
    build.add_argument("--priority", type=int, default=100)

    cross = sub.add_parser("crosswalk", help="compute candidate CRCs from a dump")
    cross.add_argument("dump", help="a MDKR_MOD_TEXTURE_DUMP directory")
    cross.add_argument("--out", required=True)

    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if args.command == "crosswalk":
        dump = Path(args.dump).expanduser()
        if not dump.is_dir():
            print(f"FAIL: not a directory: {dump}", file=sys.stderr)
            return 1
        crosswalk = build_crosswalk(dump)
        out = Path(args.out).expanduser()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(crosswalk, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print(f"considered {crosswalk['texturesConsidered']} dumped texture(s), "
              f"mapped {crosswalk['texturesMapped']}, "
              f"skipped {len(crosswalk['skipped'])}")
        if crosswalk["texturesMapped"] == 0:
            reasons = sorted({entry["reason"].split(":")[0]
                              for entry in crosswalk["skipped"]})
            print("NOTE: nothing was mapped. Skip reasons: "
                  + (", ".join(reasons) or "none"))
            if "dump-lacks-texel-span" in reasons:
                print("      A corpus with no <digest>.texels files was dumped "
                      "by a build predating MDKR_MOD_TEXTURE_DUMP_FORMAT 2; "
                      "re-dump with a current binary.")
        return 0

    pack = Path(args.pack).expanduser()
    if not pack.is_dir():
        print(f"FAIL: not a directory: {pack}", file=sys.stderr)
        return 1

    scan = scan_pack(pack, args.rom_name)
    crosswalk: dict[str, str] = {}
    if args.crosswalk:
        crosswalk = load_crosswalk(Path(args.crosswalk).expanduser(),
                                   args.crc_variant)

    if args.command == "build":
        # Checked before anything is written: the runtime rejects an over-long
        # field rather than truncating it (docs/MODDING.md), so a pack that
        # would be refused on load must not reach disk looking complete.
        for value, limit, label in (
                (args.name, MAX_PACK_NAME_CHARS, "name"),
                (args.author, MAX_PACK_AUTHOR_CHARS, "author"),
                (args.pack_version, MAX_PACK_VERSION_CHARS, "version")):
            if value is not None and len(value) > limit:
                print(f"FAIL: pack {label} is longer than {limit} characters",
                      file=sys.stderr)
                return 1
        out_dir = Path(args.out).expanduser()
        mapped = materialise(scan, pack, out_dir, crosswalk)
        write_pack_ini(out_dir / "pack.ini", args.name, args.author,
                       args.pack_version, args.priority)
    else:
        mapped = count_mapped(scan, crosswalk)

    manifest = build_manifest(
        scan, mapped, args.crc_variant, args.source_pack_version,
        Path(args.crosswalk).name if args.crosswalk else None)
    write_manifest(Path(args.manifest).expanduser(), manifest)
    summarise(scan, manifest)
    if args.command == "plan" and args.report_variants:
        if not args.crosswalk:
            print("FAIL: --report-variants needs --crosswalk", file=sys.stderr)
            return 1
        report_variants(scan, Path(args.crosswalk).expanduser())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
