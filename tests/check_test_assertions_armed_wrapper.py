#!/usr/bin/env python3
"""Suite entry point for the repository tool of the same responsibility."""

from pathlib import Path
import runpy

runpy.run_path(
    str(Path(__file__).resolve().parents[1] / "tools" /
        "check_test_assertions_armed.py"),
    run_name="__main__",
)
