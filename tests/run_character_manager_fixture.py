#!/usr/bin/env python3
"""Run the manager with the explicit synthetic-GLB validation fixture."""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import character_package_manager as manager  # noqa: E402
from character_validation_fixture import accepted_validation  # noqa: E402


if __name__ == "__main__":
    # Test-only scheduling seam for lifecycle coverage. It is deliberately
    # bounded and lives in the synthetic validator wrapper, never in the
    # shipped importer or its command protocol.
    delay_text = os.environ.get("MDKR_CHARACTER_MANAGER_FIXTURE_DELAY_MS", "0")
    try:
        delay_ms = int(delay_text)
    except ValueError:
        delay_ms = -1
    if delay_ms < 0 or delay_ms > 2000:
        raise SystemExit("invalid synthetic manager delay")
    if delay_ms:
        time.sleep(delay_ms / 1000.0)
    with mock.patch.object(
            manager, "_validate_character_glb", side_effect=accepted_validation):
        raise SystemExit(manager.main())
