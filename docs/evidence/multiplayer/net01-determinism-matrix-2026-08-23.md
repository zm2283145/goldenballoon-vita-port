# NET-01 online determinism matrix — automatable slice (O2.2-sim)

Dated record for the automatable portion of the NET-01 determinism matrix: the
O-T6 two-endpoint online mesh race replayed under every named `net_impairment`
carrier profile, in CI, with no owner hardware. The real two-different-networks
run stays OA-7 (owner hardware); this ledger records exactly what the in-process
matrix now proves and where that boundary sits.

## Provenance

- Commit: `5445381` on branch `campaign-w2` (the matrix test, the additive O-T6
  race seam, `net_impairment` wiring and this note).
- Build: CMake preset `rel` → `build-rel/`, Apple clang 21.0.0, `-Wall -Wextra
  -Wpedantic -Werror`, full build clean.
- Protocol / gameplay ids: `MDKR_ONLINE_PROTOCOL_VERSION = 1`; rollback window
  `MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS = 30` (INPUT_GAP latches at a ≥31-tick
  outrun); cadence 30 Hz; input delay 2.
- ROM: **none** — this lane is ROM-, GPU- and external-network-free. Real
  libdatachannel DTLS runs over `127.0.0.1`; signaling is the O-T2 in-process
  loopback hub.
- OS/hardware: macOS 26.4, arm64.
- Network profile: the six named `net_impairment` profiles (LAN, regional-good,
  regional-variable, poor, two-second-outage, adversarial), seeded per profile
  and per endpoint (never wall-clock).

## Commands

```
cmake --build build-rel                      # full build clean
ctest --test-dir build-rel -R 'online|match' # 26/26 pass, incl. #199/#200
ctest --test-dir build-rel -R online_live_matrix
# direct, with per-profile narration:
build-rel/mdkr_online_live_adapter_test --matrix
```

