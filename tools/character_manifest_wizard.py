#!/usr/bin/env python3
"""Generate an editable v1 character manifest from names already in a GLB.

This is intentionally a deterministic naming assistant, not an animation
retargeter. It reports every inferred clip/socket so authors can review the
small manifest instead of guessing the runtime semantic vocabulary.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import character_asset_probe as probe


CLIP_ALIASES = {
    "race.steer": ("steer", "turn", "drive"),
    "race.reverse": ("reverse", "backward", "back"),
    "race.boost": ("boost", "turbo", "dash"),
    "race.damage": ("damage", "hurt", "hit"),
    "race.item": ("item", "throw", "attack"),
    "race.spin": ("spin", "spinning"),
    "race.airborne": ("airborne", "jump", "air"),
    "race.land": ("land", "landing"),
    "race.finish_win": ("finishwin", "victory", "win", "celebrate"),
    "race.finish_lose": ("finishlose", "defeat", "lose", "sad"),
    "select.idle": ("selectidle", "idle"),
    "select.hover": ("selecthover", "hover"),
    "select.confirm": ("selectconfirm", "confirm", "selected"),
}

SOCKET_ALIASES = {
    "seat": ("seat", "root", "hips", "pelvis", "armature"),
    "head": ("head", "headbone", "neck"),
    "hand": ("righthand", "handr", "handright", "hand"),
}


def normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.casefold())


def choose(named: list[str], aliases: tuple[str, ...]) -> str | None:
    keyed = [(name, normalized(name)) for name in named]
    for alias in aliases:
        exact = [name for name, key in keyed if key == alias]
        if exact:
            return exact[0]
    for alias in aliases:
        contained = [name for name, key in keyed if alias in key]
        if contained:
            return contained[0]
    return None


def build_manifest(model: Path, package_id: str, display_name: str,
                   spdx: str, attribution: str, source_url: str,
                   donor: str, vehicles: list[str]) -> tuple[dict, dict]:
    data = model.read_bytes()
    report = probe.inspect_glb_bytes(data, require_character=True)
    if report["errors"]:
        raise probe.ProbeError("GLB is not character-ready: " + "; ".join(report["errors"]))
    document, _ = probe.parse_glb(data)
    clips = [animation.get("name") or f"animation_{index}"
             for index, animation in enumerate(document.get("animations", []))
             if isinstance(animation, dict)]
    nodes = [node.get("name") for node in document.get("nodes", [])
             if isinstance(node, dict) and isinstance(node.get("name"), str)
             and node["name"].strip()]
    fallback = choose(clips, ("idle", "default", "fallback", "rest"))
    if fallback is None:
        fallback = clips[0] if clips else None
    if fallback is None:
        raise probe.ProbeError("GLB has no named animation for the required fallback")
    states = {}
    for semantic, aliases in CLIP_ALIASES.items():
        clip = choose(clips, aliases)
        if clip is not None:
            states[semantic] = clip
    sockets = {}
    for semantic, aliases in SOCKET_ALIASES.items():
        node = choose(nodes, aliases)
        if node is not None:
            sockets[semantic] = node
    missing = [name for name in ("seat", "head") if name not in sockets]
    if missing:
        raise probe.ProbeError(
            "could not infer required socket node(s): " + ", ".join(missing) +
            "; name the intended nodes or edit the manifest manually"
        )
    manifest = {
        "schema": probe.PACKAGE_SCHEMA,
        "id": package_id,
        "display_name": display_name,
        "renderer_profile": "modern-skeletal-v1",
        "license": {
            "spdx": spdx,
            "attribution": attribution,
            "source_url": source_url,
        },
        "animations": {"fallback": fallback, "states": states},
        "gameplay": {"donor": donor, "vehicles": vehicles},
        "presentation": {
            "scale": [1.0, 1.0, 1.0],
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            "lod_bias": 0.0,
        },
        "sockets": sockets,
    }
    errors = probe.validate_manifest(manifest, report)
    if errors:
        raise probe.ProbeError("generated manifest is invalid: " + "; ".join(errors))
    decisions = {
        "fallback": fallback,
        "mapped_states": states,
        "missing_recommended_states": [
            state for state in probe.RECOMMENDED_RACE_SEMANTICS if state not in states
        ],
        "missing_select_states": [
            state for state in probe.RECOMMENDED_SELECT_SEMANTICS
            if state not in states
        ],
        "sockets": sockets,
        "scene_world_bounds": [report["bbox_min"], report["bbox_max"]],
        "author_notes": ([
            "race.steer is phase-driven: author full left at 0, neutral at 0.5, and full right at 1"
        ] if "race.steer" in states else []),
        "warnings": report["warnings"],
    }
    return manifest, decisions


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--id", required=True)
    parser.add_argument("--display-name", required=True)
    parser.add_argument("--spdx", required=True)
    parser.add_argument("--attribution", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--donor", default="diddy", choices=sorted(probe.GAMEPLAY_DONORS))
    parser.add_argument("--vehicles", nargs="+", default=["car", "hovercraft", "plane"],
                        choices=sorted(probe.VEHICLE_NAMES))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest, decisions = build_manifest(
            args.model, args.id, args.display_name, args.spdx,
            args.attribution, args.source_url, args.donor, args.vehicles
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        report = {"ok": True, "manifest": str(args.output), **decisions}
        if args.report is not None:
            args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8")
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, probe.ProbeError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, indent=2), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
