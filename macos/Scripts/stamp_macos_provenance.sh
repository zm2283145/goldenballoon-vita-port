#!/usr/bin/env bash
# Add an explicit macOS signing mode to the common release provenance sidecar.

set -euo pipefail

die() {
    printf 'stamp_macos_provenance: FAIL — %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: macos/Scripts/stamp_macos_provenance.sh \
  --signing ad-hoc-unsigned|developer-id-notarized \
  --phone-party partyless|cloud-enabled ARTIFACT VERSION
EOF
}

SIGNING_MODE=""
PHONE_PARTY_MODE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --signing)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            SIGNING_MODE="$2"
            shift 2
            ;;
        --phone-party)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            PHONE_PARTY_MODE="$2"
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done
[[ $# -eq 2 ]] || { usage >&2; exit 1; }
ARTIFACT="$1"
VERSION="$2"

case "${SIGNING_MODE}" in
    ad-hoc-unsigned|developer-id-notarized) ;;
    *) die "--signing must be ad-hoc-unsigned or developer-id-notarized" ;;
esac
case "${PHONE_PARTY_MODE}" in
    partyless|cloud-enabled) ;;
    *) die "--phone-party must be partyless or cloud-enabled" ;;
esac
[[ -f "${ARTIFACT}" ]] || die "artifact not found: ${ARTIFACT}"
[[ "${VERSION}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
    die "version must look like 1.0 or 1.0.5"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
"${PROJECT_ROOT}/tools/release/stamp_provenance.sh" \
    --phone-party "${PHONE_PARTY_MODE}" "${ARTIFACT}" "${VERSION}"

SIDECAR="${ARTIFACT}.provenance.json"
python3 - "${SIDECAR}" "${SIGNING_MODE}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
payload = json.loads(path.read_text(encoding="utf-8"))
payload["macos_signing"] = sys.argv[2]
path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

printf 'stamp_macos_provenance: PASS — %s (%s)\n' \
    "${SIDECAR}" "${SIGNING_MODE}"
