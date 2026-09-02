# Match preflight consensus v1 and v2

Status: pure local foundation; live binding and online-race admission remain
gated by the written A3 decision.

`platform/net/match_preflight.*` is the launcher-owned seam between a room in
Loading and a future engine launch. It owns no socket, clock, service request,
UI, ROM bytes or engine effect. `READY` means that a bounded set of
authenticated observations agree; it is deliberately not permission to start
the game.

## Frozen expectations

The launcher creates a new coordinator for one exact match epoch and connection
graph. It copies:

- the graph's two to four opaque endpoint ids and connection generations;
- SHA-256 of the canonical 148-byte `MdkrMatchLaunchDescriptorV1`;
- the canonical peer-key transcript digest used by the verification phrase;
- SHA-256 of the graph in ascending endpoint-id order, with every directed
  reachability bit remapped into that canonical order;
- the local endpoint id and exact connection generation.

The descriptor already binds build identity, gameplay digest, supported ROM
revision/cadence, track, rules, vehicle mask, input delay, RNG seed, canonical
slot owners and character/vehicle selection revisions. Each launcher must also
verify its local ROM against the known supported SHA-256 before it attests;
neither that ROM digest nor any ROM byte is sent to the service.

A changed epoch or connection generation constructs a new coordinator. Old
readiness can never cross that boundary.

## Authenticated reports

Each endpoint publishes a bounded attestation only over a carrier that has
already authenticated that endpoint id. Submission receives that authenticated
endpoint id and generation separately and requires the embedded claim to match;
report bytes are never their own identity evidence. The report repeats the exact epoch,
connection generation, descriptor digest and peer-key transcript digest, then
the canonical graph digest, then states three local checks:

1. the supported ROM was verified locally;
2. the room's three-compound-word phrase was confirmed by the people playing;
3. gameplay channels for the frozen graph are ready.

Native and browser share the fixed 136-byte `MPF2` encoding below; `MPF1` was
the same report without the trailing route-quality record, and the two refuse
each other (see "Route quality (v2)"). It is carried
only over a pairwise channel that already authenticated the endpoint id; it is
not a service command and has no unauthenticated fallback. All integers are
big-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `MPF1` |
| 4 | 1 | version `1` |
| 5 | 1 | ROM verified / phrase confirmed / channels ready bits |
| 6 | 2 | zero reserved |
| 8 | 4 | match epoch |
| 12 | 4 | connection generation |
| 16 | 4 | nonzero report sequence |
| 20 | 8 | authenticated endpoint id |
| 28 | 32 | launch-descriptor SHA-256 |
| 60 | 32 | peer-key transcript SHA-256 |
| 92 | 32 | canonical directed graph SHA-256 |
| 124 | 12 | route-quality record (v2; see below) |

Byte 0-3 is `MPF2` and byte 4 is version `2`. Bit `0x08` of byte 5 is the
route-measured flag; the three readiness bits are unchanged and `READY` still
requires exactly those three.

Decode requires exactly 136 bytes and rejects malformed control bytes, reserved
bits and zero identity/generation/sequence atomically. Digest mutations remain
well-formed reports so consensus can classify them as race-settings or secure-
connection/topology disagreement. The service cannot write reports or declare
consensus. Equivalent endpoint arrays hash identically, but any endpoint,
generation or directed-reachability change produces a different graph digest;
this prevents a signaling layer from equivocating about direct versus one-hop
routes while every peer still claims its own channels are ready.

Every endpoint report has a nonzero monotonic sequence. A higher sequence may
advance or withdraw a transient check. An exact retry is idempotent; reuse of a
sequence for changed content conflicts; older sequences, wrong epochs, stale
generations, authenticated-source mismatches, unknown endpoints and reserved
bits reject without mutation.

### Reliable carrier fragmentation

The report does not fit one 64-byte peer payload. The launcher therefore sends
it as exactly three payload-type-`1` envelopes on the reliable ordered control
path; gameplay continues to use payload type `0`. Each plaintext fragment is:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | nonzero report sequence |
| 4 | 1 | fragment index `0..2` |
| 5 | 1 | fragment count `3` |
| 6 | 58 | report bytes; the final fragment uses 8 and zero-pads 50 |

