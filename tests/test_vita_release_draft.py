#!/usr/bin/env python3
"""Exercise draft creation/refresh without credentials or network writes."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

from test_vita_vpk import make_sfo, verifier


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "draft_vita_release", ROOT / "tools" / "ci" / "draft_vita_release.py")
draft = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(draft)
SOURCE = "a" * 40


class FakeGitHub:
    repo = "owner/repo"

    def __init__(self):
        self.main = SOURCE
        self.calls = []
        self.uploads = []
        self.publish_during_upload = False
        self.fail_upload = False
        self.items = [{
            "id": 1, "tag_name": "v1.7.2", "draft": False, "prerelease": False,
            "published_at": "2026-09-17T00:00:00Z", "body": "Published notes",
            "assets": [{"id": 1, "name": "published.vpk", "digest": "untouched"}],
        }]
        self.next_asset = 100

    def releases(self):
        return copy.deepcopy(self.items)

    def api(self, method, endpoint, data=None):
        self.calls.append((method, endpoint, copy.deepcopy(data)))
        if endpoint == "git/ref/heads/main":
            return {"object": {"sha": self.main}}
        if endpoint == "releases/generate-notes":
            return {"body": "## What's Changed\n\n* A merged improvement in #12"}
        if method == "POST" and endpoint == "releases":
            release = dict(data, id=len(self.items) + 1, assets=[],
                           published_at=None, html_url="https://github.com/owner/repo/releases/draft")
            if release["tag_name"] == "vita-next":
                release["tag_name"] = "untagged-generated"
            self.items.append(release)
            return copy.deepcopy(release)
        if endpoint.startswith("releases/assets/"):
            asset_id = int(endpoint.rsplit("/", 1)[1])
            for release in self.items:
                release["assets"] = [a for a in release["assets"] if a["id"] != asset_id]
            return None
        release = next(item for item in self.items if item["id"] == int(endpoint.split("/")[1]))
        if method == "PATCH":
            release.update(data)
        return copy.deepcopy(release)

    def upload(self, tag, path):
        if self.fail_upload:
            raise OSError("simulated upload failure")
        release = next(item for item in self.items if (
            item["tag_name"] == tag or draft.is_next_build_release(item)
        ))
        self.uploads.append(path.name)
        release["assets"].append({
            "id": self.next_asset, "name": path.name,
            "digest": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
        })
        self.next_asset += 1
        if self.publish_during_upload:
            release["draft"] = False


class DraftReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.github = FakeGitHub()
        self.addCleanup(patch.stopall)
        patch.object(draft, "check_tag_target").start()
        patch.object(draft, "commit_notes", return_value="### Commit changes\n\n- Direct fix").start()
        self.artifacts()

    def artifacts(self, source=SOURCE, run="100", version="1.7.3"):
        for path in self.directory.iterdir():
            path.unlink()
        name = f"GoldenBalloon-Vita-v{version}-{source[:8]}-{run}-1.vpk"
        major, minor, patch_version = map(int, version.split("."))
        sfo_version = f"{major:02d}.{minor}{patch_version}"
        vpk = self.directory / name
        with zipfile.ZipFile(vpk, "w") as archive:
            for member in verifier.FILES:
                data = make_sfo(version=sfo_version) if member.endswith("param.sfo") else b"fixture"
                archive.writestr(member, data)
        report = draft.verify(vpk, version)
        (self.directory / "verification.json").write_text(json.dumps(report))
        (self.directory / "SHA256SUMS.txt").write_text(f"{report['sha256']}  {name}\n")
        (self.directory / "BUILD_INFO.txt").write_text(
            f"source={source}\nversion={version}\n")

    def run_draft(self, source=SOURCE, version="1.7.3"):
        return draft.update_draft(self.github, version, source, self.directory)

    def test_creates_unpublished_draft_with_generated_and_direct_changes(self):
        published = copy.deepcopy(self.github.items[0])
        result = self.run_draft()
        release = self.github.items[-1]
        self.assertEqual(result["action"], "created")
        self.assertEqual(result["tag"], "vita-next")
        self.assertEqual(release["tag_name"], "vita-next")
        self.assertEqual(release["name"], "Golden Balloon Vita - Next Build (v1.7.3)")
        self.assertTrue(release["draft"])
        self.assertEqual(release["target_commitish"], SOURCE)
        self.assertEqual(len(release["assets"]), 4)
        self.assertIn("merged improvement", release["body"])
        self.assertIn("Direct fix", release["body"])
        self.assertEqual(self.github.items[0], published)
        generation = next(call for call in self.github.calls if call[1] == "releases/generate-notes")
        self.assertEqual(generation[2]["previous_tag_name"], "v1.7.2")
        self.assertFalse(any(call[2] and call[2].get("draft") is False for call in self.github.calls))

    def test_published_app_version_still_creates_rolling_draft(self):
        self.artifacts(version="1.7.2")
        result = self.run_draft(version="1.7.2")
        self.assertEqual(result["action"], "created")
        self.assertEqual(self.github.items[-1]["tag_name"], "vita-next")
        self.assertEqual(len(self.github.uploads), 4)

    def test_published_rolling_tag_has_no_writes_or_uploads(self):
        self.github.items.append({
            "id": 2, "tag_name": "vita-next", "draft": False,
            "prerelease": False, "published_at": "2026-09-18T00:00:00Z",
            "body": "Published rolling tag", "assets": [],
        })
        result = self.run_draft()
        self.assertEqual(result["action"], "skipped-published")
        self.assertEqual(self.github.calls, [])
        self.assertEqual(self.github.uploads, [])

    def test_stale_build_does_not_change_draft(self):
        self.github.main = "b" * 40
        result = self.run_draft()
        self.assertEqual(result["action"], "skipped-stale")
        self.assertEqual(self.github.uploads, [])
        self.assertTrue(all(call[0] == "GET" for call in self.github.calls))

    def test_refresh_preserves_manual_notes_and_unmanaged_assets(self):
        self.run_draft()
        release = self.github.items[-1]
        old_ids = {asset["id"] for asset in release["assets"]}
        release["body"] = "Manual introduction\n\n" + release["body"] + "\nManual ending"
        release["assets"].append({"id": 900, "name": "manual.txt", "digest": "keep"})
        self.github.main = "b" * 40
        self.artifacts(self.github.main, run="101")
        result = self.run_draft(source=self.github.main)
        self.assertEqual(result["action"], "updated")
        self.assertEqual(len(self.github.items), 2)
        self.assertTrue(release["body"].startswith("Manual introduction"))
        self.assertTrue(release["body"].endswith("Manual ending"))
        ids = {asset["id"] for asset in release["assets"]}
        self.assertIn(900, ids)
        self.assertTrue(ids.isdisjoint(old_ids))
        self.assertEqual(release["target_commitish"], self.github.main)
        self.assertTrue(release["draft"])

    def test_same_artifact_retry_reuses_existing_uploads(self):
        self.run_draft()
        self.run_draft()
        self.assertEqual(len(self.github.uploads), 4)
        self.assertEqual(len(self.github.items[-1]["assets"]), 4)

    def test_recovers_existing_untagged_next_build_draft(self):
        self.github.items.append({
            "id": 2, "tag_name": "untagged-existing", "draft": True,
            "prerelease": False, "published_at": None,
            "name": "Golden Balloon Vita - Next Build (v1.7.2)",
            "body": "Manual note\n\n<!-- vita-next-build-draft -->",
            "assets": [], "html_url": "https://github.com/owner/repo/releases/draft",
        })
        result = self.run_draft()
        self.assertEqual(result["action"], "updated")
        self.assertEqual(len(self.github.items), 2)
        self.assertEqual(self.github.items[-1]["tag_name"], "vita-next")
        self.assertIn("Manual note", self.github.items[-1]["body"])

    def test_bad_checksum_rejected_before_mutation(self):
        (self.directory / "SHA256SUMS.txt").write_text("bad")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.run_draft()
        self.assertTrue(all(call[0] == "GET" for call in self.github.calls))

    def test_publish_during_upload_stops_all_further_writes(self):
        self.github.publish_during_upload = True
        with self.assertRaisesRegex(ValueError, "published while"):
            self.run_draft()
        self.assertEqual(len(self.github.uploads), 1)
        self.assertFalse(any(call[0] in ("PATCH", "DELETE") for call in self.github.calls))

    def test_failed_refresh_leaves_prior_assets_and_notes(self):
        self.run_draft()
        before = copy.deepcopy(self.github.items[-1])
        self.artifacts(run="102")
        self.github.fail_upload = True
        with self.assertRaisesRegex(OSError, "upload failure"):
            self.run_draft()
        self.assertEqual(self.github.items[-1], before)

    def test_duplicate_drafts_rejected(self):
        self.run_draft()
        self.github.items.append(copy.deepcopy(self.github.items[-1]))
        with self.assertRaisesRegex(ValueError, "Multiple drafts"):
            self.run_draft()

    def test_malformed_markers_rejected(self):
        self.run_draft()
        self.github.items[-1]["body"] = draft.END + draft.START
        count = len(self.github.uploads)
        with self.assertRaisesRegex(ValueError, "malformed"):
            self.run_draft()
        self.assertEqual(len(self.github.uploads), count)

    def test_existing_tag_is_never_moved(self):
        with patch.object(draft, "check_tag_target", side_effect=ValueError("tag points elsewhere")):
            with self.assertRaisesRegex(ValueError, "tag points elsewhere"):
                self.run_draft()
        self.assertEqual(self.github.uploads, [])

    def test_github_response_uses_utf8_on_windows(self):
        with patch.object(draft.subprocess, "run", return_value=SimpleNamespace(
            stdout='{"body": "\\u201cRelease notes\\u201d"}'
        )) as command:
            result = draft.GitHub("owner/repo").api("GET", "releases")
        self.assertEqual(result["body"], "\u201cRelease notes\u201d")
        self.assertEqual(command.call_args.kwargs["encoding"], "utf-8")


if __name__ == "__main__":
    unittest.main()
