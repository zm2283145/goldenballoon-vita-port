#!/usr/bin/env python3
"""Unit controls for the Adventure Party ownership-plateau oracle.

``plateau_no_new_high`` is the only verdict in the AP-19 performance check that
tolerates movement: renderer and pointer-registry live counts oscillate by a
few handles because queued globals retire one generation later. These cases pin
both directions of that tolerance -- the lag oscillation the release arm really
observes must pass, and every staircase shape must still fail -- so the rule
cannot be quietly widened until it accepts growth.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_adventure_party_performance import (  # noqa: E402
    plateau_exact, plateau_no_new_high)


def rows(*values: int) -> list[tuple[int, ...]]:
    """One counter per generation, wrapped like a renderer_generation row."""
    return [(5, 0, 0, value) for value in values]


def verdict(*values: int, warmed: int = 5) -> list[str]:
    return plateau_no_new_high(rows(*values), warmed, lambda row: row[3:4],
                               "control")


class PlateauNoNewHighTest(unittest.TestCase):
    def test_observed_lag_oscillation_passes(self) -> None:
        # The series the four-player release arm reported: a +-2 band around
        # 312 that never finishes above the 313 the window opened on.
        self.assertEqual(verdict(313, 311, 312, 312, 313), [])

    def test_pure_staircase_fails_as_a_new_high(self) -> None:
        failures = verdict(20, 21, 22, 23, 24)
        self.assertEqual(len(failures), 1)
        self.assertIn("new ownership high", failures[0])

    def test_spike_masked_staircase_fails_on_the_rising_run(self) -> None:
        # Clause 1 is silent here: the terminal pair (22,23) stays under the
        # transient 25. Clause 2 catches the three give-back-free increases.
        failures = verdict(25, 20, 21, 22, 23)
        self.assertEqual(len(failures), 1)
        self.assertIn("without one retirement", failures[0])
        self.assertIn("3 ownership increases", failures[0])

    def test_alternating_ownership_passes(self) -> None:
        self.assertEqual(verdict(312, 313, 312, 313, 312), [])

    def test_downward_settling_passes(self) -> None:
        self.assertEqual(verdict(320, 318, 316, 315, 315), [])

    def test_flat_plateau_passes(self) -> None:
        self.assertEqual(verdict(312, 312, 312, 312, 312), [])

    def test_terminal_new_high_fails(self) -> None:
        failures = verdict(312, 313, 311, 312, 314)
        self.assertEqual(len(failures), 1)
        self.assertIn("new ownership high", failures[0])

    def test_two_rises_with_a_give_back_pass(self) -> None:
        # A rise, a hold while the queue drains, a second rise, and the window
        # still ends on ground it already occupied: the lag can fund this.
        self.assertEqual(verdict(313, 311, 312, 312, 313), [])
        self.assertEqual(verdict(314, 311, 312, 313, 313), [])

    def test_a_fourth_generation_of_climbing_fails(self) -> None:
        # One generation longer than the tolerated run, still spike-masked.
        failures = plateau_no_new_high(
            rows(40, 20, 21, 22, 23, 24), 6, lambda row: row[3:4], "control")
        self.assertEqual(len(failures), 1)
        self.assertIn("without one retirement", failures[0])

    def test_warmed_one_short_circuits(self) -> None:
        # A development run collapses the window to one generation; a single
        # sample can neither plateau nor grow, so the rule must not opine.
        self.assertEqual(verdict(20, 21, 22, 23, 24, warmed=1), [])
        self.assertEqual(verdict(999, warmed=1), [])

    def test_short_window_is_reported_not_silently_passed(self) -> None:
        failures = verdict(312, 312, warmed=5)
        self.assertEqual(len(failures), 1)
        self.assertIn("only 2 generations, need 5", failures[0])

    def test_each_counter_is_judged_independently(self) -> None:
        # Column 0 oscillates, column 1 is a spike-masked staircase.
        multi = [(5, 0, 0, 313, 25), (5, 0, 0, 311, 20), (5, 0, 0, 312, 21),
                 (5, 0, 0, 312, 22), (5, 0, 0, 313, 23)]
        failures = plateau_no_new_high(multi, 5, lambda row: row[3:5], "control")
        self.assertEqual(len(failures), 1)
        self.assertIn("counter 1", failures[0])
        clean = [(5, 0, 0, live, second) for live, second in
                 zip((313, 311, 312, 312, 313), (25, 24, 23, 22, 21))]
        self.assertEqual(
            plateau_no_new_high(clean, 5, lambda row: row[3:5], "control"), [])


class PlateauExactTest(unittest.TestCase):
    """The sibling rule is untouched: it still demands strict equality."""

    def test_flat_window_passes(self) -> None:
        self.assertEqual(
            plateau_exact(rows(312, 312, 312, 312, 312), 5,
                          lambda row: row[3:4], "control"), [])

    def test_the_tolerated_oscillation_still_fails_exact(self) -> None:
        failures = plateau_exact(rows(313, 311, 312, 312, 313), 5,
                                 lambda row: row[3:4], "control")
        self.assertEqual(len(failures), 1)
        self.assertIn("did not plateau", failures[0])

    def test_short_window_is_reported(self) -> None:
        failures = plateau_exact(rows(312, 312), 5, lambda row: row[3:4],
                                 "control")
        self.assertIn("only 2 generations, need 5", failures[0])


if __name__ == "__main__":
    unittest.main()
