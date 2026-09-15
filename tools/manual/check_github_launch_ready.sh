#!/usr/bin/env bash
#
# check_github_launch_ready.sh -- verify GitHub-side public repository hygiene.
#
# This intentionally lives outside CI: it checks repository settings and public
# GitHub surfaces, so it needs an authenticated `gh` session.
#
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

fail=0
warn_count=0
max_actions_retention_days=14

if [ -n "${NO_COLOR:-}" ] || [ ! -t 1 ]; then
  RED=""
  YELLOW=""
  GREEN=""
  NC=""
else
  RED="$(printf '\033[31m')"
  YELLOW="$(printf '\033[33m')"
  GREEN="$(printf '\033[32m')"
  NC="$(printf '\033[0m')"
fi

note() { printf '  %s[FAIL]%s %s\n' "$RED" "$NC" "$1"; fail=1; }
warn() { printf '  %s[WARN]%s %s\n' "$YELLOW" "$NC" "$1"; warn_count=$((warn_count + 1)); }
ok()   { printf '  %s[OK]%s %s\n' "$GREEN" "$NC" "$1"; }

usage() {
  cat <<'USAGE'
Usage: tools/manual/check_github_launch_ready.sh [--repo OWNER/REPO] [--allow-private]

Checks GitHub-side public repository state that local CI cannot prove:
  - gh authentication
  - repository metadata, topics, and contributor workflow settings
  - repository visibility and collaboration features
  - GitHub Actions enabled with a minimal default token, SHA-pinned actions,
    short artifact retention, and no unreviewed hosted workflow
  - release deployment environments exist with the required reviewer and
    exact main/tag branch policies
  - local reachable git history does not expose public-blocking provenance paths
  - GitHub branch and tag refs do not expose commits outside public git history
  - GitHub pull request refs do not expose commits outside public git history
  - workflow run history does not expose commits outside public git history
  - public repository metadata/label/milestone/release/issue/comment/discussion text has no high-risk leaks
  - public repository metadata/label/milestone/release/issue/comment/discussion text has no stale commit references
  - contributor-facing GitHub labels needed for triage are present
  - GitHub release assets expose no ROM/media/save payload; every 1.7+ release
    has its complete desktop platform set with one consistent Phone Party mode;
    and every Golden-Balloon-<version>-<platform> artifact carries
    checksum/provenance sidecars whose contents match GitHub's artifact digest
    and release tag
  - GitHub Actions artifacts do not expose ROM, media, archive, app, or binary payloads
  - branch protection is readable and does not depend on hosted status checks
  - vulnerability-alert/private-reporting endpoints are available
  - secret-scanning endpoint is available when GitHub exposes it

Use --allow-private only for private mirrors or dry runs. Public repository
checks should omit it.
USAGE
}

repo=""
allow_private=0

# The forbidden-text classes live in one shared data file so this guard and
# tools/ci/check_public_history_text.sh can never drift apart on what counts as
# a leak. Each non-comment line is one regex alternative; they are joined with
# `|` into a single alternation for jq's test().
public_text_denylist='tools/public_text_denylist.txt'
retained_ref_allowlist='tools/public_retained_ref_allowlist.tsv'

public_surface_pattern() {
  local pattern

  if [ ! -f "$public_text_denylist" ]; then
    echo "GitHub public readiness FAILED: missing shared leak ruleset ${public_text_denylist}." >&2
    return 1
  fi
  pattern="$(awk 'NF && $0 !~ /^[[:space:]]*#/ { printf "%s%s", sep, $0; sep = "|" }' "$public_text_denylist")"
  if [ -z "$pattern" ]; then
    echo "GitHub public readiness FAILED: shared leak ruleset ${public_text_denylist} defines no patterns." >&2
    return 1
  fi

  printf '(%s)' "$pattern"
}

append_findings() {
  local existing="$1"
  local new_findings="$2"

  if [ -z "$new_findings" ]; then
    printf '%s' "$existing"
  elif [ -z "$existing" ]; then
    printf '%s' "$new_findings"
  else
    printf '%s\n%s' "$existing" "$new_findings"
  fi
}

public_history_shas() {
  git rev-list HEAD
}

