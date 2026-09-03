#!/usr/bin/env python3
"""AP-04 phase 1 static object census generator (measurement only).

Independently re-implements the engine's ROM asset lookup + level-object spawn
resolution (game/src/asset_loading.c asset_table_load; game/src/objects.c
allocate_object_pools / load_object_header / spawn_object; game/src/tracks.c
init_track) to enumerate, per level, every placed object whose ObjectHeader
carries OBJECT_HEADER_NO_MULTIPLATER (bit 1<<6 = 0x40, game/src/objects.h:191) --
exactly the objects objects.c:2970-2978 frees when numPlayers >= 2.

No game code is imported: agreeing with the engine is cross-checking, not a
tautology. Reads baserom.us.v80.z64 (md5 b31f8cca50f31acc9b999ed5b779d6ed).

Usage: census.py <rom> <out.json>
"""
import struct, sys, zlib, json, re, os

ROM = sys.argv[1] if len(sys.argv) > 1 else "baserom.us.v80.z64"
OUT = sys.argv[2] if len(sys.argv) > 2 else "object-census.json"
def find_repo_root():
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(8):
        if os.path.exists(os.path.join(d, "game/include/object_behaviors.h")):
            return d
        d = os.path.dirname(d)
    return os.getcwd()
REPO = find_repo_root()
rom = open(ROM, "rb").read()

# Master asset LUT (game/src/asset_loading.c: gDkrAssetsLutStart = 0x000ED0E0;
# data base = LUT end = 0x000ED1B0). Section S spans [LUT[S+1], LUT[S+2]).
LUT_START = 0x000ED0E0
DATA_BASE = 0x000ED1B0
def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be16s(b, o): return struct.unpack_from(">h", b, o)[0]
def s8(b, o):
    v = b[o]; return v - 256 if v >= 128 else v
def section(idx):
    s = be32(rom, LUT_START + 4*(idx+1)); e = be32(rom, LUT_START + 4*(idx+2))
    return rom[DATA_BASE+s: DATA_BASE+e]

S_LOM_TABLE, S_LOM = 20, 21          # ASSET_LEVEL_OBJECT_MAPS(_TABLE)
S_LH_TABLE, S_LH = 22, 23            # ASSET_LEVEL_HEADERS(_TABLE)
S_OH_TABLE, S_OBJECTS = 33, 34       # ASSET_OBJECT_HEADERS_TABLE / ASSET_OBJECTS
S_LOTT = 35                          # ASSET_LEVEL_OBJECT_TRANSLATION_TABLE

NO_MULTI = 1 << 6   # OBJECT_HEADER_NO_MULTIPLATER
NO_TT    = 1 << 5   # OBJECT_HEADER_NO_TIME_TRIAL

def read_c_enum(path, enum_name, prefix):
    txt = open(os.path.join(REPO, path)).read()
    m = re.search(r"enum\s+%s\s*\{(.*?)\}" % enum_name, txt, re.S)
    if not m: m = re.search(r"typedef enum %s\s*\{(.*?)\}" % enum_name, txt, re.S)
    out = []
    for line in m.group(1).splitlines():
        t = line.strip().rstrip(",")
        if t.startswith(prefix):
            out.append(t)
    return out

BHV = read_c_enum("game/include/object_behaviors.h", "ObjectBehaviours", "BHV_")
LVL_NAMES = [x for x in read_c_enum(
    "game/include/asset_enums.h", "AssetLevelHeadersEnum", "ASSET_LEVEL_")
    if not x.endswith("COUNT")]
def bhv_name(i): return BHV[i] if 0 <= i < len(BHV) else "BHV_?(%d)" % i

RACETYPE = {0:"DEFAULT",3:"HORSESHOE_GULCH",5:"HUBWORLD",6:"CUTSCENE_1",7:"CUTSCENE_2",
            8:"BOSS",64:"CHALLENGE_BATTLE",65:"CHALLENGE_BANANAS",66:"CHALLENGE_EGGS"}
WORLD = {-1:"NONE",0:"CENTRAL_AREA",1:"DINO_DOMAIN",2:"SHERBET_ISLAND",
         3:"SNOWFLAKE_MOUNTAIN",4:"DRAGON_FOREST",5:"FUTURE_FUN_LAND"}

