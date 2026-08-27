"""Explicit Khronos-validation seam for ROM-free synthetic GLB tests.

Production code never imports this module. Tests that exercise higher-level
Workshop behavior patch the already-focused validator boundary so their tiny
synthetic GLBs do not require a platform binary. The adapter's own suite and
frozen-package smoke tests cover the real native process contract.
"""

from __future__ import annotations

import hashlib
from contextlib import contextmanager
from typing import Any, Iterator
from unittest import mock


def accepted_validation(payload: bytes, _context: str) -> dict[str, Any]:
    return {
        "schema": "mdkr-gltf-validation-v1",
        "source_sha256": hashlib.sha256(payload).hexdigest(),
        "valid": True,
        "validator": {
            "version": "2.0.0-dev.3.10",
            "commit": "bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1",
            "target": "test-fixture",
            "distribution": "test-fixture",
            "build_executable_sha256": "0" * 64,
            "executable_sha256": "0" * 64,
            "max_issues": 256,
        },
        "report": {
            "issues": {
                "numErrors": 0,
                "numWarnings": 0,
                "numInfos": 0,
                "numHints": 0,
                "messages": [],
                "truncated": False,
            },
        },
    }


@contextmanager
def accepted_character_validation(manager: Any) -> Iterator[Any]:
    """Patch only the production manager's already-tested trust boundary."""
    with mock.patch.object(
        manager, "_validate_character_glb", side_effect=accepted_validation
    ) as validation:
        yield validation
