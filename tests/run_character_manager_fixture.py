#!/usr/bin/env python3
"""Run the manager with the explicit synthetic-GLB validation fixture."""

from __future__ import annotations

import sys
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_package_manager as manager  # noqa: E402
from character_validation_fixture import accepted_validation  # noqa: E402


if __name__ == "__main__":
    with mock.patch.object(
            manager, "_validate_character_glb", side_effect=accepted_validation):
        raise SystemExit(manager.main())
