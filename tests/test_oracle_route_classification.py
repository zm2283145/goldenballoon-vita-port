#!/usr/bin/env python3
"""ROM-free contracts for oracle route classification (gate vs diagnostic).

A route classified ``diagnostic`` records threshold shortfalls as observations
and exits 0, so a deliberately divergent lane (the Ancient Lake open-loop
drift diagnostic) stops presenting as a failing parity gate. Two properties
keep the reclassification honest:

- instrument-integrity failures (vacuous rng seeds, missing probes, too few
  common clocks) still fail closed under ``diagnostic``, so the mode cannot
  rubber-stamp a run that measured nothing; and
- ``gate`` classification produces exactly the same shortfall strings the
  diagnostic run records as observations, so nothing is weakened for real
  gates like the Bluey 2 lane.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "compare_oracle_state.py"

ARES_HEADER = (
    "frame,valid,racer_index,clock,x,y,z,x_velocity,y_velocity,z_velocity,"
    "forward_velocity,velocity,checkpoint,lap,race_finished,finish_position,"
    "player_index,logic_update_rate,rng_seed,race_start_timer,"
    "framebuffer_serial\n"
)

CLOCKS = range(1, 201)
DRIFT_START = 100
POSITION_LIMIT = 5.0
# ares x drifts by (clock - DRIFT_START) after DRIFT_START, so the first
# error above POSITION_LIMIT lands at clock DRIFT_START + 6.
EXPECTED_ONSET_CLOCK = DRIFT_START + int(POSITION_LIMIT) + 1


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def native_text(rng_for_clock) -> str:
    lines = []
    for clock in CLOCKS:
        lines.append(
            f"[ORACLE] frame={clock} map=0 slot=0 "
            f"x={float(clock):.1f} y=0.0 z=0.0 "
            "xv=1.0 yv=0.0 zv=0.0 fvel=1.0 vel=1.0 "
            f"cp={clock // 20} next=0 lap=0 countlap=0 fin=0 fpos=0 "
            f"ridx=0 pidx=0 vehicle=0 grounded=1 "
            f"clock={clock} start=-1 delta=1 rate=2 rng={rng_for_clock(clock)}"
        )
    return "\n".join(lines) + "\n"


def ares_text(rng_for_clock) -> str:
    rows = [ARES_HEADER]
    for clock in CLOCKS:
        x = float(clock if clock <= DRIFT_START else clock + (clock - DRIFT_START))
        rows.append(
            f"{clock},1,0,{clock},{x:.1f},0.0,0.0,1.0,0.0,0.0,1.0,1.0,"
            f"{int(x) // 20},0,0,0,0,2,{rng_for_clock(clock)},-1,{clock}\n"
        )
    return "".join(rows)


def run_compare(
    work: Path, classification: list[str], rng_for_clock
) -> tuple[subprocess.CompletedProcess[str], dict]:
    native = work / "native.log"
    ares = work / "ares.csv"
    out = work / "report.json"
    native.write_text(native_text(rng_for_clock), encoding="utf-8")
    ares.write_text(ares_text(rng_for_clock), encoding="utf-8")
    proc = subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--route", "synthetic_classification",
            "--native-log", str(native),
            "--ares-trace", str(ares),
            "--out", str(out),
            "--min-common-clocks", "50",
            "--min-lap", "0",
            "--min-checkpoint", "5",
            "--max-position-p95", f"{POSITION_LIMIT}",
            "--max-position-error", f"{POSITION_LIMIT}",
            "--min-progress-agreement", "0.98",
            "--min-rng-agreement", "0.9",
            "--max-velocity-ratio-deviation", "0.02",
            *classification,
        ],
        capture_output=True,
        text=True,
    )
    report = json.loads(out.read_text(encoding="utf-8")) if out.is_file() else {}
    return proc, report


def main() -> int:
    with tempfile.TemporaryDirectory() as raw:
        work = Path(raw)

        # Gate classification (explicit): drifting traces must fail, exactly
        # as they always have.
        proc, report = run_compare(work, ["--classification", "gate"], lambda c: c)
        require(proc.returncode == 1, "gate run must exit 1 on drift")
        require(report.get("result") == "FAIL", "gate result must be FAIL")
        require(report.get("classification") == "gate",
                "report must record gate classification")
        gate_failures = report.get("failures", [])
        require(any("agreement" in f for f in gate_failures),
                "gate must flag checkpoint/lap agreement")
        require(any("p95" in f for f in gate_failures),
                "gate must flag position p95")

        # Omitting --classification must behave identically to gate.
        proc_default, report_default = run_compare(work, [], lambda c: c)
        require(proc_default.returncode == 1, "default run must exit 1 on drift")
        require(report_default.get("classification") == "gate",
                "default classification must be gate")
        require(report_default.get("failures") == gate_failures,
                "default failures must match explicit gate failures")

        # Diagnostic classification: same traces, threshold shortfalls become
        # observations, divergence onset is reported, and the exit is clean.
        proc, report = run_compare(
            work, ["--classification", "diagnostic"], lambda c: c
        )
        require(proc.returncode == 0,
                f"diagnostic run must exit 0, got {proc.returncode}: "
                f"{proc.stdout}\n{proc.stderr}")
        require(report.get("result") == "DIAGNOSTIC",
                "diagnostic result must be DIAGNOSTIC")
        require(report.get("classification") == "diagnostic",
                "report must record diagnostic classification")
        require(report.get("failures") == [],
                "diagnostic threshold shortfalls must not be failures")
        observations = report.get("observations", [])
        require(sorted(observations) == sorted(gate_failures),
                "diagnostic observations must equal the gate failure strings")
        onset = report.get("position_error", {}).get("first_over_limit")
        require(onset is not None and onset.get("clock") == EXPECTED_ONSET_CLOCK,
                f"divergence onset must be clock {EXPECTED_ONSET_CLOCK}, "
                f"got {onset}")
        require("DIAGNOSTIC" in proc.stdout,
                "diagnostic verdict must be printed")
        require("divergence onset" in proc.stdout,
                "diagnostic output must headline the divergence onset")

        # Vacuity control: all-zero rng seeds are an instrument-integrity
        # failure and must still fail closed under diagnostic classification.
        proc, report = run_compare(
            work, ["--classification", "diagnostic"], lambda c: 0
        )
        require(proc.returncode == 1,
                "diagnostic must still fail on vacuous rng seeds")
        require(any("non-zero rng seed" in f for f in report.get("failures", [])),
                "vacuous-rng failure must be reported")

    # Route helper: state_classification must be a first-class route field
    # with a gate default, and the validator must reject values outside the
    # closed {gate, diagnostic} set.
    route_tool = ROOT / "tools" / "dkr_oracle_route.py"

    def route_cmd(*argv: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(route_tool), *argv],
            capture_output=True,
            text=True,
            cwd=ROOT,
        )

    proc = route_cmd("field", "race_state_oracle", "state_classification")
    require(proc.returncode == 0 and proc.stdout.strip() == "diagnostic",
            "race_state_oracle must classify as diagnostic, got "
            f"rc={proc.returncode} out={proc.stdout!r} err={proc.stderr!r}")
    proc = route_cmd("field", "bluey2_state_oracle", "state_classification")
    require(proc.returncode == 0 and proc.stdout.strip() == "gate",
            "bluey2_state_oracle must default to gate classification, got "
            f"rc={proc.returncode} out={proc.stdout!r} err={proc.stderr!r}")

    with tempfile.TemporaryDirectory() as raw:
        bad = Path(raw) / "bad_classification.json"
        route = json.loads(
            (ROOT / "tools" / "oracle_routes" / "race_state_oracle.json")
            .read_text(encoding="utf-8")
        )
        route["state_classification"] = "advisory"
        bad.write_text(json.dumps(route), encoding="utf-8")
        proc = route_cmd("validate", str(bad))
        require(proc.returncode != 0,
                "validator must reject an unknown classification value")
        blob = proc.stdout + proc.stderr
        require("state_classification" in blob,
                "rejection must name state_classification")

        # The shipped diagnostic route must validate clean, and dropping the
        # documented basis for the classification must fail closed — the
        # reclassification stays reviewable the same way thresholds are.
        proc = route_cmd(
            "validate",
            str(ROOT / "tools" / "oracle_routes" / "race_state_oracle.json"),
        )
        require(proc.returncode == 0,
                "race_state_oracle must validate with diagnostic "
                f"classification: {proc.stdout}{proc.stderr}")
        unbased = Path(raw) / "unbased_classification.json"
        route = json.loads(
            (ROOT / "tools" / "oracle_routes" / "race_state_oracle.json")
            .read_text(encoding="utf-8")
        )
        route["threshold_basis"].pop("state_classification", None)
        unbased.write_text(json.dumps(route), encoding="utf-8")
        proc = route_cmd("validate", str(unbased))
        require(proc.returncode != 0,
                "declared classification without a basis must fail validation")

    print("oracle route classification contracts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
