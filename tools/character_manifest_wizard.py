#!/usr/bin/env python3
"""Generate an editable v2/v3/v4 character manifest from names in a GLB.

This is intentionally a deterministic naming assistant, not an animation
retargeter. It reports every inferred clip/socket so authors can review the
small manifest instead of guessing the runtime semantic vocabulary.
"""

from __future__ import annotations

import argparse
import hashlib
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
    "hand.left": ("handl", "lefthand", "handleft"),
    "hand.right": ("handr", "righthand", "handright"),
    "foot.left": ("footl", "leftfoot", "footleft"),
    "foot.right": ("footr", "rightfoot", "footright"),
}

RIG_ROLE_ALIASES = {
    "hips": ("hips", "pelvis", "hip", "rootpelvis"),
    "spine": ("spine", "spine01", "spine1", "lowerback"),
    "chest": ("chest", "upperchest", "spine02", "spine2", "upperback"),
    "head": ("head", "headbone"),
    "upper_arm.left": ("leftupperarm", "upperarmleft", "leftarm", "arml"),
    "lower_arm.left": (
        "leftlowerarm", "lowerarmleft", "leftforearm", "forearmleft", "forearml"
    ),
    "hand.left": ("lefthand", "handleft", "handl"),
    "upper_arm.right": ("rightupperarm", "upperarmright", "rightarm", "armr"),
    "lower_arm.right": (
        "rightlowerarm", "lowerarmright", "rightforearm", "forearmright", "forearmr"
    ),
    "hand.right": ("righthand", "handright", "handr"),
    "upper_leg.left": (
        "leftupperleg", "leftupleg", "upperlegleft", "leftthigh",
        "thighleft", "thighl"
    ),
    "lower_leg.left": (
        "leftlowerleg", "leftleg", "lowerlegleft", "leftshin",
        "calfleft", "calfl"
    ),
    "foot.left": ("leftfoot", "footleft", "footl", "leftankle"),
    "upper_leg.right": (
        "rightupperleg", "rightupleg", "upperlegright", "rightthigh",
        "thighright", "thighr"
    ),
    "lower_leg.right": (
        "rightlowerleg", "rightleg", "lowerlegright", "rightshin",
        "calfright", "calfr"
    ),
    "foot.right": ("rightfoot", "footright", "footr", "rightankle"),
}

IDENTITY_QUATERNION = [0.0, 0.0, 0.0, 1.0]


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


def choose_with_confidence(
    named: list[str], aliases: tuple[str, ...]
) -> tuple[str | None, float, list[str]]:
    """Infer one node conservatively and report ambiguity instead of guessing."""
    keyed = [(name, normalized(name)) for name in named]
    tiers = (
        (1.0, lambda key, alias: key == alias),
        (0.9, lambda key, alias: key.endswith(alias)),
        (0.75, lambda key, alias: alias in key),
    )
    for confidence, matches in tiers:
        candidates = sorted({
            name for name, key in keyed for alias in aliases
            if matches(key, alias)
        })
        if len(candidates) == 1:
            return candidates[0], confidence, []
        if len(candidates) > 1:
            return None, 0.0, candidates
    return None, 0.0, []


