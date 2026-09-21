#!/usr/bin/env python3
"""Refresh the rolling Vita next-build draft; never publish a release."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_vita_vpk import app_version, verify


START = "<!-- vita-vpk-generated:start -->"
END = "<!-- vita-vpk-generated:end -->"
ASSETS = re.compile(r"<!-- vita-vpk-assets: ([0-9,]*) -->")
NEXT_BUILD_TAG = "vita-next"


class GitHub:
    def __init__(self, repo):
        self.repo = repo

    def api(self, method, endpoint, data=None):
        command = ["gh", "api", "--method", method, f"repos/{self.repo}/{endpoint}"]
        if data is not None:
            command += ["--input", "-"]
        result = subprocess.run(
            command, input=json.dumps(data) if data is not None else None,
            text=True, encoding="utf-8", capture_output=True, check=True,
        )
        return json.loads(result.stdout) if result.stdout.strip() else None

    def releases(self):
        result = []
        page = 1
        while True:
            batch = self.api("GET", f"releases?per_page=100&page={page}")
            result.extend(batch)
            if len(batch) < 100:
                return result
            page += 1

    def upload(self, tag, path):
        subprocess.run(
            ["gh", "release", "upload", tag, str(path), "--repo", self.repo],
            text=True, encoding="utf-8", capture_output=True, check=True,
        )


def require_draft(github, release_id):
    release = github.api("GET", f"releases/{release_id}")
    if not release["draft"]:
        raise ValueError("Release was published while this job ran; refusing further writes")
    return release


def replace_generated(body, generated):
    if START not in body and END not in body:
        return body.rstrip() + ("\n\n" if body.strip() else "") + generated
    if body.count(START) != 1 or body.count(END) != 1 or body.index(END) < body.index(START):
        raise ValueError("Draft has malformed generated-note markers; refusing to overwrite it")
    return body[:body.index(START)] + generated + body[body.index(END) + len(END):]


def commit_notes(repo, source, previous_tag):
    revision = f"{previous_tag}..{source}" if previous_tag else source
    rows = subprocess.check_output(
        ["git", "log", "--no-merges", "--max-count=100", "--format=%H%x09%s", revision],
        text=True, encoding="utf-8",
    ).splitlines()
    lines = ["### Commit changes", ""]
    for row in rows:
        commit, subject = row.split("\t", 1)
        subject = re.sub(r"([\\`*_{}\[\]<>])", r"\\\1", subject)
        lines.append(f"- {subject} ([{commit[:8]}](https://github.com/{repo}/commit/{commit}))")
    if not rows:
        lines.append("No non-merge commits since the previous release.")
    if len(rows) == 100:
        lines.append("\nShowing up to 100 commits; use the comparison link for the full history.")
    return "\n".join(lines)


def check_tag_target(tag, source):
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}"],
        text=True, encoding="utf-8", capture_output=True,
    )
    if result.returncode not in (0, 1):
        raise ValueError("Could not inspect the existing release tag")
    if result.returncode == 0 and result.stdout.strip() != source:
        raise ValueError("Existing release tag points elsewhere; refusing to move it")


def verified_files(directory, version, source):
    vpks = list(directory.glob("*.vpk"))
    if len(vpks) != 1:
        raise ValueError("Expected exactly one VPK artifact")
    vpk = vpks[0]
    pattern = rf"GoldenBalloon-Vita-v{re.escape(version)}-{source[:8]}-[0-9]+-[0-9]+\.vpk"
    if re.fullmatch(pattern, vpk.name) is None:
        raise ValueError("VPK filename does not match source/run identity")
    report = verify(vpk, version)
    recorded = json.loads((directory / "verification.json").read_text())
    if recorded != report:
        raise ValueError("Artifact verification receipt does not match the VPK")
    checksum = (directory / "SHA256SUMS.txt").read_text().strip()
    if checksum != f"{report['sha256']}  {vpk.name}":
        raise ValueError("Artifact checksum does not match the VPK")
    info = (directory / "BUILD_INFO.txt").read_text().splitlines()
    if f"source={source}" not in info or f"version={version}" not in info:
        raise ValueError("Artifact build identity does not match this job")
    return {
        vpk.name: vpk,
        f"{vpk.stem}.sha256": directory / "SHA256SUMS.txt",
        f"{vpk.stem}.build-info.txt": directory / "BUILD_INFO.txt",
        f"{vpk.stem}.verification.json": directory / "verification.json",
    }


def update_draft(github, version, source, directory):
    app_version(version)
    if re.fullmatch(r"[0-9a-f]{40}", source) is None:
        raise ValueError("Expected a full source commit SHA")
    tag = NEXT_BUILD_TAG
    releases = github.releases()
    matches = [release for release in releases if release["tag_name"] == tag]
    if any(not release["draft"] for release in matches):
        return {"action": "skipped-published", "tag": tag,
                "message": (
                    "Published vita-next release untouched. Rename its tag before "
                    "publishing so the rolling draft can be recreated."
                )}
    if len(matches) > 1:
        raise ValueError("Multiple drafts exist for this version; resolve them before retrying")
    main = github.api("GET", "git/ref/heads/main")["object"]["sha"]
    if main != source:
        return {"action": "skipped-stale", "tag": tag,
                "message": "Main advanced during the build; the newer build will refresh the draft."}
    files = verified_files(directory, version, source)
    check_tag_target(tag, source)

    stable = [release for release in releases if not release["draft"] and not release["prerelease"]]
    previous = max(stable, key=lambda release: release["published_at"]) if stable else None
    previous_tag = previous["tag_name"] if previous else None
    data = {"tag_name": tag, "target_commitish": source}
    if previous_tag:
        data["previous_tag_name"] = previous_tag
    generated = github.api("POST", "releases/generate-notes", data)["body"]
    changes = commit_notes(github.repo, source, previous_tag)
    compare = (
        f"https://github.com/{github.repo}/compare/{previous_tag}...{source}"
        if previous_tag else f"https://github.com/{github.repo}/commits/{source}"
    )
    content = (
        f"## Golden Balloon Vita Next Build\n\n"
        "**Draft build: review these notes and validate on hardware before publishing.**\n\n"
        f"App version `{version}` built from main commit `{source}`. "
        f"[Full changes]({compare}).\n\n"
        f"{generated}\n\n{changes}\n\n"
        "The attached VPK includes LiveArea artwork and trophies, but no ROM or texture pack. "
        "Debugger and profiler integration are disabled.\n\n"
        "Before publishing, replace the `vita-next` tag with the intended release tag "
        "and update `MDKR_VERSION` when the public app version changes.\n"
    )
    if matches:
        release = require_draft(github, matches[0]["id"])
        replace_generated(release.get("body") or "", f"{START}\n{END}")
    else:
        release = github.api("POST", "releases", {
            "tag_name": tag, "target_commitish": source,
            "name": f"Golden Balloon Vita - Next Build (v{version})",
            "draft": True, "prerelease": False,
            "body": f"{START}\nBuild upload in progress; do not publish yet.\n{END}",
        })
    release_id = release["id"]
    old_block = (release.get("body") or "").split(START, 1)[-1].split(END, 1)[0]
    old_ids = ASSETS.search(old_block) if START in (release.get("body") or "") else None
    old_ids = {int(value) for value in old_ids[1].split(",") if value} if old_ids else set()
    new_ids = []
    with tempfile.TemporaryDirectory(prefix="vita-draft-") as temporary:
        for name, path in files.items():
            current = require_draft(github, release_id)
            asset = next((item for item in current["assets"] if item["name"] == name), None)
            digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
            if asset is None:
                upload = Path(temporary) / name
                shutil.copyfile(path, upload)
                github.upload(tag, upload)
                current = require_draft(github, release_id)
                asset = next((item for item in current["assets"] if item["name"] == name), None)
            if asset is None or asset.get("digest") != digest:
                raise ValueError(f"Uploaded asset checksum mismatch: {name}")
            new_ids.append(asset["id"])

    # Update only our note block, retaining notes a maintainer added outside it.
    current = require_draft(github, release_id)
    state = ",".join(map(str, new_ids))
    block = f"{START}\n{content}\n<!-- vita-vpk-assets: {state} -->\n{END}"
    body = replace_generated(current.get("body") or "", block)
    github.api("PATCH", f"releases/{release_id}", {
        "body": body,
        "name": f"Golden Balloon Vita - Next Build (v{version})",
        "target_commitish": source,
    })
    for asset in current["assets"]:
        if asset["id"] in old_ids - set(new_ids):
            require_draft(github, release_id)
            github.api("DELETE", f"releases/assets/{asset['id']}")
    return {"action": "updated" if matches else "created", "tag": tag,
            "url": current["html_url"], "source": source}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    if re.fullmatch(r"[\w.-]+/[\w.-]+", args.repo) is None:
        parser.error("Expected owner/repository")
    try:
        result = update_draft(GitHub(args.repo), args.version, args.source, args.artifacts)
    except subprocess.CalledProcessError as exc:
        parser.exit(1, f"Draft release command failed: {exc.stderr or exc}\n")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Draft release failed: {exc}\n")
    print(json.dumps(result, indent=2))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(f"\n### Release draft: {result['tag']}\n\n")
            summary.write(result.get("message") or f"[Review draft]({result['url']}) — not published.")
            summary.write("\n")


if __name__ == "__main__":
    main()