# Party-critical behaviours (brief: doors, exits, Taj, golden balloons, keys,
# teleporters, race-entry triggers, lobby systems, and the shared collectables a
# party race depends on). A flagged object in this set is a co-op hazard.
PARTY_CRITICAL = {
    "BHV_EXIT","BHV_DOOR","BHV_TT_DOOR","BHV_DOOR_OPENER",
    "BHV_PARK_WARDEN","BHV_PARK_WARDEN_2","BHV_TAJ_TELEPOINT","BHV_WARDEN_SMOKE",
    "BHV_GOLDEN_BALLOON","BHV_WORLD_KEY","BHV_TELEPORT",
    "BHV_ROCKET_SIGNPOST","BHV_ROCKET_SIGNPOST_2","BHV_TROPHY_CABINET",
    "BHV_SETUP_POINT","BHV_CHECKPOINT","BHV_TRIGGER","BHV_RANGE_TRIGGER",
    "BHV_LEVEL_NAME","BHV_SILVER_COIN","BHV_SILVER_COIN_2","BHV_FLY_COIN",
    "BHV_WEAPON_BALLOON","BHV_CHARACTER_SELECT","BHV_MODECHANGE","BHV_BONUS",
    "BHV_WIZPIG_SHIP","BHV_PIG_ROCKETEER","BHV_COLLECT_EGG","BHV_EGG_CREATOR",
}
def classify(behavior):
    return "party-critical" if behavior in PARTY_CRITICAL else "cosmetic/benign"

# ---------- object headers (section 34 via table 33) ----------
def table_offsets(sec_idx):
    tbl = section(sec_idx); offs = []; i = 0
    while True:
        v = be32(tbl, i*4)
        if v == 0xFFFFFFFF: break
        offs.append(v); i += 1
    return offs

def parse_object_headers():
    offs = table_offsets(S_OH_TABLE)
    objs = section(S_OBJECTS)
    headers = []
    for i in range(len(offs)-1):
        o = offs[i]
        flags = be16(objs, o+0x30)
        beh = objs[o+0x54]
        name = objs[o+0x60:o+0x70].split(b"\x00")[0].decode("ascii","replace")
        headers.append(dict(index=i, flags=flags, behaviorId=beh,
                            behavior=bhv_name(beh), modelType=objs[o+0x53], name=name))
    return headers

def parse_lott():
    t = section(S_LOTT); n = len(t)//2
    return [be16s(t, i*2) for i in range(n)]

def parse_level_headers():
    offs = table_offsets(S_LH_TABLE); data = section(S_LH); levels = []
    for i in range(len(offs)-1):
        o = offs[i]
        levels.append(dict(
            id=i, enum=(LVL_NAMES[i] if i < len(LVL_NAMES) else "?"),
            world=s8(data,o), world_name=WORLD.get(s8(data,o), str(s8(data,o))),
            race_type=s8(data,o+0x4C),
            race_type_name=RACETYPE.get(s8(data,o+0x4C), str(s8(data,o+0x4C))),
            main_map=be16s(data,o+0xBA), collectables_map=be16s(data,o+0x36)))
    return levels

# ---------- object maps (section 21 via table 20; gzip/rzip per map) ----------
def map_objtypes(mapoffs, mapID):
    """objType list placed in this map, or (None, reason)."""
    data = section(S_LOM)
    if mapID < 0 or mapID+1 >= len(mapoffs):
        return None, "mapID %d out of table range" % mapID
    o0, o1 = mapoffs[mapID], mapoffs[mapID+1]
    if o1 <= o0: return [], "empty"
    blob = data[o0:o1]
    # rzip header: 4-byte LE decompressed size + 1 byte, then raw DEFLATE at +5.
    declared = struct.unpack_from("<I", blob, 0)[0]
    try:
        d = zlib.decompressobj(-15); out = d.decompress(blob[5:]) + d.flush()
    except Exception as e:
        return None, "inflate error: %s" % e
    if len(out) < declared:
        return None, "short inflate %d < %d" % (len(out), declared)
    entry_bytes = be32(out, 0)          # LevelObjectMapHeader.fileSize (BE)
    body = out[16:16+entry_bytes]       # OBJ_MAP_HEADER_S32S = 4 words = 16 bytes
    objtypes = []; p = 0
    while p + 2 <= len(body):
        b0, b1 = body[p], body[p+1]
        objType = b0 | ((b1 & 0x80) << 1)   # spawn_object: objects.c:3764
        stride = b1 & 0x3F                    # objects.c:2526 / :3754
        if stride < 8 or (p + stride) > len(body):
            break
        objtypes.append(objType); p += stride
    return objtypes, "ok"

def resolve(objtypes, lott, headers):
    """objType -> LOTT -> header index -> header. Count flagged placements."""
    from collections import Counter
    by_header = Counter()
    unresolved = 0
    for ot in objtypes:
        if ot < 0 or ot >= len(lott):
            unresolved += 1; continue
        hidx = lott[ot]
        if hidx < 0 or hidx >= len(headers):
            unresolved += 1; continue
        by_header[hidx] += 1
    return by_header, unresolved