`online_live_matrix` (ctest #200) is registered in the same ROM-free family as
`online_live_adapter` (#199).

## What the matrix does

Both endpoints are real live adapters composing the O-T3 lobby reducer, the O-T2
peer mesh (real libdatachannel DTLS on loopback), the O-T5 retail clamp and the
O-T6 per-tick race feed. After preflight consensus and descriptor install, the
race feeds real sealed 3-frame input bundles across the mesh for 240 authored
ticks; each endpoint folds every **confirmed** canonical frame into an FNV state
hash. The two independent endpoints must fold a byte-identical hash — or, when a
profile exceeds the 30-tick window, latch the typed recovery path instead of a
silent desync.

Impairment is injected **test-only** at the input-bundle layer, gated behind the
O-T6 driver/race seam — production `match_peer_transport` / `match_transport`
behaviour is untouched:

- The engine keeps advancing in real time via the additive test-only seam
  `mdkr_online_live_adapter_race_drain_local()` (the drain half of
  `race_advance` with the send suppressed). This is the same kind of additive
  seam O-T6 added for its reliability layer (`race_resend`), reachable only
  through the token-gated live adapter, which the production launcher never
  constructs (it is compiled only into the two online test executables).
- Every mesh transmission is offered to a seeded `MdkrNetImpairment` carrier
  first. **Only** datagrams the carrier says survived trigger the genuine
  `mdkr_online_live_adapter_race_resend()` — real seal → real DTLS → real fold.
  Impairment decides *whether/when* a bundle crosses; it never manufactures the
  frame. The confirmed frames folded into the hash are always genuine remote
  input that traversed the mesh (asserted non-vacuous: `inputEnvelopes*>0`,
  `transportAccepted*>0` for every profile).
- The carrier's malformed arm flips the datagram's last byte; the driver carries
  a trailing XOR checksum over the tick token and discards corrupted datagrams
  (`impCorruptDropped`), modelling a receiver dropping a corrupt sealed envelope.

## Results (deterministic; byte-identical across 3 reruns)

Verbatim per-profile narration (`[MATRIX]` lines, `build-rel`):

```
[MATRIX] profile=lan               outcome=converged ticks=240 hashA=e56bb71e8dadd9e1 hashB=e56bb71e8dadd9e1 recovery=0 firstTick=0  observedTick=0  slot=0 sent=480 dropped=0  dup=0  reorder=0  corrupt=0 outage=0  throttled=0  envsA=238 envsB=238 acceptedA=238 acceptedB=238
[MATRIX] profile=regional-good     outcome=converged ticks=240 hashA=e56bb71e8dadd9e1 hashB=e56bb71e8dadd9e1 recovery=0 firstTick=0  observedTick=0  slot=0 sent=480 dropped=0  dup=2  reorder=6  corrupt=0 outage=0  throttled=0  envsA=241 envsB=239 acceptedA=111 acceptedB=120
[MATRIX] profile=regional-variable outcome=converged ticks=240 hashA=e56bb71e8dadd9e1 hashB=e56bb71e8dadd9e1 recovery=0 firstTick=0  observedTick=0  slot=0 sent=480 dropped=1  dup=7  reorder=37 corrupt=0 outage=0  throttled=0  envsA=241 envsB=242 acceptedA=117 acceptedB=125
[MATRIX] profile=poor              outcome=converged ticks=240 hashA=e56bb71e8dadd9e1 hashB=e56bb71e8dadd9e1 recovery=0 firstTick=0  observedTick=0  slot=0 sent=480 dropped=7  dup=12 reorder=53 corrupt=0 outage=0  throttled=7  envsA=238 envsB=242 acceptedA=111 acceptedB=114
[MATRIX] profile=two-second-outage outcome=recovered ticks=31  hashA=f60cbe3fd1bf5502 hashB=f60cbe3fd1bf5502 recovery=1 firstTick=32 observedTick=63 slot=1 sent=126 dropped=68 dup=0  reorder=0  corrupt=0 outage=68 throttled=0  envsA=29  envsB=29  acceptedA=15  acceptedB=15
[MATRIX] profile=adversarial       outcome=recovered ticks=0   hashA=66475819b4882620 hashB=14650fb0739d0383 recovery=1 firstTick=1  observedTick=32 slot=0 sent=64  dropped=4  dup=3  reorder=15 corrupt=5 outage=0  throttled=43 envsA=24  envsB=24  acceptedA=12  acceptedB=14
```

| Profile | Outcome | Evidence |
|---|---|---|
| lan | **converged** | identical hash `e56bb71e8dadd9e1`, 240 ticks |
| regional-good | **converged** | identical hash, dup/reorder exercised |
| regional-variable | **converged** | identical hash, loss+dup+reorder exercised |
| poor | **converged** | identical hash, loss+dup+reorder+throttle exercised |
| two-second-outage | **typed recovery** | INPUT_GAP, first=32 observed=63 (31-tick outrun), 68 outage drops; identical pre-fault hash `f60cbe3fd1bf5502` |
| adversarial | **typed recovery** | INPUT_GAP, first=1 observed=32; 43 throttled + 5 corrupt-dropped starve confirmation from the start |

The four convergent profiles fold the identical hash `e56bb71e8dadd9e1` — the
same content regardless of carrier, because impairment only reorders/delays the
timing of genuine frames. Both recovery profiles latch INPUT_GAP on an
identical pre-fault prefix, then unwind typed rather than desyncing silently.
(The two-second-outage row folds identical hashes up to the fault; the
adversarial row's endpoints break at different fold counts, so its final
hashA/hashB differ — the divergence is where each stopped, not a content
disagreement, and the test never asserts adversarial hash equality.)

### Key assertions (verbatim, `tests/test_online_live_adapter.cpp`)

```c
/* Remote input really crossed the mesh -- impairment did not shortcut the
 * convergence proof. */
CHECK(r.inputEnvelopesA > 0u && r.inputEnvelopesB > 0u);
CHECK(r.transportAcceptedA > 0u && r.transportAcceptedB > 0u);
...
/* The one thing that must NEVER pass: both endpoints ran to the target
 * yet folded different state. */
CHECK(!silentDesync);
...
if (converged) {
    CHECK(r.hashA == r.hashB);
    CHECK(r.racedTicks >= 240u);
}
...
if (spec.profile == MDKR_NET_PROFILE_TWO_SECOND_OUTAGE) {
    CHECK(r.impOutageDropped > 0u);
    CHECK(r.recoveryReason == 1u); /* INPUT_GAP */
    CHECK(r.recoveryObservedTick - r.recoveryFirstTick >= 31u);
}
```

Full run: `online_live_matrix: 95 checks, 0 failures`, stable across 3 reruns
with byte-identical hashes and recovery ticks. The existing `online_live_adapter`
ctest (240-tick unimpaired 2-endpoint convergence) and the wider `online|match`
family stay green (26/26).

## TURN-forced arm — SKIPPED (OA-7-only), attempt recorded

Attempted, honestly negative:

- Local coturn: **not installed** (`turnserver`/`coturn` absent) — no trivially
  available local relay.
- Metered free relay: `global.relay.metered.ca:443` answered a TCP handshake,
  but a TURN **allocation** requires per-app Metered API credentials, which are
  not provisioned here; `openrelay.metered.ca` did not resolve and
  `stun.l.google.com:19302` timed out. No usable relay allocation is reachable.
- Structural reason it is OA-7-only anyway: the automatable in-process lane uses
  the O-T2 loopback **signal** hub and real DTLS over `127.0.0.1` host
  candidates — there is no ICE agent / `iceServers` path to force relay through.
  Relay-forcing only has meaning on the real two-process WebRTC e2e lane, and
  applying `iceTransportPolicy: relay` lives in the RFC/ICE internals of
  `match_peer_transport` (out of this task's fence). Genuinely forcing
  relay-only across two different networks is the OA-7 owner-hardware run with
  service-delivered TURN credentials.

Verdict: TURN-forced relay is **OA-7-only**; the wave is not blocked on it.

## Automatable-vs-OA-7 boundary (for the A3 GO packet)

**Proven automatable now (this ledger):**

- Online rollback **converges to a byte-identical confirmed state hash** across
  the LAN, regional-good, regional-variable and poor carrier profiles, with real
  sealed input crossing a real libdatachannel DTLS mesh, in CI, no hardware.
- Profiles that exceed the 30-tick window (two-second-outage; adversarial in the
  single-link 2-endpoint case) **fire the typed INPUT_GAP recovery path** at the
  retained-history boundary rather than desyncing silently.
- Fully deterministic and reproducible (seeded per profile/endpoint; no
  wall-clock dependence), stable across reruns.

**Remains OA-7 (owner hardware / real service):**

- Real two-**different**-networks race (not loopback): real RTT, real NAT
  traversal, real ISP jitter/loss between two physical machines.
- TURN-**forced** relay path (`iceTransportPolicy: relay`) with
  service-delivered TURN credentials over a real relay.
- 3- and 4-endpoint **mesh fan-out** under impairment: the in-process harness is
  2-endpoint (its install path holds a single process-global engine roster).
  Adversarial fan-out to 3 peers under the 1-delivery/tick throttle — where the
  4-process engine-side rollback lab already shows typed recovery — is not
  reproduced here.

## Known limitations

- 2-endpoint only in-process (per above). Adversarial recovers here via
  single-link throttle starvation, which is a valid typed-recovery witness but
  is not the mesh-fan-out failure mode.
- Impairment is modelled at the input-bundle layer, not inside DTLS: a surviving
  datagram still crosses real DTLS, but DTLS-level corruption/retransmit is not
  separately impaired (the state channel is already lossy by design).

## Reviewer / status

Automated author: O2.2-sim. **CONDITIONAL** on OA-7: the automatable determinism
evidence is complete and green; final NET-01 sign-off waits on the owner-hardware
two-network + TURN-forced run this ledger de-risks. No production netcode
behaviour was changed.