The AEAD envelope supplies authenticated source/destination identity,
connection generations, route, epoch and its own globally unique transport
sequence. Reassembly accepts fragments in any order, treats an exact duplicate
as idempotent, rejects a changed same-sequence/index fragment, rejects stale
reports, and atomically lets a newer report replace an incomplete older one.
Every reassembler is initialized for one exact authenticated source-to-recipient
key direction, and every fragment submission carries the matching decrypted
envelope context and payload type. The completed report's epoch, endpoint id
and connection generation must match that direction before output is published;
cross-peer fragment splicing and forged attribution therefore fail atomically.
The embedded report sequence must also match every fragment header. A one-hop
forwarder still sees only three opaque fixed envelopes; the signaling service
has no preflight route.

## Route quality (v2)

Before admission completes, each launcher measures the route it is about to
race over. The measurement owns no socket and reads no clock: the launcher
hands it the host milliseconds it already samples for its own ladders, so
nothing on the simulation side of the authority boundary is involved. It runs
only while the race transport does not exist, and it touches no simulation
state.

### Replay

Both real lanes are replayed at their real cadence and payload size for 6000
ms, then drained for 1000 ms:

| Lane | Channel | Cadence | Payload |
|---|---|---|---|
| bundle | unreliable state (`gb-match-state-v1`), sealed payload type `0` | one authored tick (33 ms at 30 Hz) | 64 B — one input bundle |
| control | reliable ordered control (`gb-match-control-v1`), sealed payload type `1` | 200 ms | 64 B — one sealed fragment |

Neither lane needs a new sealed payload type: the envelope's type space is
unchanged at `0`/`1`. The bundle lane is where real datagram loss is visible;
on the reliable control lane SCTP retransmission turns loss into latency
instead, which is exactly what that lane does in a race.

Each probe is one fixed 64-byte payload:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `MRQ1` |
| 4 | 1 | lane (`0` bundle, `1` control) |
| 5 | 1 | kind (`0` probe, `1` echo) |
| 6 | 4 | nonzero probe sequence |
| 10 | 8 | originating endpoint id |
| 18 | 46 | zero filler to the lane's real payload size |

The recipient returns the identical probe with kind `1`. The bundle lane's echo
is broadcast like race input, so only the endpoint named in the origin field
times it. Non-zero filler, an unknown lane and a zero sequence or origin all
refuse to decode, and a probe can only be decoded before the race transport
exists.

### Metrics and the score ladder

`p95` is the 95th percentile of the answered round trips; `jitter` is the mean
absolute difference between consecutive answered round trips in send order;
`loss` counts probes never echoed, over probes sent; `late` counts answered
probes slower than 100 ms — the depth past which a 30 Hz authored tick can no
longer absorb the arrival — over probes answered; `undrained` counts probes the
replay wanted to emit after its sample table filled, i.e. the outbound side did
not drain.

The score starts at 10 and takes, per metric, the deduction of the first rung
the value does not exceed, or the row's worst:

| Metric | Rungs (threshold → deduction) | Past every rung |
|---|---|---|
| p95 RTT (ms) | 40 → 0, 70 → 1, 110 → 2, 160 → 3, 220 → 4 | 5 |
| jitter (ms) | 5 → 0, 12 → 1, 25 → 2, 45 → 3 | 4 |
| loss (‰) | 5 → 0, 20 → 2, 50 → 4 | 6 |
| late (‰) | 10 → 0, 50 → 1, 150 → 2 | 3 |
| undrained | zero → 0 | 3 |

The result is clamped to 1-10 and named:

| Score | Band | Room chip |
|---:|---|---|
| 8-10 | `steady` | “~45 ms · steady” |
| 5-7 | `uneven` | “~120 ms · uneven” |
| 1-4 | `rough` | “~260 ms · rough” |

The launcher shows exactly one chip: the round trip a player can feel, then the
band's name. The outcome also joins the mesh bring-up boundary in the online
forensics ring as the code `route-<band>-<score>`, so a dump reads the route a
session started on beside every later stall.

### Record

| Offset | Bytes | Field |
|---:|---:|---|
| 124 | 2 | p95 RTT, ms |
| 126 | 2 | jitter, ms |
| 128 | 2 | loss, per thousand |
| 130 | 2 | late samples, per thousand |
| 132 | 2 | undrained probes |
| 134 | 1 | score, 1-10 |
| 135 | 1 | band (`1` rough, `2` uneven, `3` steady) |

A report either sets the route-measured flag and carries a record whose score
and band are exactly what the ladder above produces from its five metric
fields, or carries an all-zero record with the flag clear. A band that
disagrees with its own score, a rate above 100%, and a record without its flag
each refuse the whole report atomically, so a peer cannot show one number and
band it as another.