scan_github_public_text_surface() {
  local repo_name="$1"
  local pattern
  local findings=""
  local scan_output
  local owner
  local name

  pattern="$(public_surface_pattern)"

  echo
  echo "== Public GitHub text surface =="

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    "repos/${repo_name}" \
    --jq 'select(((.full_name // "") + "\n" + (.description // "") + "\n" + (.homepage // "")) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "repository\t\(.html_url)\t\(.full_name)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub repository metadata"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/labels?per_page=100" \
    --jq '.[] | select(((.name // "") + "\n" + (.description // "")) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "label\t\(.url)\t\(.name)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub label names and descriptions"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/milestones?state=all&per_page=100" \
    --jq '.[] | select(((.title // "") + "\n" + (.description // "")) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "milestone\t\(.html_url)\t\(.title)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub milestone titles and descriptions"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/releases?per_page=100" \
    --jq '.[] | select(((.tag_name // "") + "\n" + (.name // "") + "\n" + (.body // "") + "\n" + ([(.assets // [])[] | (.name // "")] | join("\n"))) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "release\t\(.html_url)\t\(.tag_name)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub release notes and asset names"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/issues?state=all&per_page=100" \
    --jq '.[] | select(((.title // "") + "\n" + (.body // "")) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "issue-or-pr\t\(.html_url)\t\(.title)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub issue/PR titles and bodies"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/issues/comments?per_page=100" \
    --jq '.[] | select((.body // "") | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "issue-comment\t\(.html_url)\t\(.user.login)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub issue/PR comments"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/comments?per_page=100" \
    --jq '.[] | select((.body // "") | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "commit-comment\t\(.html_url)\t\(.user.login)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub commit comments"
  fi

  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
    --paginate "repos/${repo_name}/pulls/comments?per_page=100" \
    --jq '.[] | select((.body // "") | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "pr-review-comment\t\(.html_url)\t\(.user.login)"' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub PR review comments"
  fi

  pull_numbers=""
  if pull_numbers="$(gh api \
    --paginate "repos/${repo_name}/pulls?state=all&per_page=100" \
    --jq '.[].number' 2>/dev/null)"; then
    while IFS= read -r pr_number; do
      [ -n "$pr_number" ] || continue
      if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api \
        --paginate "repos/${repo_name}/pulls/${pr_number}/reviews?per_page=100" \
        --jq '.[] | select((.body // "") | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) | "pr-review\t\(.html_url // .pull_request_url)\t\(.user.login)"' 2>/dev/null)"; then
        findings="$(append_findings "$findings" "$scan_output")"
      else
        note "could not scan GitHub PR review summaries for pull request #${pr_number}"
      fi
    done <<< "$pull_numbers"
  else
    note "could not list GitHub pull requests for review-summary scan"
  fi

  owner="${repo_name%%/*}"
  name="${repo_name#*/}"
  if scan_output="$(MDKR_PUBLIC_SURFACE_PATTERN="$pattern" gh api graphql --paginate \
    -F owner="$owner" \
    -F name="$name" \
    -f query='
      query($owner: String!, $name: String!, $endCursor: String) {
        repository(owner: $owner, name: $name) {
          discussions(first: 100, after: $endCursor, orderBy: {field: UPDATED_AT, direction: DESC}) {
            pageInfo { hasNextPage endCursor }
            nodes {
              number
              title
              body
              url
              comments(first: 100) {
                totalCount
                nodes {
                  body
                  url
                  author { login }
                }
              }
            }
          }
        }
      }' \
    --jq '
      .data.repository.discussions.nodes[]? as $discussion
      | (
          if ((($discussion.title // "") + "\n" + ($discussion.body // "")) | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i")) then
            "discussion\t\($discussion.url)\t\($discussion.title)"
          else empty end
        ),
        (
          $discussion.comments.nodes[]?
          | select((.body // "") | test(env.MDKR_PUBLIC_SURFACE_PATTERN; "i"))
          | "discussion-comment\t\(.url)\t\(.author.login // "unknown")"
        ),
        (
          if (($discussion.comments.totalCount // 0) > (($discussion.comments.nodes // []) | length)) then
            "discussion-comment-scan-incomplete\t\($discussion.url)\tloaded \((($discussion.comments.nodes // []) | length)) of \($discussion.comments.totalCount) comments"
          else empty end
        )' 2>/dev/null)"; then
    findings="$(append_findings "$findings" "$scan_output")"
  else
    note "could not scan GitHub Discussions text"
  fi

  if [ -n "$findings" ]; then
    note "high-risk text found in public GitHub metadata/label/milestone/release/issue/comment/discussion surface"
    printf '%s\n' "$findings" | sed 's/^/  - /'
  else
    ok "no high-risk text found in repository metadata, labels, milestones, releases, issues, PR/commit comments, PR review summaries, or discussions"
  fi
}

scan_github_release_assets() {
  local repo_name="$1"
  local release_records
  local release_index
  local release_asset_lines
  local asset_index=""
  local blocked_assets=""
  local expected_assets=""
  local missing_release_assets=""
  local missing_sidecars=""
  local invalid_sidecars=""
  local orphan_sidecars=""
  local inconsistent_party_modes=""
  local release_party_modes=""
  local review_assets=""
  local forbidden_asset_pattern
  local release_artifact_pattern
  local sidecar_pattern
  local suffix
  local tag
  local url
  local asset_name
  local asset_size
  local _asset_id
  local asset_api_digest
  local artifact_digest
  local artifact_version
  local artifact_platform
  local expected_signing
  local release_version
  local required_asset
  local missing_for_release
  local macos_present
  local party_mode
  local party_modes
  local party_mode_count
  local tag_commit
  local sidecar_line
  local _sidecar_tag
  local sidecar_url
  local sidecar_name
  local sidecar_size
  local sidecar_id
  local _sidecar_digest
  local metadata_tmp
  local named_asset
  local provenance_errors

  echo
  echo "== GitHub release asset surface =="

  if ! release_records="$(gh api \
    --paginate "repos/${repo_name}/releases?per_page=100" \
    --jq '
      .[] as $release
      | (["release", ($release.tag_name // ""),
          ($release.html_url // ""), "", "", "", ""] | @tsv),
        (($release.assets // [])[]?
         | ["asset", ($release.tag_name // ""),
            ($release.html_url // ""), (.name // ""),
            ((.size // 0) | tostring), ((.id // 0) | tostring),
            (.digest // "")] | @tsv)' 2>/dev/null)"; then
    note "could not scan GitHub release assets"
    return
  fi

  if [ -z "$release_records" ]; then
    ok "no GitHub releases are published"
    return
  fi

  # Keep one row for every release, including an empty draft, separately from
  # the asset rows. The v1.7+ completeness check must be able to name a release
  # whose first producer never uploaded anything at all.
  release_index="$(printf '%s\n' "$release_records" | awk -F '\t' \
    '$1 == "release" { print $2 "\t" $3 }')"
  release_asset_lines="$(printf '%s\n' "$release_records" | awk -F '\t' \
    '$1 == "asset" { sub(/^asset\t/, ""); print }')"

  # A release ships packaged binaries: flagging .dmg/.zip/.tar.gz/.AppImage as
  # such would flag exactly what this project publishes. What must never appear
  # is a ROM or a ROM-derived media/save payload, whatever the container is
  # named. Payload shape is the assertion; the project's own artifact family is
  # recognised by its exact release naming and then held to the provenance
  # contract tools/release/verify_provenance.sh writes (a .sha256 and a
  # .provenance.json beside every artifact).
  forbidden_asset_pattern='(^|[-_.])(base)?rom([-_.]|$)|(^|[-_.])(cdata|eeprom|save|saves|screenshot|screenshots|capture|captures|sfx|soundtrack)([-_.]|$)|\.(z64|n64|v64|rom|cdata|eeprom|rz|ctl|tbl|sbk|seq|aifc|aiff|seg|raw|bmp|png|jpg|jpeg|gif|webp|wav|mp3|ogg|flac|m4a|aac|mp4|mov|m4v|mkv|avi|webm|jsonl)$'
  release_artifact_pattern='^Golden-Balloon-([0-9]+(\.[0-9]+){1,2}|dev)-(macos-arm64-(unsigned|signed-notarized)\.dmg|windows-x64\.zip|linux-x86_64\.AppImage|linux-x86_64\.tar\.gz)$'
  sidecar_pattern='^Golden-Balloon-.*(\.sha256|\.provenance\.json)$'

  while IFS=$'\t' read -r tag url asset_name asset_size _asset_id asset_api_digest; do
    [ -n "$asset_name" ] || continue
    asset_index="$(append_findings "$asset_index" "${tag}"$'\t'"${asset_name}")"
  done <<< "$release_asset_lines"

  # From 1.7 onward, a release is a platform set rather than an arbitrary
  # collection of individually valid files. Require both Linux formats,
  # Windows, and at least one of the policy-recognised macOS formats. Sidecar
  # completeness for every present primary is checked in the main loop below.
  while IFS=$'\t' read -r tag url; do
    [ -n "$tag" ] || continue
    release_version="${tag#v}"
    if [[ ! "$tag" =~ ^v[0-9]+(\.[0-9]+){1,2}$ ]] || \
       ! awk -F. 'BEGIN { exit ! ($1 + 0 > 1 || ($1 + 0 == 1 && $2 + 0 >= 7)) }' \
         <<< "$release_version"; then
      continue
    fi

    missing_for_release=""
    for required_asset in \
      "Golden-Balloon-${release_version}-linux-x86_64.AppImage" \
      "Golden-Balloon-${release_version}-linux-x86_64.tar.gz" \
      "Golden-Balloon-${release_version}-windows-x64.zip"; do
      if ! grep -Fqx "${tag}"$'\t'"${required_asset}" <<< "$asset_index"; then
        missing_for_release="$(append_findings "$missing_for_release" "$required_asset")"
      fi
    done
    macos_present=0
    for required_asset in \
      "Golden-Balloon-${release_version}-macos-arm64-unsigned.dmg" \
      "Golden-Balloon-${release_version}-macos-arm64-signed-notarized.dmg"; do
      if grep -Fqx "${tag}"$'\t'"${required_asset}" <<< "$asset_index"; then
        macos_present=1
      fi
    done
    if [ "$macos_present" -eq 0 ]; then
      missing_for_release="$(append_findings "$missing_for_release" \
        "Golden-Balloon-${release_version}-macos-arm64-{unsigned,signed-notarized}.dmg (one required)")"
    fi
    while IFS= read -r required_asset; do
      [ -n "$required_asset" ] || continue
      missing_release_assets="$(append_findings "$missing_release_assets" \
        "${tag}"$'\t'"${required_asset}"$'\t'"${url}")"
    done <<< "$missing_for_release"
  done <<< "$release_index"

  while IFS=$'\t' read -r tag url asset_name asset_size _asset_id asset_api_digest; do
    [ -n "$asset_name" ] || continue
    if printf '%s\n' "$asset_name" | grep -Eiq "$forbidden_asset_pattern"; then
      blocked_assets="$(append_findings "$blocked_assets" "${tag}"$'\t'"${asset_name}"$'\t'"${asset_size} bytes"$'\t'"${url}")"
    elif printf '%s\n' "$asset_name" | grep -Eq "$release_artifact_pattern"; then
      expected_assets="$(append_findings "$expected_assets" "${tag}"$'\t'"${asset_name}"$'\t'"${asset_size} bytes")"

      artifact_version="$(printf '%s\n' "$asset_name" | sed -E 's/^Golden-Balloon-([0-9]+(\.[0-9]+){1,2}|dev)-.*/\1/')"
      case "$asset_name" in
        *-macos-arm64-unsigned.dmg)
          artifact_platform="macos"
          expected_signing="ad-hoc-unsigned"
          ;;
        *-macos-arm64-signed-notarized.dmg)
          artifact_platform="macos"
          expected_signing="developer-id-notarized"
          ;;
        *-windows-x64.zip)
          artifact_platform="windows"
          expected_signing=""
          ;;
        *-linux-x86_64.AppImage|*-linux-x86_64.tar.gz)
          artifact_platform="linux"
          expected_signing=""
          ;;
      esac

      if [ "$artifact_version" = "dev" ] || [ "$tag" != "v${artifact_version}" ]; then
        invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${asset_name}"$'\t'"release tag does not match artifact version"$'\t'"${url}")"
      fi
      tag_commit="$(gh api "repos/${repo_name}/commits/${tag}" --jq '.sha // ""' 2>/dev/null || true)"
      if ! printf '%s\n' "$tag_commit" | grep -Eq '^[0-9a-f]{40}$'; then
        invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${asset_name}"$'\t'"release tag does not resolve to a commit"$'\t'"${url}")"
      fi
      if ! printf '%s\n' "$asset_api_digest" | grep -Eq '^sha256:[0-9a-f]{64}$'; then
        invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${asset_name}"$'\t'"GitHub API exposes no authoritative SHA-256 digest"$'\t'"${url}")"
        artifact_digest=""
      else
        artifact_digest="${asset_api_digest#sha256:}"
      fi

      for suffix in .sha256 .provenance.json; do
        sidecar_line="$(awk -F '\t' -v tag="$tag" -v name="${asset_name}${suffix}" \
          '$1 == tag && $3 == name && !found { print; found = 1 }' \
          <<< "$release_asset_lines")"
        if [ -z "$sidecar_line" ]; then
          missing_sidecars="$(append_findings "$missing_sidecars" "${tag}"$'\t'"${asset_name}"$'\t'"no ${suffix} sidecar"$'\t'"${url}")"
          continue
        fi

        IFS=$'\t' read -r _sidecar_tag sidecar_url sidecar_name sidecar_size sidecar_id _sidecar_digest <<< "$sidecar_line"
        case "$suffix" in
          .sha256)
            if [ "$sidecar_size" -gt 512 ]; then
              invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"checksum sidecar exceeds 512 bytes"$'\t'"${sidecar_url}")"
              continue
            fi
            ;;
          .provenance.json)
            if [ "$sidecar_size" -gt 16384 ]; then
              invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"provenance sidecar exceeds 16384 bytes"$'\t'"${sidecar_url}")"
              continue
            fi
            ;;
        esac

        metadata_tmp="$(mktemp "${TMPDIR:-/tmp}/mdkr64-release-metadata.XXXXXX")"
        if ! gh api -H 'Accept: application/octet-stream' \
          "repos/${repo_name}/releases/assets/${sidecar_id}" > "$metadata_tmp" 2>/dev/null; then
          invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"could not download sidecar"$'\t'"${sidecar_url}")"
          rm -f "$metadata_tmp"
          continue
        fi

        if [ "$suffix" = ".sha256" ]; then
          if [ -z "$artifact_digest" ] || ! printf '%s  %s\n' "$artifact_digest" "$asset_name" | cmp -s - "$metadata_tmp"; then
            invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"checksum content does not exactly match GitHub's artifact digest and basename"$'\t'"${sidecar_url}")"
          fi
        else
          if [ -z "$artifact_digest" ]; then
            provenance_errors="artifact has no authoritative GitHub digest"
          elif ! provenance_errors="$(jq -r \
            --arg artifact "$asset_name" \
            --arg commit "$tag_commit" \
            --arg platform "$artifact_platform" \
            --arg sha256 "$artifact_digest" \
            --arg version "$artifact_version" \
            'if type != "object" then
               "JSON root is not an object"
             else
               [if .schema == "mdkr64-provenance/1" then empty else "schema" end,
                if .artifact == $artifact then empty else "artifact name" end,
                if .commit == $commit then empty else "release tag commit" end,
                if .platform == $platform then empty else "platform" end,
                if .sha256 == $sha256 then empty else "artifact digest" end,
                if .source_dirty == false then empty else "clean-source flag" end,
                if .version == $version then empty else "version" end]
               | join(", ")
             end' "$metadata_tmp" 2>/dev/null)"; then
            provenance_errors="unparseable JSON"
          fi
          if [ -n "$provenance_errors" ]; then
            invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"provenance mismatch: ${provenance_errors}"$'\t'"${sidecar_url}")"
          fi
          if awk -F. 'BEGIN { exit ! ($1 + 0 > 1 || ($1 + 0 == 1 && $2 + 0 >= 7)) }' <<< "$artifact_version"; then
            if ! jq -e '.phone_party == "partyless" or .phone_party == "cloud-enabled"' \
              "$metadata_tmp" >/dev/null 2>&1; then
              invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"1.7+ provenance has no valid Phone Party mode"$'\t'"${sidecar_url}")"
            else
              party_mode="$(jq -r '.phone_party' "$metadata_tmp")"
              release_party_modes="$(append_findings "$release_party_modes" \
                "${tag}"$'\t'"${party_mode}"$'\t'"${asset_name}")"
            fi
            if [ -n "$expected_signing" ] && \
               ! jq -e --arg signing "$expected_signing" '.macos_signing == $signing' \
                 "$metadata_tmp" >/dev/null 2>&1; then
              invalid_sidecars="$(append_findings "$invalid_sidecars" "${tag}"$'\t'"${sidecar_name}"$'\t'"1.7+ macOS provenance has the wrong signing mode"$'\t'"${sidecar_url}")"
            fi
          fi
        fi
        rm -f "$metadata_tmp"
      done
    elif printf '%s\n' "$asset_name" | grep -Eq "$sidecar_pattern"; then
      named_asset="${asset_name%.provenance.json}"
      named_asset="${named_asset%.sha256}"
      if ! grep -Fqx "${tag}"$'\t'"${named_asset}" <<< "$asset_index"; then
        orphan_sidecars="$(append_findings "$orphan_sidecars" "${tag}"$'\t'"${asset_name}"$'\t'"sidecar names no artifact in this release"$'\t'"${url}")"
      fi
    else
      review_assets="$(append_findings "$review_assets" "${tag}"$'\t'"${asset_name}"$'\t'"${asset_size} bytes"$'\t'"${url}")"
    fi
  done <<< "$release_asset_lines"

  # The portable workflow and the separately dispatched macOS workflow read a
  # mutable repository variable at different times. Individually valid
  # sidecars are therefore insufficient: every artifact attached to one 1.7+
  # release must disclose the same Phone Party capability.
  while IFS=$'\t' read -r tag url; do
    [ -n "$tag" ] || continue
    release_version="${tag#v}"
    if [[ ! "$tag" =~ ^v[0-9]+(\.[0-9]+){1,2}$ ]] || \
       ! awk -F. 'BEGIN { exit ! ($1 + 0 > 1 || ($1 + 0 == 1 && $2 + 0 >= 7)) }' \
         <<< "$release_version"; then
      continue
    fi
    party_modes="$(awk -F '\t' -v release_tag="$tag" \
      '$1 == release_tag { print $2 }' <<< "$release_party_modes" | \
      awk 'NF' | LC_ALL=C sort -u)"
    party_mode_count="$(awk 'NF { count++ } END { print count + 0 }' \
      <<< "$party_modes")"
    if [ "$party_mode_count" -gt 1 ]; then
      inconsistent_party_modes="$(append_findings "$inconsistent_party_modes" \
        "${tag}"$'\t'"$(paste -sd, <<< "$party_modes")"$'\t'"${url}")"
    fi
  done <<< "$release_index"

  if [ -n "$blocked_assets" ]; then
    note "GitHub release assets include ROM/media/save-shaped payloads"
    printf '%s\n' "$blocked_assets" | sed 's/^/  - /'
  fi

  if [ -n "$missing_release_assets" ]; then
    note "1.7+ releases are missing required platform artifacts"
    printf '%s\n' "$missing_release_assets" | sed 's/^/  - /'
  fi

  if [ -n "$missing_sidecars" ]; then
    note "published release artifacts are missing checksum/provenance sidecars"
    printf '%s\n' "$missing_sidecars" | sed 's/^/  - /'
  fi

  if [ -n "$invalid_sidecars" ]; then
    note "published release checksum/provenance sidecars do not match authoritative GitHub artifact and tag metadata"
    printf '%s\n' "$invalid_sidecars" | sed 's/^/  - /'
  fi

  if [ -n "$orphan_sidecars" ]; then
    note "published release sidecars name artifacts that are not present in the same release"
    printf '%s\n' "$orphan_sidecars" | sed 's/^/  - /'
  fi

  if [ -n "$inconsistent_party_modes" ]; then
    note "1.7+ release artifacts disagree on Phone Party mode"
    printf '%s\n' "$inconsistent_party_modes" | sed 's/^/  - /'
  fi

  if [ -n "$review_assets" ]; then
    warn "GitHub release assets outside the Golden-Balloon-<version>-<platform> family are published; manually verify they contain no generated or ROM-derived payload"
    printf '%s\n' "$review_assets" | sed 's/^/  - /'
  fi

  if [ -n "$expected_assets" ] && [ -z "$missing_sidecars" ] && \
     [ -z "$invalid_sidecars" ] && [ -z "$orphan_sidecars" ] && \
     [ -z "$missing_release_assets" ] && \
     [ -z "$inconsistent_party_modes" ]; then
    ok "published release artifacts match the project's naming and each checksum/provenance sidecar is content-valid"
    printf '%s\n' "$expected_assets" | sed 's/^/  - /'
  elif [ -z "$blocked_assets" ] && [ -z "$review_assets" ] && \
       [ -z "$missing_sidecars" ] && [ -z "$invalid_sidecars" ] && \
       [ -z "$orphan_sidecars" ] && [ -z "$missing_release_assets" ] && \
       [ -z "$inconsistent_party_modes" ]; then
    ok "GitHub release assets do not need review"
  fi
}

scan_github_actions_artifacts() {
  local repo_name="$1"
  local artifact_lines
  local blocked_artifacts=""
  local review_artifacts=""
  local forbidden_artifact_pattern
  local name
  local size
  local url
  local sha
  local workflow_url

  echo
  echo "== GitHub Actions artifact surface =="

  if ! artifact_lines="$(gh api \
    --paginate "repos/${repo_name}/actions/artifacts?per_page=100" \
    --jq '.artifacts[]? | select(.expired != true) | [(.name // ""), ((.size_in_bytes // 0) | tostring), (.archive_download_url // ""), ((.workflow_run.head_sha // "")[0:12]), (.workflow_run.html_url // "")] | @tsv' 2>/dev/null)"; then
    note "could not scan GitHub Actions artifacts"
    return
  fi

  if [ -z "$artifact_lines" ]; then
    ok "no unexpired GitHub Actions artifacts are published"
    return
  fi

  forbidden_artifact_pattern='(^|[-_.])(base)?rom([-_.]|$)|(^|[-_.])(cdata|eeprom|save|saves|screenshot|screenshots|capture|captures|audio|music|sfx|dmg|pkg|app|binary|binaries|exe|library|lib|universal)([-_.]|$)|\.(z64|n64|v64|rom|cdata|eeprom|rz|ctl|tbl|sbk|seq|aifc|aiff|seg|raw|bmp|png|jpg|jpeg|gif|webp|wav|mp3|ogg|flac|m4a|aac|mp4|mov|m4v|mkv|avi|webm|jsonl|dmg|pkg|app|zip|7z|tar|tgz|gz|bz2|xz|exe|msi|dll|so|dylib|a)$'

  while IFS=$'\t' read -r name size url sha workflow_url; do
    [ -n "$name" ] || continue
    if printf '%s\n' "$name" | grep -Eiq "$forbidden_artifact_pattern"; then
      blocked_artifacts="$(append_findings "$blocked_artifacts" "${name}\t${size} bytes\thead=${sha:-unknown}\t${workflow_url:-$url}")"
    else
      review_artifacts="$(append_findings "$review_artifacts" "${name}\t${size} bytes\thead=${sha:-unknown}\t${workflow_url:-$url}")"
    fi
  done <<< "$artifact_lines"

  if [ -n "$blocked_artifacts" ]; then
    note "GitHub Actions artifacts include ROM/media/archive/app/binary-shaped payloads"
    printf '%s\n' "$blocked_artifacts" | sed 's/^/  - /'
  fi

  if [ -n "$review_artifacts" ]; then
    warn "GitHub Actions artifacts are published; manually verify they contain only logs or other non-generated text"
    printf '%s\n' "$review_artifacts" | sed 's/^/  - /'
  elif [ -z "$blocked_artifacts" ]; then
    ok "GitHub Actions artifacts do not need review"
  fi
}

scan_github_launch_labels() {
  local repo_name="$1"
  local labels
  local missing=""
  local label

  echo
  echo "== Contributor label surface =="

  if ! labels="$(gh api --paginate "repos/${repo_name}/labels?per_page=100" --jq '.[].name' 2>/dev/null)"; then
    note "could not scan GitHub labels"
    return
  fi

  for label in \
    audio \
    bug \
    build \
    documentation \
    "good first issue" \
    "help wanted" \
    macos \
    matching \
    parity \
    provenance \
    release-hygiene \
    renderer \
    validation; do
    if printf '%s\n' "$labels" | grep -Fxq "$label"; then
      :
    else
      missing="${missing}"$'\n'"${label}"
    fi
  done

  if [ -n "$missing" ]; then
    warn "recommended contributor triage label(s) are missing"
    printf '%s\n' "$missing" | awk 'NF { print "  - " $0 }'
  else
    ok "recommended contributor triage labels are present"
  fi
}

scan_github_deployment_environments() {
  local repo_name="$1"
  local environment_json
  local reviewer_count
  local custom_policies
  local actual_policies
  local expected_policies

  echo
  echo "== Deployment environments =="

  if environment_json="$(gh api \
      "repos/${repo_name}/environments/macos-release" 2>/dev/null)"; then
    reviewer_count="$(printf '%s' "$environment_json" | jq \
      '[.protection_rules[]? | select(.type == "required_reviewers") | .reviewers[]?] | length')"
    if [ "$reviewer_count" -gt 0 ]; then
      ok "macos-release requires ${reviewer_count} environment reviewer(s)"
    else
      note "macos-release has no required-reviewer protection rule"
    fi
    custom_policies="$(printf '%s' "$environment_json" | jq -r \
      '.deployment_branch_policy.custom_branch_policies // false')"
    if [ "$custom_policies" = "true" ]; then
      ok "macos-release uses custom deployment branch/tag policies"
    else
      note "macos-release does not use custom deployment branch/tag policies"
    fi
    if actual_policies="$(gh api --paginate \
        "repos/${repo_name}/environments/macos-release/deployment-branch-policies?per_page=100" \
        --jq '.branch_policies[]? | [.type, .name] | @tsv' 2>/dev/null)"; then
      actual_policies="$(printf '%s\n' "$actual_policies" | awk 'NF' | LC_ALL=C sort)"
      expected_policies="$(printf 'branch\tmain\ntag\tv*\n' | LC_ALL=C sort)"
      if [ "$actual_policies" = "$expected_policies" ]; then
        ok "macos-release permits only public main candidates and v* tags"
      else
        note "macos-release deployment policies must be exactly branch main and tag v*"
        printf '%s\n' "$actual_policies" | sed 's/^/  - actual: /'
      fi
    else
      note "could not read macos-release deployment branch/tag policies"
    fi
  else
    note "macos-release deployment environment is missing or unreadable"
  fi

  if environment_json="$(gh api \
      "repos/${repo_name}/environments/github-pages" 2>/dev/null)"; then
    custom_policies="$(printf '%s' "$environment_json" | jq -r \
      '.deployment_branch_policy.custom_branch_policies // false')"
    if [ "$custom_policies" = "true" ]; then
      ok "github-pages uses custom deployment tag policies"
    else
      note "github-pages does not use custom deployment tag policies"
    fi
    if actual_policies="$(gh api --paginate \
        "repos/${repo_name}/environments/github-pages/deployment-branch-policies?per_page=100" \
        --jq '.branch_policies[]? | [.type, .name] | @tsv' 2>/dev/null)"; then
      actual_policies="$(printf '%s\n' "$actual_policies" | awk 'NF' | LC_ALL=C sort)"
      expected_policies=$'tag\tv*'
      if [ "$actual_policies" = "$expected_policies" ]; then
        ok "github-pages permits only v* tags"
      else
        note "github-pages deployment policy must be exactly tag v*"
        printf '%s\n' "$actual_policies" | sed 's/^/  - actual: /'
      fi
    else
      note "could not read github-pages deployment tag policies"
    fi
  else
    note "github-pages deployment environment is missing or unreadable"
  fi
}

scan_github_workflow_run_history() {
  local repo_name="$1"
  local reachable_shas
  local run_lines
  local stale_runs=""
  local run_count=0
  local run_id
  local sha
  local title
  local url

  echo
  echo "== Workflow run history surface =="

  reachable_shas="$(public_history_shas)"

  if ! run_lines="$(gh run list --repo "$repo_name" --limit 1000 \
    --json databaseId,headSha,displayTitle,url \
    --jq '.[] | [.databaseId, .headSha, .displayTitle, .url] | @tsv' 2>/dev/null)"; then
    note "could not scan GitHub workflow run history"
    return
  fi

  while IFS=$'\t' read -r run_id sha title url; do
    [ -n "$run_id" ] || continue
    run_count=$((run_count + 1))
    if [ -z "$sha" ]; then
      stale_runs="$(append_findings "$stale_runs" "run ${run_id}\t(no head SHA)\t${url}")"
    elif ! printf '%s\n' "$reachable_shas" | grep -Fqx "$sha"; then
      stale_runs="$(append_findings "$stale_runs" "run ${run_id}\t${sha:0:12}\t${title}\t${url}")"
    fi
  done <<< "$run_lines"

  if [ -n "$stale_runs" ]; then
    note "workflow runs reference commits outside current public git history"
    printf '%s\n' "$stale_runs" | sed 's/^/  - /'
  else
    ok "no workflow runs reference commits outside current git history"
  fi

  if [ "$run_count" -ge 1000 ]; then
    warn "only scanned the latest 1000 workflow runs; delete old runs or extend the audit before release"
  fi
}

scan_github_public_commit_refs() {
  local repo_name="$1"
  local scan_output

  echo
  echo "== Public GitHub commit-reference surface =="

  if scan_output="$(python3 tools/check_github_public_commit_refs.py \
      --repo "$repo_name" --reviewed-refs "$retained_ref_allowlist" 2>&1)"; then
    ok "$scan_output"
  else
    note "public GitHub text references stale or unverified commits"
    printf '%s\n' "$scan_output" | sed 's/^/  /'
  fi
}

scan_local_public_tree() {
  local scan_output

  echo
  echo "== Local public tracked-tree boundary =="

  if scan_output="$(python3 tools/check_public_surface.py --tree 2>&1)"; then
    ok "$scan_output"
  else
    note "local tracked tree exposes private/high-risk material"
    printf '%s\n' "$scan_output" | sed 's/^/  /'
  fi
}

scan_github_branch_tag_refs() {
  local repo_name="$1"
  local remote_url
  local reachable_shas
  local ref_lines
  local peeled_tag_refs=""
  local stale_refs=""
  local ref_count=0
  local sha
  local ref
  local base_ref

  echo
  echo "== Branch and tag ref surface =="

  reachable_shas="$(public_history_shas)"
  remote_url="$(gh repo view "$repo_name" --json sshUrl --jq '.sshUrl // ""' 2>/dev/null || true)"
  if [ -z "$remote_url" ]; then
    remote_url="https://github.com/${repo_name}.git"
  fi

  if ! ref_lines="$(git ls-remote "$remote_url" 'refs/heads/*' 'refs/tags/*' 2>/dev/null)"; then
    note "could not scan GitHub branch and tag refs"
    return
  fi

  while IFS=$'\t' read -r sha ref; do
    [ -n "$sha" ] || continue
    case "$ref" in
      *'^{}')
        base_ref="${ref%^\{\}}"
        peeled_tag_refs="$(append_findings "$peeled_tag_refs" "$base_ref")"
        ;;
    esac
  done <<< "$ref_lines"

  while IFS=$'\t' read -r sha ref; do
    [ -n "$sha" ] || continue
    case "$ref" in
      *'^{}')
        ref_count=$((ref_count + 1))
        base_ref="${ref%^\{\}}"
        if ! printf '%s\n' "$reachable_shas" | grep -Fqx "$sha"; then
          stale_refs="$(append_findings "$stale_refs" "${base_ref} ${sha:0:12}")"
        fi
        ;;
      refs/tags/*)
        if printf '%s\n' "$peeled_tag_refs" | grep -Fqx "$ref"; then
          :
        else
          ref_count=$((ref_count + 1))
          if ! printf '%s\n' "$reachable_shas" | grep -Fqx "$sha"; then
            stale_refs="$(append_findings "$stale_refs" "${ref} ${sha:0:12}")"
          fi
        fi
        ;;
      *)
        ref_count=$((ref_count + 1))
        if ! printf '%s\n' "$reachable_shas" | grep -Fqx "$sha"; then
          stale_refs="$(append_findings "$stale_refs" "${ref} ${sha:0:12}")"
        fi
        ;;
    esac
  done <<< "$ref_lines"

  if [ -n "$stale_refs" ]; then
    note "branch or tag refs expose commits outside current public git history"
    printf '%s\n' "$stale_refs" | sed 's/^/  - /'
  elif [ "$ref_count" -eq 0 ]; then
    warn "no branch or tag refs are advertised"
  else
    ok "all advertised branch and tag refs are reachable from current git history"
  fi
}

scan_github_pull_refs() {
  local repo_name="$1"
  local remote_url
  local reachable_shas
  local ref_lines
  local stale_refs=""
  local reviewed_refs=""
  local allowlist_lines
  local invalid_allowlist
  local duplicate_allowlist_refs
  local missing_allowlist=""
  local allow_ref
  local allow_sha
  local ref_count=0
  local sha
  local ref

  echo
  echo "== Pull request ref surface =="

  reachable_shas="$(public_history_shas)"
  remote_url="$(gh repo view "$repo_name" --json sshUrl --jq '.sshUrl // ""' 2>/dev/null || true)"
  if [ -z "$remote_url" ]; then
    remote_url="https://github.com/${repo_name}.git"
  fi

  if ! ref_lines="$(git ls-remote "$remote_url" 'refs/pull/*' 2>/dev/null)"; then
    note "could not scan GitHub pull request refs"
    return
  fi

  if [ ! -f "$retained_ref_allowlist" ]; then
    note "retained pull-ref allowlist is missing: $retained_ref_allowlist"
    return
  fi
  allowlist_lines="$(awk 'NF && $0 !~ /^[[:space:]]*#/' "$retained_ref_allowlist")"
  invalid_allowlist="$(printf '%s\n' "$allowlist_lines" | awk -F '\t' '
    NF != 2 ||
    $1 !~ /^refs\/pull\/[0-9][0-9]*\/(head|merge)$/ ||
    length($2) != 40 || $2 !~ /^[0-9a-f]+$/ { print }
  ')"
  if [ -n "$invalid_allowlist" ]; then
    note "retained pull-ref allowlist has malformed rows"
    printf '%s\n' "$invalid_allowlist" | sed 's/^/  - /'
    return
  fi
  duplicate_allowlist_refs="$(printf '%s\n' "$allowlist_lines" | \
    awk -F '\t' 'NF { print $1 }' | LC_ALL=C sort | uniq -d)"
  if [ -n "$duplicate_allowlist_refs" ]; then
    note "retained pull-ref allowlist repeats refs"
    printf '%s\n' "$duplicate_allowlist_refs" | sed 's/^/  - /'
    return
  fi

  while IFS=$'\t' read -r sha ref; do
    [ -n "$sha" ] || continue
    ref_count=$((ref_count + 1))
    if ! printf '%s\n' "$reachable_shas" | grep -Fqx "$sha"; then
      if printf '%s\n' "$allowlist_lines" | grep -Fqx "${ref}"$'\t'"${sha}"; then
        reviewed_refs="$(append_findings "$reviewed_refs" "${ref} ${sha:0:12}")"
      else
        stale_refs="$(append_findings "$stale_refs" "${ref} ${sha:0:12}")"
      fi
    fi
  done <<< "$ref_lines"

  while IFS=$'\t' read -r allow_ref allow_sha; do
    [ -n "$allow_ref" ] || continue
    if ! printf '%s\n' "$ref_lines" | grep -Fqx "${allow_sha}"$'\t'"${allow_ref}"; then
      missing_allowlist="$(append_findings "$missing_allowlist" "${allow_ref} ${allow_sha:0:12}")"
    fi
  done <<< "$allowlist_lines"

  if [ -n "$stale_refs" ]; then
    note "unreviewed pull request refs expose commits outside current public git history"
    printf '%s\n' "$stale_refs" | sed 's/^/  - /'
  fi
  if [ -n "$missing_allowlist" ]; then
    note "retained pull-ref allowlist does not match GitHub's advertised refs"
    printf '%s\n' "$missing_allowlist" | sed 's/^/  - /'
  fi
  if [ -n "$reviewed_refs" ]; then
    ok "retained pull request refs match exact full-history-reviewed commits"
    printf '%s\n' "$reviewed_refs" | sed 's/^/  - /'
  fi
  if [ -z "$stale_refs" ] && [ -z "$missing_allowlist" ] && [ "$ref_count" -eq 0 ]; then
    ok "no pull request refs are advertised"
  elif [ -z "$stale_refs" ] && [ -z "$missing_allowlist" ]; then
    ok "all advertised pull request refs are reachable or exact reviewed retained refs"
  fi
  echo "  GitHub keeps closed PR refs read-only; every unreachable entry must match the exact reviewed allowlist."
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      repo="$2"
      shift 2
      ;;
    --allow-private)
      allow_private=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub public readiness FAILED: gh CLI is not installed." >&2
  exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "GitHub public readiness FAILED: jq is not installed." >&2
  exit 1
fi

echo "== GitHub public readiness =="

if gh auth status -h github.com >/dev/null 2>&1; then
  ok "gh is authenticated for github.com"
else
  note "gh is not authenticated for github.com"
fi

if [ -z "$repo" ]; then
  repo="$(gh repo view --json nameWithOwner --jq '.nameWithOwner' 2>/dev/null || true)"
fi
if [ -z "$repo" ]; then
  note "could not resolve GitHub repository; pass --repo OWNER/REPO"
else
  ok "repository resolved: $repo"
fi

head_sha="$(git rev-parse HEAD)"
branch="$(git branch --show-current)"

if [ "$branch" = "main" ]; then
  ok "current branch is main"
else
  note "current branch is '$branch', expected main"
fi

if [ -n "$repo" ]; then
  remote_url_for_head="$(gh repo view "$repo" --json sshUrl --jq '.sshUrl // ""' 2>/dev/null || true)"
  if [ -z "$remote_url_for_head" ]; then
    remote_url_for_head="https://github.com/${repo}.git"
  fi
  github_main_sha="$(git ls-remote "$remote_url_for_head" 'refs/heads/main' 2>/dev/null | awk '{ print $1 }')"
  if [ -n "$github_main_sha" ] && [ "$head_sha" = "$github_main_sha" ]; then
    ok "HEAD matches GitHub main ($head_sha)"
  elif [ -n "$github_main_sha" ]; then
    note "HEAD ($head_sha) does not match GitHub main ($github_main_sha)"
  else
    note "could not read GitHub main ref from $remote_url_for_head"
  fi

  echo
  echo "== Repository settings =="
  is_private="$(gh repo view "$repo" --json isPrivate --jq '.isPrivate' 2>/dev/null || echo unknown)"
  has_issues="$(gh repo view "$repo" --json hasIssuesEnabled --jq '.hasIssuesEnabled' 2>/dev/null || echo unknown)"
  has_discussions="$(gh repo view "$repo" --json hasDiscussionsEnabled --jq '.hasDiscussionsEnabled' 2>/dev/null || echo unknown)"
  has_wiki="$(gh repo view "$repo" --json hasWikiEnabled --jq '.hasWikiEnabled' 2>/dev/null || echo unknown)"
  description="$(gh repo view "$repo" --json description --jq '.description // ""' 2>/dev/null || true)"
  topics="$(gh repo view "$repo" --json repositoryTopics --jq '.repositoryTopics[].name' 2>/dev/null || true)"
  remote_default_branch="$(gh api "repos/${repo}" --jq '.default_branch // ""' 2>/dev/null || true)"
  allow_update_branch="$(gh api "repos/${repo}" --jq '.allow_update_branch // false' 2>/dev/null || echo false)"
  delete_branch_on_merge="$(gh api "repos/${repo}" --jq '.delete_branch_on_merge // false' 2>/dev/null || echo false)"
  allow_squash_merge="$(gh api "repos/${repo}" --jq '.allow_squash_merge // false' 2>/dev/null || echo false)"
  allow_merge_commit="$(gh api "repos/${repo}" --jq '.allow_merge_commit // false' 2>/dev/null || echo false)"
  allow_rebase_merge="$(gh api "repos/${repo}" --jq '.allow_rebase_merge // false' 2>/dev/null || echo false)"

  case "$is_private" in
    false) ok "repository is public" ;;
    true)
      if [ "$allow_private" -eq 1 ]; then
        warn "repository is private and --allow-private was supplied"
      else
        note "repository is private; public repository hygiene checks expect public visibility"
      fi
      ;;
    *) note "could not determine repository visibility" ;;
  esac

  [ "$has_issues" = "true" ] && ok "Issues are enabled" || note "Issues are not enabled"
  [ "$has_discussions" = "true" ] && ok "Discussions are enabled" || note "Discussions are not enabled"
  [ "$has_wiki" = "false" ] && ok "Wiki is disabled" || warn "Wiki is enabled; verify this is intentional"
  [ "$remote_default_branch" = "main" ] && ok "default branch is main" || note "default branch is '$remote_default_branch', expected main"
  [ "$delete_branch_on_merge" = "true" ] && ok "delete branch on merge is enabled" || warn "delete branch on merge is not enabled"
  [ "$allow_update_branch" = "true" ] && ok "PR update-branch button is enabled" || warn "PR update-branch button is not enabled"
  [ "$allow_squash_merge" = "true" ] && ok "squash merge is enabled" || warn "squash merge is not enabled"
  [ "$allow_merge_commit" = "true" ] && ok "merge commit is enabled" || warn "merge commit is not enabled"
  [ "$allow_rebase_merge" = "true" ] && ok "rebase merge is enabled" || warn "rebase merge is not enabled"

  if [ -n "$description" ]; then
    ok "repository description is set"
    case "$(printf '%s' "$description" | tr '[:upper:]' '[:lower:]')" in
      *"bring your own rom"*|*"no copyrighted assets"*|*"no game data"*)
        ok "repository description mentions asset/ROM expectations"
        ;;
      *)
        warn "repository description does not mention bring-your-own-ROM/no-assets expectations"
        ;;
    esac
  else
    note "repository description is empty"
  fi

  missing_topics=""
  for topic in bring-your-own-rom decompilation game-preservation n64 native-port source-port; do
    if printf '%s\n' "$topics" | grep -Fxq "$topic"; then
      :
    else
      missing_topics="${missing_topics} ${topic}"
    fi
  done
  if [ -z "$missing_topics" ]; then
    ok "required repository topics are present"
  else
    warn "missing recommended repository topic(s):${missing_topics}"
  fi

  actions_enabled="$(gh api "repos/${repo}/actions/permissions" --jq 'if has("enabled") then (.enabled | tostring) else "unknown" end' 2>/dev/null || echo unknown)"
  actions_allowed="$(gh api "repos/${repo}/actions/permissions" --jq '.allowed_actions // "unknown"' 2>/dev/null || echo unknown)"
  actions_sha_pinning="$(gh api "repos/${repo}/actions/permissions" --jq 'if has("sha_pinning_required") then (.sha_pinning_required | tostring) else "unknown" end' 2>/dev/null || echo unknown)"
  workflow_default_permissions="$(gh api "repos/${repo}/actions/permissions/workflow" --jq '.default_workflow_permissions // "unknown"' 2>/dev/null || echo unknown)"
  workflow_can_approve_prs="$(gh api "repos/${repo}/actions/permissions/workflow" --jq 'if has("can_approve_pull_request_reviews") then (.can_approve_pull_request_reviews | tostring) else "unknown" end' 2>/dev/null || echo unknown)"
  actions_retention_days="$(gh api "repos/${repo}/actions/permissions/artifact-and-log-retention" --jq '.days // "unknown"' 2>/dev/null || echo unknown)"
  # Actions must be ENABLED: correctness.yml is the hosted push/pull_request
  # ROM-free source gate, and release.yml / windows-validate.yml /
  # macos-release.yml are workflow_dispatch lanes a maintainer runs. What
  # matters is not whether the feature is on, but that the token it hands those
  # lanes is minimal, that third-party actions are SHA-pinned, that artifacts
  # expire quickly, and that no workflow exists on GitHub outside the tracked
  # set plus the exact GitHub-managed Dependabot surface below.
  case "$actions_enabled" in
    true)
      ok "GitHub Actions are enabled, as the hosted correctness lane and the dispatch release lanes require"
      ;;
    false)
      note "GitHub Actions are disabled; correctness.yml's push/pull_request gate and the dispatch release lanes cannot run"
      ;;
    *)
      note "could not determine whether GitHub Actions are enabled"
      ;;
  esac
  case "$actions_allowed" in
    all|selected) ok "GitHub Actions allowed-actions policy is ${actions_allowed}" ;;
    *) warn "could not confirm GitHub Actions allowed-actions policy" ;;
  esac
  [ "$actions_sha_pinning" = "true" ] && ok "GitHub Actions require full-SHA action pins" || note "GitHub Actions do not require full-SHA action pins"
  [ "$workflow_default_permissions" = "read" ] && ok "default GitHub Actions token permissions are read-only" || note "default GitHub Actions token permissions are '${workflow_default_permissions}', expected read"
  [ "$workflow_can_approve_prs" = "false" ] && ok "GitHub Actions cannot approve pull requests by default" || note "GitHub Actions can approve pull requests by default"
  case "$actions_retention_days" in
    ''|*[!0-9]*)
      note "could not determine GitHub Actions artifact/log retention"
      ;;
    *)
      if [ "$actions_retention_days" -le "$max_actions_retention_days" ]; then
        ok "GitHub Actions artifact/log retention is ${actions_retention_days} day(s)"
      else
        note "GitHub Actions artifact/log retention is ${actions_retention_days} day(s), expected ${max_actions_retention_days} or less"
      fi
      ;;
  esac

  # A workflow GitHub knows about that the tracked tree does not define is an
  # unreviewed automation surface on a public repository. Compare complete
  # paths, not basenames: a nested/moved remote workflow with the same filename
  # as a reviewed one is still a different automation surface. GitHub's one
  # synthetic Dependabot workflow is the narrow exception; it is generated from
  # the tracked dependabot.yml and cannot itself exist under .github/workflows.
  tracked_workflows="$(
    find .github/workflows -type f \( -name '*.yml' -o -name '*.yaml' \) 2>/dev/null \
      | LC_ALL=C sort -u
  )"
  managed_workflows=""
  if git ls-files --error-unmatch -- .github/dependabot.yml >/dev/null 2>&1; then
    managed_workflows="dynamic/dependabot/dependabot-updates"
  fi
  reviewed_workflows="$(
    printf '%s\n%s\n' "$tracked_workflows" "$managed_workflows" \
      | awk 'NF' | LC_ALL=C sort -u
  )"
  if remote_workflows="$(gh api --paginate "repos/${repo}/actions/workflows" \
    --jq '.workflows[]? | select(.state != "deleted") | .path' 2>/dev/null)"; then
    unexpected_workflows="$(
      printf '%s\n' "$remote_workflows" \
        | LC_ALL=C sort -u \
        | grep -Fxv -f <(printf '%s\n' "$reviewed_workflows") || true
    )"
    if [ -n "$unexpected_workflows" ]; then
      note "GitHub hosts workflow(s) this repository does not track"
      printf '%s\n' "$unexpected_workflows" | sed 's/^/  - /'
    elif [ -n "$managed_workflows" ] && \
        printf '%s\n' "$remote_workflows" | grep -Fxq "$managed_workflows"; then
      ok "every GitHub-hosted workflow is tracked or the exact GitHub-managed Dependabot workflow"
    else
      ok "every workflow GitHub hosts is tracked in .github/workflows"
    fi
  else
    note "could not enumerate GitHub-hosted workflows"
  fi

  echo
  echo "== Hosted GitHub Actions run status =="
  run_id="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 --json databaseId --jq '.[0].databaseId // ""' 2>/dev/null || true)"
  if [ -z "$run_id" ]; then
    ok "no hosted CI run is required for the local-CI public repo policy"
  else
    run_sha="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 --json headSha --jq '.[0].headSha // ""')"
    run_status="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 --json status --jq '.[0].status // ""')"
    run_conclusion="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 --json conclusion --jq '.[0].conclusion // ""')"
    run_url="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 --json url --jq '.[0].url // ""')"

    if [ "$actions_enabled" = "false" ]; then
      ok "hosted CI is disabled; latest recorded run is informational only"
    elif [ "$run_sha" = "$head_sha" ]; then
      warn "latest hosted CI run is for current HEAD, but hosted CI is not a required release gate"
    else
      warn "latest hosted CI run is for $run_sha, not current HEAD $head_sha"
    fi

    if [ "$run_status" = "completed" ] && [ "$run_conclusion" = "success" ]; then
      ok "latest hosted CI run succeeded: $run_url"
    elif [ "$actions_enabled" = "false" ]; then
      ok "latest recorded hosted CI run is not green, but hosted CI is disabled and not a release gate: $run_url"
    else
      warn "latest hosted CI run is not green and is not a required release gate: status=${run_status:-unknown}, conclusion=${run_conclusion:-unknown}, url=$run_url"
      echo
      echo "  Job summary:"
      gh api "repos/${repo}/actions/runs/${run_id}/jobs" \
        --jq '.jobs[] | "  - \(.name): status=\(.status) conclusion=\(.conclusion) runner_id=\(.runner_id // 0) steps=\((.steps // []) | length) id=\(.id)"' \
        2>/dev/null || warn "could not fetch CI job summary"

      job_ids="$(gh api "repos/${repo}/actions/runs/${run_id}/jobs" --jq '.jobs[].id' 2>/dev/null || true)"
      if [ -n "$job_ids" ]; then
        echo
        echo "  Failed-run annotations:"
        while IFS= read -r job_id; do
          [ -n "$job_id" ] || continue
          gh api "repos/${repo}/check-runs/${job_id}/annotations" \
            --jq '.[] | "  - " + .message' 2>/dev/null || true
        done <<< "$job_ids"
      fi
    fi
  fi

  scan_local_public_tree
  scan_github_branch_tag_refs "$repo"
  scan_github_pull_refs "$repo"
  scan_github_workflow_run_history "$repo"
  scan_github_public_text_surface "$repo"
  scan_github_public_commit_refs "$repo"
  scan_github_launch_labels "$repo"
  scan_github_release_assets "$repo"
  scan_github_actions_artifacts "$repo"
  scan_github_deployment_environments "$repo"

  echo
  echo "== Protection and security settings =="
  protection_tmp="$(mktemp "${TMPDIR:-/tmp}/mdkr64-branch-protection.XXXXXX")"
  protection_json="$(mktemp "${TMPDIR:-/tmp}/mdkr64-branch-protection-json.XXXXXX")"
  if gh api "repos/${repo}/branches/main/protection" >"$protection_json" 2>"$protection_tmp"; then
    ok "main branch protection is enabled/readable"
    protection_eval="$(mktemp "${TMPDIR:-/tmp}/mdkr64-branch-protection-eval.XXXXXX")"
    protection_eval_status=0
    python3 tools/check_github_branch_protection.py \
      "$protection_json" \
      --format tabs \
      >"$protection_eval" || protection_eval_status=$?
    while IFS=$'\t' read -r level message; do
      case "$level" in
        ok) ok "$message" ;;
        warn) warn "$message" ;;
        fail) note "$message" ;;
      esac
    done < "$protection_eval"
    if [ "$protection_eval_status" -gt 1 ]; then
      note "could not evaluate branch protection details"
    fi
    rm -f "$protection_eval"
  else
    protection_msg="$(tr '\n' ' ' < "$protection_tmp" | sed 's/[[:space:]]\+/ /g')"
    if [ "$allow_private" -eq 1 ] && [ "$is_private" = "true" ]; then
      warn "main branch protection is not readable for this private repository check: $protection_msg"
    else
      note "main branch protection is not enabled/readable: $protection_msg"
    fi
  fi
  rm -f "$protection_tmp" "$protection_json"

  if gh api "repos/${repo}/vulnerability-alerts" --silent 2>/dev/null; then
    ok "Dependabot vulnerability alerts endpoint is available"
  else
    note "Dependabot vulnerability alerts endpoint is not available"
  fi

  secret_scanning_tmp="$(mktemp "${TMPDIR:-/tmp}/mdkr64-secret-scanning.XXXXXX")"
  if gh api "repos/${repo}/secret-scanning/alerts" --silent 2>"$secret_scanning_tmp"; then
    ok "secret scanning endpoint is available"
  else
    secret_scanning_msg="$(tr '\n' ' ' < "$secret_scanning_tmp" | sed 's/[[:space:]]\+/ /g')"
    if [ "$allow_private" -eq 1 ] && [ "$is_private" = "true" ]; then
      warn "secret scanning endpoint is not available for this private repository check: $secret_scanning_msg"
    else
      warn "secret scanning endpoint is not available; enable secret scanning/push protection if GitHub exposes it: $secret_scanning_msg"
    fi
  fi
  rm -f "$secret_scanning_tmp"

  private_vuln_tmp="$(mktemp "${TMPDIR:-/tmp}/mdkr64-private-vuln.XXXXXX")"
  if gh api "repos/${repo}/private-vulnerability-reporting" --silent 2>"$private_vuln_tmp"; then
    ok "private vulnerability reporting endpoint is available"
  else
    private_vuln_msg="$(tr '\n' ' ' < "$private_vuln_tmp" | sed 's/[[:space:]]\+/ /g')"
    if [ "$allow_private" -eq 1 ] && [ "$is_private" = "true" ]; then
      warn "private vulnerability reporting endpoint is not available for this private repository check: $private_vuln_msg"
    else
      note "private vulnerability reporting endpoint is not available: $private_vuln_msg"
    fi
  fi
  rm -f "$private_vuln_tmp"
fi

echo
if [ "$fail" -ne 0 ]; then
  echo "GitHub public readiness FAILED (${warn_count} warning(s))."
  exit 1
fi

echo "GitHub public readiness passed (${warn_count} warning(s))."
