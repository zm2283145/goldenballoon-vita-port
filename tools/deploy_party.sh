#!/bin/bash
#
# deploy_party.sh -- the one command that deploys the Golden Balloon Party
# service to production.
#
#   tools/deploy_party.sh --dry-run    # everything except the mutation
#   tools/deploy_party.sh              # deploy
#
# It is deliberately boring and loud. In order it:
#
#   1. records the commit and refuses an unexplained dirty tree;
#   2. requires Node 22+ and the exact lockfile-pinned Wrangler;
#   3. runs `npm ci && npm run check` in services/party;
#   4. resolves the ONE owner-supplied value (PARTY_DOMAIN) and generates
#      services/party/wrangler.production.jsonc from the tracked wrangler.jsonc;
#   5. runs the production config gate in fail-closed mode, plus the edge-policy
#      and internal-API gates, and proves the untouched placeholder config is
#      still rejected;
#   6. runs `wrangler deploy --dry-run` and inspects the bindings census;
#   7. requires the required secrets to already exist in the Wrangler secret
#      store (their values are never read, printed or passed on a command line);
#   8. deploys;
#   9. asserts the hand-applied edge rate-limit rule is live on the zone, then
#      prints a consolidated verification summary and the phone-pairing
#      verification command to run next.
#
# Step 9's rule assertion fails the deploy only when the zone answers and the
# reviewed rule is verifiably absent/disabled/diverged; a missing API token or
# scope degrades to a loud yellow warning (the rule is hand-applied zone state,
# so the deploy cannot mint it anyway). --skip-rate-limit-check bypasses it with
# a red warning.
#
# --dry-run stops before 7/8/9 and needs no Cloudflare account at all, so the
# whole path above is exercisable offline.
#
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
SERVICE="$ROOT/services/party"
GENERATED="$SERVICE/wrangler.production.jsonc"
PLACEHOLDER_HOST="party.example.invalid"
# A fixture host used only by a bare --dry-run so the generation, gate and
# binding inspection are all exercised without an account. It is never
# deployable: steps 7/8/9 are unreachable in dry-run mode.
DRY_RUN_FIXTURE_HOST="party.dry-run-fixture.gb-not-a-real-zone.net"

DRY_RUN=0
ALLOW_DIRTY=0
SKIP_CHECKS=0
SKIP_RATE_LIMIT=0
DOMAIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)              DRY_RUN=1; shift ;;
        --allow-dirty)          ALLOW_DIRTY=1; shift ;;
        --skip-checks)          SKIP_CHECKS=1; shift ;;
        --skip-rate-limit-check) SKIP_RATE_LIMIT=1; shift ;;
        --domain)               DOMAIN_OVERRIDE="$2"; shift 2 ;;
        -h|--help)              sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "deploy_party: unknown argument: $1" >&2; exit 2 ;;
    esac
done

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\ndeploy_party: FAIL -- %s\n' "$*" >&2; exit 1; }
# Terminal colors, only when stdout is a TTY, so piped/CI logs stay plain.
if [[ -t 1 ]]; then
    RED=$'\033[31m'; YELLOW=$'\033[33m'; GREEN=$'\033[32m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
    RED=""; YELLOW=""; GREEN=""; BOLD=""; RESET=""
fi

# --------------------------------------------------------------------------
say "1/9  worktree"

COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"
echo "  commit  $COMMIT ($BRANCH)"
echo "  dirty   $DIRTY tracked/untracked path(s)"
if [[ "$DIRTY" != "0" ]]; then
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "  NOTE: dry run publishes nothing, continuing on a dirty tree."
    elif [[ $ALLOW_DIRTY -eq 1 ]]; then
        echo "  WARNING: deploying a dirty tree because --allow-dirty was given."
        echo "  The deployed build will not correspond to $COMMIT."
    else
        fail "the worktree is dirty; commit or stash first, or pass --allow-dirty
  and record why. A deployed build must correspond to a known commit."
    fi
fi

# --------------------------------------------------------------------------
say "2/9  toolchain"

NODE=""
for candidate in "$(command -v node || true)" \
                 "$HOME"/.nvm/versions/node/v*/bin/node; do
    [[ -x "$candidate" ]] || continue
    major="$("$candidate" -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)"
    if [[ "$major" -ge 22 ]]; then NODE="$candidate"; fi
