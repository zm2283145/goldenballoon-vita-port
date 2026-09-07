#!/usr/bin/env python3
"""Pure recipe/notice consistency regressions; no build or packaging execution."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_third_party_notices import native_party_pin_problems


class NativePartyNoticePins(unittest.TestCase):
    def setUp(self):
        self.commit = "443f6934d9007eb7076ab7825ba330f355fcbead"
        self.recipe = ('set(MDKR_LIBDATACHANNEL_VERSION "0.24.5")\n'
                       f'set(MDKR_LIBDATACHANNEL_COMMIT\n "{self.commit}")\n')
        self.notices = {name: f"libdatachannel 0.24.5 at `{self.commit}`"
                        for name in ("NOTICE.md", "THIRD_PARTY.md",
                                     "third_party/native_phone_party/NOTICE.txt")}

    def test_matching_multiline_recipe_and_notices(self):
        self.assertEqual(native_party_pin_problems(self.recipe, self.notices), [])

    def test_one_stale_notice_cannot_hide_behind_other_matching_copies(self):
        for name in self.notices:
            with self.subTest(notice=name):
                changed = dict(self.notices)
                changed[name] = "libdatachannel 0.24.3 at an older commit"
                failures = native_party_pin_problems(self.recipe, changed)
                self.assertEqual(len(failures), 2)
                self.assertTrue(all(name in failure for failure in failures))

    def test_recipe_upgrade_rejects_consistently_stale_notices(self):
        changed = self.recipe.replace("0.24.5", "0.24.6")
        self.assertEqual(len(native_party_pin_problems(changed, self.notices)), 3)

    def test_missing_or_ambiguous_recipe_pin_fails_closed(self):
        for recipe in ("", self.recipe + self.recipe):
            with self.subTest(recipe=recipe):
                failures = native_party_pin_problems(recipe, self.notices)
                self.assertEqual(len(failures), 2)
                self.assertTrue(all("exactly one" in failure for failure in failures))

    def test_prefix_of_another_version_or_commit_is_not_a_match(self):
        notices = {name: text.replace("0.24.5", "0.24.50").replace(self.commit, self.commit + "0")
                   for name, text in self.notices.items()}
        self.assertEqual(len(native_party_pin_problems(self.recipe, notices)), 6)

    def test_missing_notice_is_not_treated_as_an_optional_consumer(self):
        changed = dict(self.notices)
        changed["NOTICE.md"] = ""
        self.assertEqual(len(native_party_pin_problems(self.recipe, changed)), 2)


if __name__ == "__main__":
    unittest.main()