def main():
    headers = parse_object_headers()
    lott = parse_lott()
    levels = parse_level_headers()
    mapoffs = table_offsets(S_LOM_TABLE)
    flagged_idx = {h["index"] for h in headers if h["flags"] & NO_MULTI}

    header_records = []
    for h in headers:
        if h["flags"] & NO_MULTI:
            header_records.append(dict(
                index=h["index"], name=h["name"], behavior=h["behavior"],
                flags="0x%04x" % h["flags"], no_multiplayer=True,
                no_time_trial=bool(h["flags"] & NO_TT),
                classification=classify(h["behavior"])))

    level_records = []
    for L in levels:
        rec = dict(L)
        rec["maps"] = {}
        flagged_here = {}   # header index -> {name,behavior,count,classification}
        parse_notes = []
        for label, mid in (("main", L["main_map"]), ("collectables", L["collectables_map"])):
            objtypes, status = map_objtypes(mapoffs, mid)
            if objtypes is None:
                rec["maps"][label] = dict(map_id=mid, status="NOT PARSED: "+status)
                parse_notes.append("%s map %d: %s" % (label, mid, status))
                continue
            by_header, unresolved = resolve(objtypes, lott, headers)
            n_flagged = sum(c for hi,c in by_header.items() if hi in flagged_idx)
            rec["maps"][label] = dict(map_id=mid, total_objects=len(objtypes),
                                      unresolved=unresolved, flagged_objects=n_flagged)
            for hi, c in by_header.items():
                if hi in flagged_idx:
                    h = headers[hi]
                    e = flagged_here.setdefault(hi, dict(
                        header_index=hi, name=h["name"], behavior=h["behavior"],
                        classification=classify(h["behavior"]), count=0))
                    e["count"] += c
        rec["flagged_objects"] = sorted(flagged_here.values(), key=lambda x:-x["count"])
        rec["flagged_total"] = sum(e["count"] for e in flagged_here.values())
        rec["flagged_party_critical"] = sum(
            e["count"] for e in flagged_here.values()
            if e["classification"] == "party-critical")
        if parse_notes: rec["parse_notes"] = parse_notes
        level_records.append(rec)

    doc = dict(
        generated="2026-08-28",
        method="static ROM asset parse (independent re-implementation of the "
               "engine spawn resolution chain)",
        rom=dict(path=os.path.basename(ROM), md5="b31f8cca50f31acc9b999ed5b779d6ed",
                 size=len(rom)),
        flag=dict(name="OBJECT_HEADER_NO_MULTIPLATER", bit=6, value=NO_MULTI,
                  freed_by="objects.c:2970-2978 when numPlayers >= 2"),
        counts=dict(object_headers=len(headers),
                    flagged_headers=len(header_records),
                    translation_table_capacity=len(lott),
                    levels=len(levels), object_maps=len(mapoffs)-1),
        flagged_object_headers=header_records,
        levels=level_records)
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=2)
    # stdout summary
    print("object headers=%d  flagged=%d  levels=%d  maps=%d  LOTT=%d" %
          (len(headers), len(header_records), len(levels), len(mapoffs)-1, len(lott)))
    print("flagged header behaviors:",
          sorted({h["behavior"] for h in header_records}))
    print("any flagged header party-critical:",
          any(h["classification"]=="party-critical" for h in header_records))
    hubs = [r for r in level_records if r["race_type_name"]=="HUBWORLD"]
    print("\nHUBS:")
    for r in hubs:
        print("  L%-2d %-30s flagged=%d (party-critical=%d) maps main=%s coll=%s"
              % (r["id"], r["enum"], r["flagged_total"], r["flagged_party_critical"],
                 r["maps"].get("main"), r["maps"].get("collectables")))
    print("\nADVENTURE COURSES (DEFAULT/HORSESHOE, world 0-5):")
    for r in level_records:
        if r["race_type_name"] in ("DEFAULT","HORSESHOE_GULCH") and 0 <= r["world"] <= 5:
            print("  L%-2d %-22s %-18s flagged=%d (pc=%d)" %
                  (r["id"], r["enum"], r["world_name"], r["flagged_total"],
                   r["flagged_party_critical"]))
    # global: any party-critical flagged placement anywhere
    pc = sum(r["flagged_party_critical"] for r in level_records)
    print("\nTOTAL party-critical flagged placements across ALL levels:", pc)
    npnotes = [(r["id"],r["enum"],r.get("parse_notes")) for r in level_records if r.get("parse_notes")]
    print("levels with unparsed maps:", len(npnotes))
    for x in npnotes: print("   ", x)

if __name__ == "__main__":
    main()