done
[[ -n "$NODE" ]] || fail "Node 22+ is required (services/party engines.node >=22)."
echo "  node    $NODE ($("$NODE" -p 'process.versions.node'))"

PINNED="$(sed -n 's/.*"wrangler": "\([^"]*\)".*/\1/p' "$SERVICE/package.json" | head -1)"
[[ -n "$PINNED" ]] || fail "services/party/package.json does not pin wrangler."
case "$PINNED" in
    [0-9]*) ;;
    *) fail "wrangler must be pinned to an exact version, found '$PINNED'." ;;
esac
LOCKED="$(sed -n 's/.*"wrangler": "\([^"]*\)".*/\1/p' "$SERVICE/package-lock.json" \
    | grep -x '[0-9][0-9.]*' | head -1 || true)"
if [[ -n "$LOCKED" && "$LOCKED" != "$PINNED" ]]; then
    fail "package.json pins wrangler $PINNED but the lockfile resolves $LOCKED."
fi
echo "  wrangler pinned at $PINNED"

# --------------------------------------------------------------------------
say "3/9  service install and checks"

if [[ $SKIP_CHECKS -eq 1 ]]; then
    echo "  SKIPPED by --skip-checks. This is not a deploy path; use it only to"
    echo "  re-run a later step after a green run."
else
    ( cd "$SERVICE" && npm ci && npm run check )
fi

WRANGLER_JS="$SERVICE/node_modules/wrangler/bin/wrangler.js"
[[ -f "$WRANGLER_JS" ]] || fail "missing $WRANGLER_JS; run (cd services/party && npm ci)."
INSTALLED="$(sed -n 's/.*"version": "\([^"]*\)".*/\1/p' \
    "$SERVICE/node_modules/wrangler/package.json" | head -1)"
[[ "$INSTALLED" == "$PINNED" ]] || \
    fail "installed wrangler $INSTALLED is not the pinned $PINNED."
echo "  installed wrangler $INSTALLED matches the pin"

# --------------------------------------------------------------------------
say "4/9  production config"

PARTY_DOMAIN="${DOMAIN_OVERRIDE:-${PARTY_DOMAIN:-}}"
if [[ -z "$PARTY_DOMAIN" && -f "$SERVICE/ops/production.env" ]]; then
    # shellcheck disable=SC1091
    PARTY_DOMAIN="$(sed -n 's/^[[:space:]]*PARTY_DOMAIN[[:space:]]*=[[:space:]]*//p' \
        "$SERVICE/ops/production.env" | tail -1 | tr -d '"'"'"' \r')"
fi
if [[ -z "$PARTY_DOMAIN" ]]; then
    if [[ $DRY_RUN -eq 1 ]]; then
        PARTY_DOMAIN="$DRY_RUN_FIXTURE_HOST"
        echo "  ****************************************************************"
        echo "  DRY RUN FIXTURE DOMAIN: $PARTY_DOMAIN"
        echo "  services/party/ops/production.env does not exist, so this run is"
        echo "  proving the machinery, not a deployable configuration. Copy"
        echo "  ops/production.env.example and set PARTY_DOMAIN before deploying."
        echo "  ****************************************************************"
    else
        fail "PARTY_DOMAIN is not set.
  cp services/party/ops/production.env.example services/party/ops/production.env
  then set PARTY_DOMAIN to the hostname that will serve the controller."
    fi
fi
case "$PARTY_DOMAIN" in
    *://*|*/*|*:*|*" "*|"") fail "PARTY_DOMAIN must be a bare hostname, got '$PARTY_DOMAIN'." ;;
    *.) fail "PARTY_DOMAIN must not end with a dot, got '$PARTY_DOMAIN'." ;;
    "$PLACEHOLDER_HOST") fail "PARTY_DOMAIN is still the fail-closed placeholder." ;;
esac
echo "  PARTY_DOMAIN  $PARTY_DOMAIN"
echo "  PARTY_ORIGIN  https://$PARTY_DOMAIN"

# The tracked config is the single source of truth; the generated file differs
# from it by exactly the one placeholder host.
sed "s/$PLACEHOLDER_HOST/$PARTY_DOMAIN/g" "$SERVICE/wrangler.jsonc" > "$GENERATED"
if grep -q "$PLACEHOLDER_HOST" "$GENERATED"; then
    fail "the generated config still contains the $PLACEHOLDER_HOST placeholder."
fi
DIFFERENCES="$(diff <(sed "s/$PARTY_DOMAIN/$PLACEHOLDER_HOST/g" "$GENERATED") \
    "$SERVICE/wrangler.jsonc" | wc -l | tr -d ' ')"
[[ "$DIFFERENCES" == "0" ]] || \
    fail "the generated config differs from wrangler.jsonc by more than the host."
echo "  generated  $GENERATED"

# --------------------------------------------------------------------------
say "5/9  gates"

python3 "$ROOT/tests/check_party_production_config.py" \
    --config "$GENERATED" --require-real-domain
python3 "$ROOT/tests/check_party_edge_policy.py"
python3 "$ROOT/tests/check_party_internal_api.py"

# The fail-closed default must still be refused. If this ever passes, the
# placeholder has been edited in place and an unconfigured checkout became
# deployable.
if python3 "$ROOT/tests/check_party_production_config.py" \
        --require-real-domain >/dev/null 2>&1; then
    fail "the tracked wrangler.jsonc no longer fails closed on its placeholder."
fi
echo "  fail-closed placeholder still rejected in the tracked config"

# --------------------------------------------------------------------------
say "6/9  dry run and binding census"

DRY_OUT="$SERVICE/.deploy-dry-run"
rm -rf "$DRY_OUT"
# wrangler >=4.12x colorizes the binding census even under CI, which broke the
# fixed-string binding checks below. Ask it not to (NO_COLOR) and strip any SGR
# escapes that survive, so the census parses regardless of the wrangler version.
BINDINGS="$( cd "$SERVICE" && CI=1 NO_COLOR=1 "$NODE" "$WRANGLER_JS" deploy --dry-run \
    --outdir "$DRY_OUT" --config "$GENERATED" --env production 2>&1 \
    | perl -pe 's/\e\[[0-9;]*m//g' )"
printf '%s\n' "$BINDINGS" | sed 's/^/  | /'

for binding in "env.PARTY_ROOMS (PartyRoom)" "env.PARTY_BUDGETS (PartyBudget)" \
               "env.PARTY_CODES (PartyCodeDirectory)" \
               "env.MATCH_ROOMS (MatchRoom)" "env.PARTY_ORIGIN" \
               "env.MAX_ADMISSIONS_PER_DAY" "env.CONTROL_RESERVE_PER_DAY"; do
    printf '%s\n' "$BINDINGS" | grep -qF "$binding" || \
        fail "the dry run is missing the required binding: $binding"
done
COUNT="$(printf '%s\n' "$BINDINGS" | grep -cE '^env\.' || true)"
[[ "$COUNT" == "7" ]] || \
    fail "the dry run exposes $COUNT bindings; exactly 7 are reviewed."
# Secrets must live in the secret store, never as plain-text vars in the bundle.
for secret in PARTY_HMAC_KEY OPS_READ_TOKEN; do
    printf '%s\n' "$BINDINGS" | grep -qE "^env\.$secret" && \
        fail "$secret is configured as a plain-text var; it must be a secret."
done
printf '%s\n' "$BINDINGS" | grep -qF "assets directory" || \
    fail "the dry run did not read the dist/web assets directory."
echo "  7/7 reviewed bindings present, no secret is a plain-text var"

if [[ $DRY_RUN -eq 1 ]]; then
    say "dry run complete"
    echo "  Nothing was deployed. Steps 7 (secrets), 8 (deploy) and 9 (edge"
    echo "  rate-limit assertion) need an authenticated Cloudflare account and"
    echo "  were not attempted."
    echo
    echo "deploy_party: PASS (dry run) -- commit $COMMIT, origin https://$PARTY_DOMAIN"
    exit 0
fi

# --------------------------------------------------------------------------
say "7/9  secrets"

echo "  Reading the secret NAMES only. Values are never read or printed."
SECRETS="$( cd "$SERVICE" && CI=1 "$NODE" "$WRANGLER_JS" secret list \
    --config "$GENERATED" --env production 2>&1 )" || \
    fail "could not list secrets. Run 'wrangler login' first."
# Zero-cost ruling: this deployment is STUN-only, so TURN_KEY_ID/TURN_API_TOKEN
# are intentionally absent and deliberately NOT asserted here (a set TURN pair
# would opt into a paid Realtime path); STUN-only is the supported posture.
for secret in PARTY_HMAC_KEY OPS_READ_TOKEN; do
    printf '%s\n' "$SECRETS" | grep -qF "$secret" || fail "$secret is not set.
  Provision it interactively -- the value is typed into the prompt, never a
  shell argument, so it does not enter your shell history:

    (cd services/party && npx wrangler secret put $secret \\
        --config wrangler.production.jsonc --env production)

  PARTY_HMAC_KEY must be at least 32 random bytes. OPS_READ_TOKEN must be an
  independent random secret of at least 32 characters -- never the same value."
    echo "  $secret is provisioned"
done
SECRETS_STATUS="PARTY_HMAC_KEY + OPS_READ_TOKEN present (TURN pair intentionally absent: STUN-only)"

# --------------------------------------------------------------------------
say "8/9  deploy"

( cd "$SERVICE" && CI=1 "$NODE" "$WRANGLER_JS" deploy \
    --config "$GENERATED" --env production )
echo "  deployed commit $COMMIT to https://$PARTY_DOMAIN"

# --------------------------------------------------------------------------
say "9/9  edge rate-limit assertion"

# The per-IP /api/ throttle (30 req / 10 s) is a HAND-APPLIED zone rule, not
# Worker code -- so if it was never applied, or was later removed, nothing else
# here would notice and the main brute-force throttle would be silently absent.
# Assert it is live on every deploy. A verifiably-absent rule is a hard failure
# (the deploy already shipped, but the operator must apply the rule); a missing
# token/scope or an unreachable API degrades to a loud warning, because the
# deploy cannot mint zone state and must not be blocked on a credential gap.
RATE_LIMIT_SCRIPT="$SERVICE/ops/verify-edge-rate-limit.sh"
RATE_LIMIT_RULE="services/party/ops/free-rate-limit-rule.json"
RATE_LIMIT_FAILED=0
if [[ $SKIP_RATE_LIMIT -eq 1 ]]; then
    RATE_LIMIT_STATUS="SKIPPED by --skip-rate-limit-check"
    printf '%s' "$RED"
    echo "  !! SKIPPED by --skip-rate-limit-check. The edge /api/ rate-limit rule"
    echo "  !! was NOT verified. If it is not live, the main brute-force throttle"
    echo "  !! is silently absent. Re-run without this flag and confirm PASS."
    printf '%s' "$RESET"
elif [[ ! -x "$RATE_LIMIT_SCRIPT" ]]; then
    RATE_LIMIT_STATUS="UNVERIFIED (checker $RATE_LIMIT_SCRIPT missing/not executable)"
    printf '%s%s%s\n' "$YELLOW" \
        "  WARNING: $RATE_LIMIT_SCRIPT is missing or not executable; the edge rule could not be verified." "$RESET"
else
    RATE_LIMIT_RC=0
    "$RATE_LIMIT_SCRIPT" || RATE_LIMIT_RC=$?
    case "$RATE_LIMIT_RC" in
        0)
            RATE_LIMIT_STATUS="verified live on the zone"
            printf '%s%s%s\n' "$GREEN" "  the reviewed /api/ rate-limit rule is live and enabled." "$RESET"
            ;;
        1)
            RATE_LIMIT_STATUS="VERIFIABLY ABSENT/DIVERGED -- apply $RATE_LIMIT_RULE"
            RATE_LIMIT_FAILED=1
            printf '%s' "$RED$BOLD"
            echo "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            echo "  !! EDGE RATE-LIMIT RULE IS NOT LIVE ON THE ZONE."
            echo "  !! The per-IP /api/ brute-force throttle is silently absent."
            echo "  !! FIX: apply the reviewed payload to the zone's http_ratelimit"
            echo "  !!      entry point:  $RATE_LIMIT_RULE"
            echo "  !!      (see docs/multiplayer/DEPLOY_PHONE_PARTY.md)"
            echo "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            printf '%s' "$RESET"
            ;;
        2)
            RATE_LIMIT_STATUS="UNVERIFIED (CLOUDFLARE_API_TOKEN/CLOUDFLARE_ZONE_ID not set)"
            printf '%s' "$YELLOW"
            echo "  WARNING: the edge rate-limit rule could NOT be verified because"
            echo "  CLOUDFLARE_API_TOKEN and CLOUDFLARE_ZONE_ID are not set. This is"
            echo "  not a pass. Verify manually with a least-privilege Zone WAF token:"
            echo "    CLOUDFLARE_API_TOKEN=... CLOUDFLARE_ZONE_ID=... $RATE_LIMIT_SCRIPT"
            printf '%s' "$RESET"
            ;;
        3)
            RATE_LIMIT_STATUS="UNVERIFIED (API unreachable, or token lacks Zone WAF read scope, or zone id not routable)"
            printf '%s' "$YELLOW"
            echo "  WARNING: the edge rate-limit rule could NOT be verified (the API"
            echo "  was unreachable, the token lacks Zone WAF read scope, or the zone"
            echo "  id is not routable). This is not a pass; the rule's status is"
            echo "  unknown. Re-verify with a least-privilege Zone WAF token and the"
            echo "  correct CLOUDFLARE_ZONE_ID once connectivity/scope is fixed."
            printf '%s' "$RESET"
            ;;
        4)
            RATE_LIMIT_STATUS="UNVERIFIED (reviewed payload $RATE_LIMIT_RULE missing from checkout)"
            printf '%s' "$YELLOW"
            echo "  WARNING: the edge rate-limit rule could NOT be verified because"
            echo "  the reviewed payload $RATE_LIMIT_RULE is missing from this"
            echo "  checkout. This is a local repo-integrity problem, NOT a zone"
            echo "  status -- restore the file from version control and re-verify."
            printf '%s' "$RESET"
            ;;
        *)
            RATE_LIMIT_STATUS="UNVERIFIED (checker returned an unexpected code rc=$RATE_LIMIT_RC)"
            printf '%s' "$YELLOW"
            echo "  WARNING: the edge rate-limit checker returned an unexpected exit"
            echo "  code ($RATE_LIMIT_RC); the rule's status is unknown. This is not"
            echo "  a pass. Investigate $RATE_LIMIT_SCRIPT."
            printf '%s' "$RESET"
            ;;
    esac