def build_manifest(model: Path, package_id: str, display_name: str,
                   spdx: str, attribution: str, source_url: str,
                   donor: str, vehicles: list[str], *,
                   source_forward: str = "+z",
                   target_height_m: float = 1.25,
                   fallback_clip: str | None = None,
                   socket_overrides: dict[str, str] | None = None,
                   portrait: Path | None = None,
                   minimap_rgb: list[int] | None = None,
                   rig_mode: str | None = None) -> tuple[dict, dict]:
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
    fallback = fallback_clip
    if fallback is not None and fallback not in clips:
        raise probe.ProbeError(
            f"selected fallback animation {fallback!r} does not exist in the GLB"
        )
    if fallback is None:
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
    if socket_overrides is not None:
        unknown = sorted(set(socket_overrides) - set(SOCKET_ALIASES))
        if unknown:
            raise probe.ProbeError(
                "unknown socket override(s): " + ", ".join(unknown)
            )
        for semantic, node in socket_overrides.items():
            if node not in nodes:
                raise probe.ProbeError(
                    f"selected {semantic} socket node {node!r} does not exist in the GLB"
                )
            sockets[semantic] = node
    missing = [name for name in ("seat", "head") if name not in sockets]
    if missing:
        raise probe.ProbeError(
            "could not infer required socket node(s): " + ", ".join(missing) +
            "; name the intended nodes or edit the manifest manually"
        )
    contexts = {
        "select": {
            "anchor": "ground",
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": list(IDENTITY_QUATERNION),
            "scale": 1.0,
        },
    }
    for vehicle in vehicles:
        contexts[vehicle] = {
            "anchor": "seat",
            "translation_m": [0.0, 0.0, 0.0],
            "rotation_xyzw": list(IDENTITY_QUATERNION),
            "scale": 1.0,
        }
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
            "source_forward": source_forward,
            "target_height_m": target_height_m,
            "contexts": contexts,
            "lod_bias": 0.0,
        },
        "sockets": sockets,
    }
    portrait_report = None
    if portrait is None and minimap_rgb is not None:
        raise probe.ProbeError("--minimap-rgb requires --portrait")
    if portrait is not None:
        portrait_bytes = portrait.read_bytes()
        portrait_report = probe.inspect_portrait_png(portrait_bytes)
        if minimap_rgb is None:
            raise probe.ProbeError(
                "a portrait requires an explicit three-byte minimap colour"
            )
        manifest["schema"] = probe.PACKAGE_SCHEMA_V3
        manifest["identity"] = {
            "portrait_file": "portrait.png",
            "portrait_sha256": hashlib.sha256(portrait_bytes).hexdigest(),
            "minimap_rgb": minimap_rgb,
        }
    rig_decision = None
    if rig_mode is not None:
        if portrait is None:
            raise probe.ProbeError(
                "--rig-mode emits source-v4 and therefore requires --portrait "
                "and --minimap-rgb"
            )
        roles = {}
        ambiguities = {}
        if rig_mode == "humanoid-retarget-v1":
            for role in probe.HUMANOID_ROLES:
                node, confidence, ambiguous = choose_with_confidence(
                    nodes, RIG_ROLE_ALIASES[role]
                )
                if ambiguous:
                    ambiguities[role] = ambiguous
                elif node is not None:
                    roles[role] = {
                        "node": node,
                        "inferred": True,
                        "confidence": confidence,
                    }
            missing = [role for role in probe.HUMANOID_ROLES if role not in roles]
            if missing or ambiguities:
                detail = []
                if missing:
                    detail.append("missing: " + ", ".join(missing))
                if ambiguities:
                    detail.append("ambiguous: " + "; ".join(
                        f"{role} -> {', '.join(candidates)}"
                        for role, candidates in ambiguities.items()
                    ))
                raise probe.ProbeError(
                    "humanoid role inference is incomplete (" + " | ".join(detail) +
                    "); use authored-clips-only or edit and review source-v4 manually"
                )
        manifest["schema"] = probe.PACKAGE_SCHEMA_V4
        manifest["rig"] = {
            "mode": rig_mode,
            "reviewed": False,
            "roles": roles,
        }
        rig_decision = {
            "mode": rig_mode,
            "reviewed": False,
            "roles": roles,
            "inference_requires_review": bool(roles),
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
        "source_forward": source_forward,
        "target_height_m": target_height_m,
        "source_height_m": (
            report["bbox_max"][1] - report["bbox_min"][1]
            if report.get("bbox_min") is not None and report.get("bbox_max") is not None
            else None
        ),
        "attachment_contexts": contexts,
        "author_notes": ([
            "race.steer is phase-driven: author full left at 0, neutral at 0.5, and full right at 1"
        ] if "race.steer" in states else []) + [
            "Confirm source_forward visually; mesh facing cannot be inferred reliably from geometry",
            "Select is ground-anchored; vehicle contexts are pelvis/seat-anchored independently",
        ],
        "warnings": report["warnings"],
        "identity_portrait": portrait_report,
        "rig": rig_decision,
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
    parser.add_argument(
        "--source-forward", default="+z", choices=sorted(probe.SOURCE_FORWARD_AXES),
        help="direction the model faces before normalization; confirm this visually",
    )
    parser.add_argument(
        "--target-height", default=1.25, type=float,
        help="normalized standing height in engine meters (default: 1.25)",
    )
    parser.add_argument(
        "--portrait", type=Path,
        help="square identity PNG; selecting it emits source-v3",
    )
    parser.add_argument(
        "--minimap-rgb", nargs=3, type=int, metavar=("R", "G", "B"),
        help="required with --portrait; each component is 0..255",
    )
    parser.add_argument(
        "--rig-mode", choices=sorted(probe.RIG_MODES),
        help=("emit source-v4 rig metadata; inferred humanoid roles remain "
              "unreviewed until the author explicitly confirms them"),
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest, decisions = build_manifest(
            args.model, args.id, args.display_name, args.spdx,
            args.attribution, args.source_url, args.donor, args.vehicles,
            source_forward=args.source_forward,
            target_height_m=args.target_height,
            portrait=args.portrait,
            minimap_rgb=args.minimap_rgb,
            rig_mode=args.rig_mode,
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