The measurement is reported, not required. `READY` still turns on the three
readiness checks alone: a route that measures badly informs the players and
widens the measuring endpoint's entry timing, it never refuses the launch. The
measured report is published as a second attestation for the round, at the
round's higher sequence, after the compatibility report; a round's two
sequences are `2·epoch` and `2·epoch + 1`, so both stay strictly below the next
round's pair.

### Entry timing

The manifest's `input_delay` is the agreed floor: it is compared at admission,
it is inside the launch descriptor whose SHA-256 every report binds, and it is
never lowered. Each endpoint may lead by further whole authored ticks resolved
from its **own** measured p95 RTT — `ceil(p95 / tick_ms)`, minus the floor
already covered, clamped to `[0, 4 − floor]` — up to a hard cap of 4 authored
ticks. Leading further is local: bundles carry their own tick numbers, so two
endpoints leading by different amounts still commit the identical canonical
timeline, which the loopback lane proves by folding the same state hash on both
endpoints with the widen armed.

### Version refusal

`MPF1` and `MPF2` refuse each other by name, in both directions, and neither
downgrades silently:

- the v2 parser reads the format tag before the length, so a 124-byte `MPF1`
  report is refused as `legacy_mpf1` rather than as a short buffer;
- a tag that is neither is `unknown_format`; a well-tagged report of the wrong
  length is `length`;
- the frozen `MPF1` header rule (exactly 124 bytes, tag `MPF1`, version `1`)
  refuses a 136-byte `MPF2` report on all three counts.

## Deterministic status and UX

Evaluation always returns one typed state, the lowest opaque endpoint id that
currently owns the issue, and received/required progress. The launcher maps the
state to a stable recovery action without exposing ids to players:

| State | Player-facing meaning | Primary recovery |
|---|---|---|
| Descriptor mismatch | “Race settings changed” | Return everyone to the same retained room settings and freeze a new epoch. |
| Transcript mismatch | “Secure connection changed” | Tear down the affected peer generation, reconnect, and compare the new phrase. |
| Graph mismatch | “Connection routes changed” | Retire Ready, exchange one fresh authenticated graph, and retry secure connections. |
| Route unavailable | “Couldn’t connect everyone” | Retry secure connections; offer Cancel immediately. Never imply that a free relay exists. |
| Waiting for peers | “Waiting for players · 2 of 3” | Keep room and selections; allow Cancel/Leave. |
| ROM unverified | “Verify your supported game copy” | Open the local ROM picker/check; upload nothing. |
| Verify phrase | “Compare these words” | Show the same large, copyable phrase on every game screen; require explicit confirmation. |
| Channels not ready | “Finishing secure connection” | Retry only the carrier, preserving room state. |
| Ready | “Everyone is ready” | Continue only when the separate product admission gate is enabled after A3 `GO`. |

Descriptor, transcript and graph disagreement outrank transient waiting states so the UI
does not spin when retry cannot help. Route failure is evaluated before missing
reports because an inadmissible graph cannot become ready through more waiting.
For every failure, the launcher retains local and Phone Party play as an
immediate escape hatch.

## Security and recovery invariants

- No display name, IP address, credential, SDP, input, ROM hash or ROM byte is
  retained by this component.
- The endpoint id shown in diagnostics is equality/routing data, not UI copy or
  telemetry.
- Readiness withdrawal is accepted only at a higher authenticated sequence and
  blocks launch immediately.
- Fragment state is per authenticated peer direction; neither a fragment nor a
  completed report may claim identity from its plaintext bytes.
- Invalid internal state fails closed as `INVALID`.
- All initialization and rejection paths leave caller-owned output unchanged.
- Consensus cannot bypass the publisher admission policy, A3 decision, engine
  manifest validation, rollback transport, or loaded-ROM validation.

Strict native/browser tests share one exact `MPF2` vector and cover three-
fragment reorder/duplicate/conflict/stale/padding/replacement behavior,
cross-source splicing and forged-attribution rejection, staged
success, progress, deterministic issue ownership, disconnected topology,
descriptor/transcript/graph disagreement, reordered-graph equivalence, stale
identity, idempotence, conflict, readiness
withdrawal, corrupted state and fail-atomic negative paths. Real WebRTC
binding, browser UI projection, disconnect timing and human phrase usability
remain explicit post-gate work.