fi

# --------------------------------------------------------------------------
# Consolidated post-deploy verification summary. TLS and WSS pairing are proven
# by tools/verify_party_deploy.py (the next command); the rate-limit rule and
# secrets presence were asserted above / in step 7.
printf '\n%s== verification summary%s\n' "$BOLD" "$RESET"
echo "  TLS ................ pending -- proven by verify_party_deploy.py (next command)"
echo "  WSS pairing ........ pending -- proven by verify_party_deploy.py (next command)"
echo "  edge rate-limit .... $RATE_LIMIT_STATUS"
echo "  secrets presence ... $SECRETS_STATUS"

if [[ $RATE_LIMIT_FAILED -eq 1 ]]; then
    printf '\n%sdeploy_party: DEPLOYED but FAILED the edge rate-limit assertion --%s\n' "$RED" "$RESET"
    echo "commit $COMMIT is live at https://$PARTY_DOMAIN, but the edge /api/"
    echo "rate-limit rule is not in force. Apply $RATE_LIMIT_RULE to the zone's"
    echo "http_ratelimit entry point (see docs/multiplayer/DEPLOY_PHONE_PARTY.md),"
    echo "then re-assert it directly:"
    echo "    CLOUDFLARE_API_TOKEN=... CLOUDFLARE_ZONE_ID=... $RATE_LIMIT_SCRIPT"
    echo "and run the phone-pairing verification:"
    echo "    python3 tools/verify_party_deploy.py --origin https://$PARTY_DOMAIN"
    exit 1
fi

cat <<EOF

deploy_party: PASS -- commit $COMMIT deployed to https://$PARTY_DOMAIN

Run the verification now. It pairs a synthetic phone over WSS and exits
nonzero on any failure. The --*-status flags carry this run's rate-limit and
secrets findings into its consolidated summary:

    python3 tools/verify_party_deploy.py --origin https://$PARTY_DOMAIN \\
        --rate-limit-status '$RATE_LIMIT_STATUS' \\
        --secrets-status '$SECRETS_STATUS'

EOF
