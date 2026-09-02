#!/usr/bin/env bash
# Deploy-time assertion that the reviewed edge rate-limit rule is actually
# live on the zone. The Worker-side per-address create throttle is the
# code-enforced availability control; this zone rule is the hand-applied edge
# layer in front of it, and a hand-applied rule is exactly the kind that
# silently is not there. This script asks the Cloudflare Rulesets API for the
# zone's http_ratelimit entry point and requires a rule matching
# free-rate-limit-rule.json: same ref, enabled, same expression, action and
# rate-limit parameters.
#
# Credentials come from the environment only:
#   CLOUDFLARE_API_TOKEN  least-privilege token able to read zone WAF rules
#   CLOUDFLARE_ZONE_ID    the zone serving PARTY_DOMAIN
#
# Exit codes:
#   0  the rule is live and matches the reviewed payload
#   1  the rule is verifiably wrong: the zone answered authoritatively and the
#      reviewed rule is missing, disabled, or diverged. This includes a 404 /
#      "ruleset not found" for the http_ratelimit phase entrypoint (a fresh zone
#      that never had the ruleset created) and a present-but-empty entrypoint.
#   2  credentials absent — an explicit refusal to claim success, so a
#      credential-less CI run can never report the rule as verified
#   3  the check could not run: the API was unreachable, returned non-JSON, or
#      refused the request for auth/scope reasons (e.g. a token without the Zone
#      WAF read scope), or the zone id could not be routed. This is "cannot
#      verify", not "verifiably absent", so a caller can degrade it to a warning
#      instead of blocking a deploy on a credential/scope/config problem.
#   4  local repo-integrity error: the reviewed payload file is not present in
#      this checkout. Not a zone status at all.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POLICY="$HERE/free-rate-limit-rule.json"
[[ -f "$POLICY" ]] || {
    echo "verify-edge-rate-limit: ERROR -- the reviewed payload is missing from" >&2
    echo "  this checkout: $POLICY" >&2
    echo "  This is a local repo-integrity problem, not a zone status. Restore" >&2
    echo "  the file from version control. Exiting 4." >&2
    exit 4
}

if [[ -z "${CLOUDFLARE_API_TOKEN:-}" || -z "${CLOUDFLARE_ZONE_ID:-}" ]]; then
    echo "verify-edge-rate-limit: UNVERIFIED -- CLOUDFLARE_API_TOKEN and" >&2
    echo "  CLOUDFLARE_ZONE_ID are required to assert the zone rule exists." >&2
    echo "  Exiting 2: this is a refusal to claim success, not a pass." >&2
    exit 2
fi

# Capture the HTTP status alongside the body: a 404 on the phase entrypoint of a
# fresh zone is an AUTHORITATIVE "the ruleset was never created" (rule absent),
# which must be a hard failure, not a cannot-verify. Auth/scope refusals (401,
# 403, and Cloudflare's 9xxx/10000 authentication codes) stay cannot-verify.
BODY_AND_CODE="$(curl --silent --show-error --max-time 30 \
    --write-out '\n%{http_code}' \
    --header "Authorization: Bearer $CLOUDFLARE_API_TOKEN" \
    "https://api.cloudflare.com/client/v4/zones/$CLOUDFLARE_ZONE_ID/rulesets/phases/http_ratelimit/entrypoint")" || {
    echo "verify-edge-rate-limit: UNVERIFIED -- the Cloudflare API was" >&2
    echo "  unreachable, so the rule's status could not be determined." >&2
    exit 3
}
HTTP_CODE="${BODY_AND_CODE##*$'\n'}"
RESPONSE="${BODY_AND_CODE%$'\n'*}"

RESPONSE="$RESPONSE" HTTP_CODE="$HTTP_CODE" python3 - "$POLICY" <<'PY'
import json
import os
import sys

policy_path = sys.argv[1]
with open(policy_path, encoding="utf-8") as handle:
    wanted = json.load(handle)["rules"][0]

http_code = os.environ.get("HTTP_CODE", "").strip()

try:
    envelope = json.loads(os.environ["RESPONSE"])
except json.JSONDecodeError:
    print("verify-edge-rate-limit: UNVERIFIED -- the API response was not JSON,"
          f" so the rule's status could not be determined (HTTP {http_code}).",
          file=sys.stderr)
    sys.exit(3)

