#!/usr/bin/env python3
"""Verify the exact native Khronos glTF Validator shipped by the Workshop."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import gltf_validator_adapter as adapter


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--target", required=True)
    args = parser.parse_args(argv)
    try:
        manifest = adapter.verify_installation(
            args.executable, args.manifest, args.target
        )
    except (OSError, adapter.ValidatorError) as exc:
        print(f"glTF Validator verification failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({
        "ok": True,
        "target": manifest["target"],
        "version": manifest["version"],
        "distribution": manifest["distribution"],
        "executable_sha256": manifest["executable_sha256"],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
