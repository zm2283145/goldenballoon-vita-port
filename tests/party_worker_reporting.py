"""Privacy-safe Worker check reporting; no process, network or browser execution."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

from check_browser_runtime import CheckFailure


ROOT = Path(__file__).resolve().parent.parent
STARTUP_DIAGNOSTIC_BYTES = 16 * 1024


class SanitizedWorkerFailure(CheckFailure):
    """Only fixed report labels and private metadata paths, never source output."""


def safe_failure_summary(error: BaseException) -> str:
    if isinstance(error, SanitizedWorkerFailure):
        return str(error)
    category = ("check_failed" if isinstance(error, CheckFailure) else
                "process_failed" if isinstance(error, subprocess.SubprocessError) else
                "local_io_failed" if isinstance(error, OSError) else
                "unexpected_failure")
    # Assertion locations are actionable without copying their values: response
    # bodies, invite URLs and CDP errors may contain credentials.
    location = ""
    trace = error.__traceback__
    allowed_files = {
        "check_browser_online_two_person.py", "check_party_service_chaos.py",
        "check_party_capacity.py", "check_browser_runtime.py",
    }
    while trace is not None:
        filename = Path(trace.tb_frame.f_code.co_filename)
        if filename.name in allowed_files and filename.parent == ROOT / "tests":
            # Prefer the actual assertion call over the shared require() body.
            if filename.name != "check_browser_runtime.py" or not location:
                location = f" at tests/{filename.name}:{trace.tb_lineno}"
        trace = trace.tb_next
    return category + location


def startup_diagnostic(log, start_offset: int | None,
                       returncode: int | None, reason: str) -> str:
    """Retain bounded metadata, never raw Worker output or command/env values.

    Only fixed diagnostic labels leave the caller's private log. A startup
    caller supplies the offset before spawning; whole-check reporting uses zero.
    Missing offset means no scan, not permission to read a previous attempt.
    """
    observed_bytes = 0
    sample = b""
    available = False
    try:
        if start_offset is None:
            raise ValueError("startup offset unavailable")
        log.flush()
        # Reopening avoids moving the writer's shared offset on Windows/POSIX.
        with Path(log.name).open("rb") as reader:
            size = os.fstat(reader.fileno()).st_size
            observed_bytes = max(0, size - start_offset)
            reader.seek(max(start_offset, size - STARTUP_DIAGNOSTIC_BYTES))
            sample = reader.read(STARTUP_DIAGNOSTIC_BYTES).lower()
            available = True
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    signatures = {
        "module_not_found": (b"cannot find module", b"err_module_not_found", b"module_not_found"),
        "address_in_use": (b"eaddrinuse", b"address already in use"),
        "permission_denied": (b"eacces", b"eperm", b"permission denied"),
        "build_error": (b"build failed", b"syntaxerror", b"could not resolve"),
        "runtime_start_failure": (b"failed to start", b"runtime failed"),
    }
    signals = [name for name, markers in signatures.items()
               if any(marker in sample for marker in markers)]
    diagnostic = {
        "schemaVersion": 1,
        "reason": reason if reason in {
            "spawn_failed", "early_exit", "readiness_failed", "check_failed", "worker_log_error",
        } else "unclassified",
        "returncode": returncode,
        "logAvailable": available,
        "observedBytes": observed_bytes,
        "sampledBytes": len(sample),
        "truncated": observed_bytes > len(sample),
        "signals": signals,
        "rawOutputRetained": False,
    }
    summary = ",".join(signals) if signals else "unclassified"
    try:
        # mkstemp creates a private 0600 file outside auto-removed working dirs.
        fd, name = tempfile.mkstemp(prefix="mdkr-worker-startup-", suffix=".json")
        try:
            output = os.fdopen(fd, "w", encoding="utf-8")
        except OSError:
            os.close(fd)
            raise
        with output:
            json.dump(diagnostic, output, sort_keys=True)
            output.write("\n")
        return f"signals={summary}; private sanitized diagnostic: {name}"
    except OSError:
        return f"signals={summary}; sanitized diagnostic could not be saved"


def private_worker_failure(error: BaseException, log,
                           returncode: int | None = None,
                           reason: str = "check_failed") -> SanitizedWorkerFailure:
    if isinstance(error, SanitizedWorkerFailure):
        return error  # Preserve the already-safe diagnostic identity/path.
    category = "worker_log_error" if reason == "worker_log_error" else safe_failure_summary(error)
    try:
        detail = startup_diagnostic(log, 0, returncode, reason)
    except Exception:
        # Reporting must not replace the actual failure if evidence is unavailable.
        detail = "private sanitized diagnostic unavailable"
    return SanitizedWorkerFailure(f"{category}; {detail}")