if not envelope.get("success"):
    errors = [e for e in (envelope.get("errors") or []) if isinstance(e, dict)]
    codes = {e.get("code") for e in errors}
    messages = " ".join((e.get("message") or "").lower() for e in errors)
    detail = json.dumps(envelope.get("errors", []))[:512]

    # Auth/scope refusals: cannot verify. Cloudflare uses 401/403 plus a family
    # of authentication error codes (9106/9109/9103/9107/9108/10000/9000).
    auth_codes = {9106, 9109, 9103, 9107, 9108, 9000, 10000}
    auth_words = ("authentication", "authorization", "not authorized",
                  "permission", "access denied", "insufficient", "unauthorized")
    is_auth = (http_code in ("401", "403") or bool(codes & auth_codes)
               or any(word in messages for word in auth_words))

    # A zone-id/routing problem is a config error, not "rule absent": also
    # cannot verify, so a mistyped CLOUDFLARE_ZONE_ID never reads as "apply the
    # rule".
    is_zone_problem = ("could not route" in messages
                       or ("zone" in messages and ("not found" in messages
                           or "invalid" in messages or "does not exist" in messages)))

    # A 404 / "ruleset not found" for the phase entrypoint is authoritative
    # absence: the http_ratelimit ruleset was never created on this zone.
    ruleset_missing = (http_code == "404"
                       or ("ruleset" in messages and "not found" in messages)
                       or "does not exist" in messages)

    if is_auth or is_zone_problem:
        print("verify-edge-rate-limit: UNVERIFIED -- the API refused the request"
              " for auth/scope/config reasons (a token without Zone WAF read"
              " scope, or a wrong zone id, does this), so the rule's status could"
              f" not be determined (HTTP {http_code}): {detail}", file=sys.stderr)
        sys.exit(3)
    if ruleset_missing:
        print("verify-edge-rate-limit: FAIL -- the zone has no http_ratelimit"
              f" phase entrypoint ruleset (HTTP {http_code}); the reviewed /api/"
              " rate-limit rule is not live. Apply"
              " services/party/ops/free-rate-limit-rule.json per"
              " docs/multiplayer/DEPLOY_PHONE_PARTY.md.", file=sys.stderr)
        sys.exit(1)
    print("verify-edge-rate-limit: UNVERIFIED -- the API returned an"
          f" unrecognized failure (HTTP {http_code}), so the rule's status could"
          f" not be determined: {detail}", file=sys.stderr)
    sys.exit(3)

rules = (envelope.get("result") or {}).get("rules") or []
live = [rule for rule in rules if rule.get("ref") == wanted["ref"]]
if not live:
    print("verify-edge-rate-limit: FAIL -- no rule with ref"
          f" {wanted['ref']!r} exists on the zone's http_ratelimit"
          " entry point. Apply services/party/ops/free-rate-limit-rule.json"
          " per docs/multiplayer/DEPLOY_PHONE_PARTY.md.", file=sys.stderr)
    sys.exit(1)

rule = live[0]
mismatches = []
if rule.get("enabled") is not True:
    mismatches.append("the rule is disabled")
for field in ("expression", "action"):
    if rule.get(field) != wanted[field]:
        mismatches.append(f"{field}: live {rule.get(field)!r}"
                          f" != reviewed {wanted[field]!r}")
live_limit = rule.get("ratelimit") or {}
wanted_limit = wanted["ratelimit"]
for field in ("period", "requests_per_period", "mitigation_timeout"):
    if live_limit.get(field) != wanted_limit[field]:
        mismatches.append(f"ratelimit.{field}: live {live_limit.get(field)!r}"
                          f" != reviewed {wanted_limit[field]!r}")
if sorted(live_limit.get("characteristics") or []) != \
        sorted(wanted_limit["characteristics"]):
    mismatches.append("ratelimit.characteristics diverged from the reviewed"
                      " payload")
if mismatches:
    print("verify-edge-rate-limit: FAIL -- the live rule diverges from the"
          " reviewed payload:", file=sys.stderr)
    for item in mismatches:
        print(f"  - {item}", file=sys.stderr)
    sys.exit(1)

print("verify-edge-rate-limit: PASS -- the reviewed /api/ rate-limit rule"
      f" ({wanted['ref']}) is live and enabled on the zone.")
PY
