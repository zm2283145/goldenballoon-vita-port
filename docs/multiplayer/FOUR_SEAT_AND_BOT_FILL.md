# Four online seats and bot-fill: seat model, design and staged plan

Scope: what it would take to run three and four online racers on the shipping
native route, and how to fill an unoccupied online seat with a deterministic
computer racer. This is a scoping and design document. It enables nothing, it
changes no runtime gate, and it does not claim any evidence.

The governing constraints stay in force. Online race admission remains
fail-closed until the written A3 decision
([`STATUS.md`](STATUS.md)); the capability boundary recorded in
[`NATIVE_MULTIPLAYER_QUALITY_PLAN.md`](NATIVE_MULTIPLAYER_QUALITY_PLAN.md)
still says that multi-peer support needs rekey, membership, authority, launch
and disconnect qualification *before* more seats are exposed, and that removing
a limit is not an implementation. Nothing below proposes removing a limit on
its own.

## 1. Summary

The seat model is already four wide almost everywhere that matters. Every wire
structure, every ring, every roster and both reducers are sized for four. The
two-endpoint shape is a launcher policy fence plus product copy, not a protocol
limit.

That is the good news and it is also the trap: because the caps are policy, the
code paths behind them have never run at three or four, and three of them are
*silently wrong* rather than refusing. One is a hard protocol failure that would
stop a three-peer room from ever reaching a race.

Ranked by what actually blocks a four-player race:

| # | Blocker | Where | Kind |
|---|---|---|---|
| 1 | Preflight graph digest cannot agree at N>=3 | `platform/online/match_live_adapter.cpp:2678` | Protocol defect |
| 2 | Any peer loss ends the race for everyone | `platform/online/match_live_adapter.cpp:3215`, `platform/app/main_app.cpp:1592` | Product model |
| 3 | Departure-hold divergence at N>=3 (N8-D-r2) | `OPERATIONAL_BACKLOG.md:332` | Open residual, owner-reserved |
| 4 | Non-leader resolves the wrong endpoint as itself | `platform/app/ui_online_room.cpp:1824`, `platform/net/party_link.c:159` | Silent defect |
| 5 | Native screen handoff refuses any room that is not exactly two | `platform/app/online_live_wiring.cpp:913` | Policy fence |
| 6 | Per-peer rekey on a peer bump is not closed at 3-4P; commitment nonce goes stale | `platform/online/match_peer_transport.cpp:486,507` | Known gap, security-relevant |
| 7 | Six NAT pairs instead of one, STUN-only, forwarder unwired | `platform/app/online_live_wiring.cpp:96`, `platform/online/match_peer_transport.h:81` | Connectivity |
| 8 | One route-quality measurement slot for three routes | `platform/online/match_live_adapter.cpp:4635` | Under-modelled |
| 9 | Roughly thirty two-player copy sites plus four pinned doc claims | `platform/app/ui_online_room.cpp`, `tests/test_product_claim_boundaries.py:177` | Copy |

Bot-fill, by contrast, is close. The deterministic computer-racer takeover it
needs already exists, is already four wide, and — used for bot-fill rather than
for a disconnect — sidesteps the one consensus problem that makes the
disconnect case hard.

## 2. Where the two-endpoint shape actually lives

Nothing in the protocol limits the room to two. Both reducers admit four
endpoints and four seats:

- `platform/online/lobby_core.h:13-15` — `MDKR_ONLINE_MAX_ENDPOINTS 4u`,
  `MDKR_ONLINE_MAX_SEATS 4u`, `MDKR_ONLINE_MAX_SEATS_PER_ENDPOINT 2u`.
- `services/party/src/match/protocol.ts:2-5` — the same three numbers.
- `platform/net/match_manifest.c:46` — `slot_count` is validated `2..4`.

The shape comes from four places, all in the launcher:

1. `platform/app/online_live_wiring.cpp:913` — `if (vm.member_count != 2u)
   return false;`. This is the real gate. A three- or four-member room is
   admitted by every reducer and then sits in SELECTING forever, because the
   native screen handoff never fires. Called every frame from
   `platform/app/ui_online_room.cpp:2523`.
2. `platform/app/online_live_wiring.cpp:353` and `:1692` — `localSeatCount =
   1u`. This caps *local seats per endpoint*, not endpoints. It is what keeps
   the couch pair off the online route, not what keeps the room at two.
3. `CMakeLists.txt:25-26` and `platform/app/online_live_wiring.cpp:15-21` — the
   declared fences: two endpoints, retail identities, STUN-only.
4. Product copy, everywhere in `platform/app/ui_online_room.cpp`, and four
   strings hard-pinned by `tests/test_product_claim_boundaries.py:175-183`
   including "A race is two players, not more."

## 3. Seat-model map

### 3.1 Lobby, roster and seat assignment

`JOIN` carries the requested seat count in `command->value`
(`platform/online/lobby_core.c:517-531`). Admission is one arithmetic site:

```c
/* platform/online/lobby_core.c:79-95 */
static unsigned free_seats(const MdkrOnlineLobby *lobby) {
    return MDKR_ONLINE_MAX_SEATS - lobby->seat_count;
}
...
    if (endpoint_id == 0u || seat_count == 0u ||
        seat_count > MDKR_ONLINE_MAX_SEATS_PER_ENDPOINT ||
        lobby->member_count >= MDKR_ONLINE_MAX_ENDPOINTS ||
        free_seats(lobby) < seat_count || member(lobby, endpoint_id) != NULL) {
        return false;
    }
```

Seats are first-fit into the lowest free index (`lobby_core.c:104-119`). LEAVE
compacts order-preserving and shifts the tournament series with the same
permutation (`lobby_core.c:419-459`).

**Today:** correct and already four wide. `all_ready()`'s `member_count < 2u`
(`lobby_core.c:338-346`) is a minimum, not a maximum.

**For four:** no reducer change. Two notes. `PUBLISH_RESULTS` packs one
placement byte per seat into a `uint32_t` (`lobby_core.c:670-709`) — exactly
four bytes, zero headroom, so four is the structural ceiling of the command.
And a RESULTS-phase leave renumbers the survivors' canonical slots for the next
race; at two players that was invisible because a leave stranded the room.

### 3.2 Canonical seat mapping and how peers agree

Canonical slot *k* is the *k*-th occupied seat in seat order. Three sites derive
it independently and identically: `platform/online/match_live_adapter.cpp:325-330`
(manifest owners), `platform/online/match_launch_builder.c:53-64` (selections),
`platform/online/match_live_adapter.cpp:4442-4459` (results attribution).

Peers agree by **single-writer authority, not replicated reduction**. Every peer
receives the same ordered seat array from one authoritative service reducer and
copies it verbatim (`platform/online/match_live_transport.cpp:1256-1284`). The
race seed is hashed from room id, epoch, leader generation and leader endpoint
id only (`match_live_adapter.cpp:303-316`), so it agrees for the same reason.

**For four:** no change to the derivation. But nothing tests it above two, and
the argument rests entirely on the service being the single writer.

Two defects that only bite at N>=3, both silent:

- `platform/app/ui_online_room.cpp:1824-1834` — `betaLocalEndpoint()` returns
  the *first occupied non-leader member* when this client is not the leader. At
  three or four that is the wrong peer. It mislabels "You" in the roster strip
  and corrupts `is_local` at `:2146`, `:2179`, `:2434`, `:3013`.
- `platform/net/party_link.c:141-160` — `party_link_resolve_local()` returns
  `candidates == 1u ? candidate : 0u`. At three or more endpoints a non-leader
  always resolves to zero, so no seat is ever marked local. The header comment
  at `:137-140` admits the assumption. Its blast radius is much wider than the
  roster strip, and it lands the moment a *third* endpoint joins:
  `online_session_snapshot_has_local_seat()`
  (`game/src/online/online_session.c:1086-1095`) returns false, which
  short-circuits the remote-vacate debounce at `:1281` — session-end detection
  stops working for that client — and breaks the CHARSELECT entry gate at
  `:1739`; `partyLinkBuildLocalView()` never sets `have_seat`
  (`platform/app/online_live_wiring.cpp:589-598`), so character and vehicle
  selection never converges; and worst, the peer-loss loop at
  `online_live_wiring.cpp:561-565` vacates every seat that is `occupied &&
  !is_local`, which with `is_local` false everywhere means a remote peer's loss
  marks the local player's own seat vacated.

### 3.3 Launch descriptor and manifest

Both are fixed-size, already four wide, and need **no format change** for four
human seats.

`MdkrMatchManifestV1` (`platform/net/match_manifest.h:23-37`) is 112 bytes with
`slot_owner[4]` and `slot_count`. Layout at `match_manifest.c:83-98`; byte 109
is the only spare and the decoder **strictly requires it to be zero**
(`match_manifest.c:109`), so it is a clean forward-compatible extension point
that an old peer refuses rather than misreads.

`MdkrMatchLaunchDescriptorV1` (`platform/net/match_launch_descriptor.h:28-32`)
is 148 bytes: 8 header, 112 manifest, 24 selections (4 x 6), 4 checksum
(`match_launch_descriptor.c:64-76`). **Zero spare bytes.** Any new descriptor
field is a size and version change.

`mdkr_match_launch_descriptor_validate` (`match_launch_descriptor.c:27-55`)
already does the work bot-fill would otherwise have to reinvent: for every
occupied slot it requires a nonzero revision, a legal character, a legal vehicle
inside the manifest's vehicle mask, and **character uniqueness across occupied
slots**; every unoccupied slot must be `NO_CHARACTER`/`NO_VEHICLE`.

Two findings here.

**The manifest cannot express two seats on one endpoint.**
`mdkr_match_manifest_validate` requires distinct owners per occupied slot
(`match_manifest.c:55-60`), while `manifestFromLobby` writes the seat's
`endpoint_id` once per occupied seat (`match_live_adapter.cpp:325-330`). A
member that joined with `seatCount = 2` therefore writes the same id into two
slots and the manifest fails to validate — the launch refuses with no specific
diagnostic. Couch-pair online is structurally impossible today, not merely
fenced off. The `two-local` endpoint in the convergence gate does not contradict
this: that manifest is fabricated with four distinct owners and the local mask
is supplied separately by environment.

**`m.slot_count` is assigned twice** in `manifestFromLobby`
(`match_live_adapter.cpp:335` then `:341`). The second wins. They agree today
only because `lobby.seat_count` is maintained as the count of occupied seats.
Harmless now, silently wrong if they ever diverge.

### 3.4 Input packet and bundle

The 24-byte `MDKR_MATCH_INPUT_PACKET` **never touches the network**. Its only
callers are the in-process loopback ingress at `platform/app/main_app.cpp:436-466`.
Byte 17 is the pad sample's `present` flag hardcoded to 1
(`platform/net/match_input_packet.c:56`, `:73`), not a seat count. `canonical_slot`
is already validated `< 4`.

The live carrier is the 64-byte `MdkrMatchInputBundle`
(`platform/net/match_input_bundle.h:15-27`): three frames x
`MDKR_SESSION_MAX_PLAYERS` (4) samples, with a four-bit `slot_mask`. Each
endpoint sends only its own slots (`match_live_adapter.cpp:4133`) and fans the
bundle out to every peer, sealing a separate envelope per peer
(`platform/online/match_peer_transport.cpp:1983`).

**For four:** no change. Upstream cost at four peers is roughly a 132-byte
envelope x 3 peers x 30 Hz, about 12 KB/s.

### 3.5 Transport, mesh, crypto and preflight

The mesh is a genuine N-way full mesh: one `PeerRuntime` per remote endpoint in
a `std::map` (`platform/online/match_peer_transport.cpp:281`), roster admitted
at 2..4 (`:1922-1924`), every ladder a per-peer loop (`:1466`). Glare is
impossible by construction — the lower endpoint id is the sole offerer
(`match_peer_transport.h:43-46`) — and the offer ladder is explicitly sized for
"<= 3 simultaneous offers" (`:130-133`).

Signalling models a room of four: directed delivery to one exact
endpoint/generation (`services/party/src/match/match-room.ts:558-587`), welcome
carrying at most three sorted non-self peers
(`platform/online/match_signal_client.cpp:303`).

Crypto is exactly sized for four, not merely headroomed:
`MDKR_MATCH_PEER_KEYRING_SLOTS 18u` is "three lanes in each direction toward
each of up to three remote peers" (`platform/net/match_peer_crypto.h:95-96`).
The transcript and phrase ceremony take 2..4 entries, sorted by endpoint id
before hashing (`platform/net/match_peer_transcript.c:103-109`), producing one
phrase for the whole roster.

Sized for four is not the same as *closed* for four, and the difference is
executing code rather than a comment. `rekeyPeer()` computes
`const bool soleOtherPeer = peers.size() == 1u;`
(`platform/online/match_peer_transport.cpp:486`) and refreshes this endpoint's
own commitment nonce only in that branch (`:507`). The source states the
consequence plainly (`:470-483`): in a three- or four-peer mesh a single peer's
bump does not change this endpoint's generation, bystanders never reset their
lane and cannot accept a re-hello, so this endpoint keeps a stale nonce and
relies on the reconnecting peer's entropy plus the re-verify barrier. Full
per-peer-bump grind resistance at 3-4P is explicitly deferred to the
full-mesh-rekey protocol; two players is "fully closed". Any four-seat work must
treat this as a security precondition, not a nicety.

**And then the blocker.** `buildGraph()`
(`platform/online/match_live_adapter.cpp:2678-2714`) sets edges only between the
local endpoint and each ready peer. Its comment — "Both peers derive the same
topology" — holds only at N=2. At three or four each endpoint builds a
*different star centred on itself*, so each hashes a different `graph_digest`.
`mdkr_match_preflight_evaluate` compares every peer's attested graph digest
against the local one and returns `MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH`
(`platform/net/match_preflight.c:1048-1050`) **before** it ever reaches the
admissibility check at `:1051`. A three-peer room would never reach READY.

This is a code defect at N>=3, not a policy fence, and it is the single item
that must be fixed first because everything downstream is unreachable without
it.

### 3.6 Snapshot and rollback ring

Sizing is a non-issue, and four players are cheaper than the worst two-player
case.

- `MDKR_ROLLBACK_SNAPSHOT_SLOTS 32u`, `MDKR_ROLLBACK_MAX_RING_BYTES 16 MiB`
  (`platform/rollback/rollback_limits.h:8,16`), enforced per slot at
  `platform/rollback/rollback_ring.c:13-14` — a hard 524,288-byte ceiling per
  snapshot.
- Snapshot content is not dimensioned by player count
  (`platform/rollback/rollback_game_authority.c:397-583`): pools are registered
  whole, `gRacers` is a fixed `[10]`, `gCameras` a fixed `[8]`.
- Measured: the largest standard-track row is 508,513 bytes / 16,272,416-byte
  ring; the qualified native four-player route is 500,225 bytes across 143
  ranges (`STATUS.md:23,178`, `docs/ref/rollback-authority-v1.md:117-118`).
  Four players sit at the *low* end of the band. Range headroom is 192 - 155 =
  37 (`platform/rollback/rollback_snapshot.h:14`).
- Input history is `[128 ticks][4 slots]` in one flat array
  (`platform/net/net_input.h:14-15,41-56`); the usable reconciliation window is
  30 ticks (`platform/net/match_transport.h:17-18`); engine-side authored rows
  are 32 x `MDKR_INPUT_PORTS` (4).

Note that `docs/evidence/multiplayer/rollback.md:11,13` still quotes the
pre-subpool era (297,441-305,729 bytes). Do not size anything off that table.

### 3.7 Presentation and audio

Four-way split is retail code (`game/src/camera.c:2131-2199`,
`cam_set_layout` at `:1619-1643`), and the online path reuses it verbatim. The
only difference is what feeds the loop count:

```c
/* game/src/tracks.c:1291-1301 */
endpointMappedViews = mdkr_net_roster_runtime_active();
if (endpointMappedViews) {
    /* Keep the canonical four-player layout installed for fixed-tick
     * authority. Only the number/placement of presentation passes is
     * endpoint-local. */
    numViewports = (s32)mdkr_net_roster_runtime_viewport_count(0u);
} else {
    numViewports = cam_set_layout(gScenePlayerViewports);
}
```

That comment is the most fragile invariant in the whole design. `gNumViewports`
is read by racer physics and AI (`game/src/racer.c:2264`, `:2811-2845`, `:3524`,
`:4073`, `:6059-6091`, and more). Any future code that calls
`cam_set_layout(local_viewport_count)` during an online race forks the
simulation, and nothing guards against it structurally.

An endpoint declares its seats in two layers, both four wide:
`mdkr_net_roster_configure_local` (owns input) then
`mdkr_net_roster_set_viewports` (renders), the latter refusing any viewport that
is not a local seat (`platform/net/net_roster.c:38-68`).

Audio collapses to one endpoint listener at any seat count
(`platform/net/local_listener_mix.c:22-65`): zero local viewports is silent,
the loudest local candidate wins, and two or more local viewports force centre
pan. One retail-original consequence for three and four players: the engine-idle
sound layer is gated on `numCameras <= 2` (`game/src/audio_vehicle.c:1155-1156`)
and is simply absent.

### 3.8 Save and progression custody

There is no per-seat custody question, and that is by construction rather than
by luck.

`mdkr_rollback_game_runtime_host_io_allowed(bool progression_write)`
(`platform/rollback/rollback_game_runtime.h:47-53`) is the final host-I/O
firewall: while a rollback match is active, resimulation may not touch the host
and progression may not be persisted from the match timeline. A rejected write
is reported to gameplay as consumed so the online timeline does not retry it
forever. It gates `osEepromWrite`, `osEepromLongWrite` and `virtual_pak_store`
in `platform/stubs_dkr.c`, proven by `tests/test_durable_rollback_firewall.py`.

The gate is seat-count independent. Four seats add nothing to answer here.
Shared progression, trophies and tournament custody remain explicitly out of
scope (`NATIVE_MULTIPLAYER_QUALITY_PLAN.md`, capability 6).

### 3.9 Teardown

Per-seat teardown is already generic; the *policy* is not.

`endDepartedRace()` (`platform/online/match_live_adapter.cpp:3616-3618`) retires
the peer and calls `onPeerLost()`, which sets a single global
`racePeerLost_ = true` (`:3215`). The launcher then ends the whole session:

```c
/* platform/app/main_app.cpp:1592-1599 */
if (drainTick > info.firstTick &&
    mdkr_online_live_adapter_race_peer_lost(ctx->visible)) {
    ctx->endReason = LiveRaceEndReason::OpponentLeft;
```

At four players one person quitting ends the race for the other three, on the
same pump that just scheduled the takeover for their seat. `racePeerLost_` has
to become "no remote human seats left", not "any peer left".

Three teardown paths actually run, and none of them is per-seat:

- Engine exit, one global runtime teardown per race:
  `mdkr_online_session_leave_race()` and its two siblings
  (`game/src/online/online_session.c:1392`, `:1357`, `:1297-1302`) each call
  `mdkr_rollback_game_runtime_level_end()`
  (`platform/rollback/rollback_game_runtime.c:532`) and then request exit.
- Network teardown in `~LiveAdapter` (`platform/online/match_live_adapter.cpp:410-426`),
  which carries the only enforced ordering in the area: send `LEAVE`, close the
  mesh announcing peer end, reset the mesh *before* the backend it borrows its
  feed from.
- Double-teardown guards: `State::teardown()`'s `closed` latch
  (`platform/online/match_peer_transport.cpp:1851-1852`),
  `OnlineRoom_beginAppExit()`'s `sOnlineAppClosing`
  (`platform/app/ui_online_room.cpp:3345`), and `teardownAdapterAsync()`
  (`:3283-3314`), which detaches the destructor because it joins mesh and
  signal worker threads and can stall for seconds.

Two structural cautions for four seats.

`MDKR_SESSION_MAX_EFFECTS` is `2u` (`platform/session/session_types.h:15`), and
a failed `append_effect` makes the command invalid, which
`SessionRuntime::dispatch` propagates as a rejection
(`platform/app/session_runtime.cpp:15-16`). A teardown that needs a third
ordered effect would not merely drop the effect; it would make
`LEAVE_ONLINE_RACE` fail.

The session effect contract is a parallel, unexecuted specification. Nothing
outside `session_core.c` / `session_types.h` reads `effects[]`, and the only
caller of `lastStep()` (`platform/app/main_app.cpp:822`) reads `.error` for a
log line. Real teardown ordering is hand-written at the call sites and merely
happens to agree with the documented contract. More seats means more
departure and rejoin permutations, each hand-ordered, with nothing enforcing
the contract that was written to govern them.

Related, and worth knowing before anyone reaches for it: eight lifecycle
headers in `platform/online/` (`reserved_cleanup_worker.h`,
`transport_retirement.h`, `rtc_initialization_transaction.h`,
`prepared_work_queue.h`, `queued_work_admission.h`, `receive_work.h`,
`startup_stage_ownership.h`, `reserved_owner_work.h`) are included by their own
unit tests only. No production translation unit includes any of them.

## 4. Bot-fill design

### 4.1 What already exists

The deterministic computer-racer takeover is real, and it is the original game's
AI rather than recorded input:

```c
/* game/src/racer.c:4996-5011 */
if (tempRacer->playerIndex == PLAYER_COMPUTER || networkAiTakeover) {
    /* The frozen canonical identity remains a human slot. Only the vehicle
     * solver sees PLAYER_COMPUTER, for this authored tick, so DKR's AI path
     * (including its CPU-field assumptions) drives the disconnected seat
     * without changing roster/camera/HUD ownership. Every endpoint obtains
     * the same mask from the launcher control schedule, including replay. */
    const s16 savedPlayerIndex = tempRacer->playerIndex;
    if (networkAiTakeover) tempRacer->playerIndex = PLAYER_COMPUTER;
    update_AI_racer(obj, tempRacer, updateRate, updateRateF);
    if (networkAiTakeover) tempRacer->playerIndex = savedPlayerIndex;
```

`networkAiTakeover` comes from `mdkr_match_input_runtime_slot_ai_controlled()`
(`game/src/racer.c:4931-4934`), whose mask is refreshed by
`mdkr_match_input_runtime_begin_tick()` on the authored path
(`platform/rollback/rollback_game_runtime.c:1038`) **and** inside the correction
replay loop (`:961-963`). The header states the contract: the mask is derived
from the immutable launcher control schedule, not from restored host state
(`platform/net/match_input_runtime.h:57-59`).

Be clear about how much this has actually run, though. On the shipping
two-endpoint path the mask is "almost always mask 0" by the seam's own comment
(`platform/online/match_live_adapter.cpp:4368-4374`), because any peer loss ends
the race on the same pump that schedules the takeover. The mechanism is proven
at the tick level and has never raced a kart to a finish line in production.
Bot-fill would be its first real use, which is an argument for building it and
an argument for not trusting it until a lane drives it end to end.

Underneath, the schedule is immutable and per-slot:
`mdkr_match_transport_schedule_ai_takeover()`
(`platform/net/match_transport.c:424-465`) accepts one activation tick per slot,
returns `DUPLICATE` for the same tick and `CONFLICT` for a different one, and
has no mid-race handback (`match_transport.h:124-126`). From the activation tick
the transport authors one neutral **received** pad per tick for the seat as the
canonical history placeholder and back-dates the confirmation frontier so the
gap detector stops chasing the departed peer (`match_transport.c:297-347`).

### 4.2 Why the disconnect case is hard and bot-fill is not

The AI is deterministic. The *decision* is the weak half, and only for
disconnects. At three or more endpoints:

- The proposer is the lowest surviving endpoint id
  (`mdkr_match_drop_is_proposer`, `platform/net/match_transport.c:555-565`) and
  the tick must reach every survivor within one input delay.
- A survivor that heard the peer holds the verdict while a proposer that heard
  silence proposes, and the two commit different canonical input for that seat.
  This is `OPERATIONAL_BACKLOG.md:332` (N8-D-r2), marked **must be answered
  before GO**, and it is echoed in the source at
  `platform/online/match_live_adapter.cpp:3459-3471`.
- `TOO_LATE` / `CONFLICT` from `finaliseDepartedSeats`
  (`match_live_adapter.cpp:3306-3330`) is recorded in the forensics ring and
  otherwise ignored — the source comment says so plainly: an endpoint that did
  not finalise where its peers did simply keeps racing a divergent simulation.

**Bot-fill has none of this**, because its activation tick is not negotiated at
all. It is frozen in the checksummed launch descriptor that every peer validated
before tick one. That is the design's central claim and the reason to prefer it
over any scheme that decides bot behaviour at runtime.

### 4.3 The design

A bot seat is a **canonical slot occupied by an authored computer racer**,
scheduled for takeover at the first authored tick from frozen state.

1. **Lobby.** `MdkrOnlineSeat` (`platform/online/lobby_core.h:99-107`) gains a
   `bot` flag alongside `occupied`. Only the leader may set or clear it, only in
   `MDKR_ONLINE_LOBBY`, and a bot seat carries a leader-chosen character and
   vehicle exactly like a human seat. Capacity arithmetic is unchanged: a bot
   seat consumes a seat, and a joining human is refused with the existing
   `MDKR_ONLINE_ERROR_CAPACITY` unless the leader frees the seat first.
2. **Owner id.** `mdkr_match_manifest_validate` requires a nonzero, unique owner
   per occupied slot (`match_manifest.c:55-60`). Bot seats take reserved ids
   derived deterministically from the manifest — for example the room id folded
   with the slot index into a reserved high range that no service-assigned
   endpoint id can occupy. Every peer derives the same value from the same
   frozen inputs. The reserved range must be validated as non-assignable, not
   merely assumed.
3. **Mesh isolation.** `buildMeshRoster()`
   (`match_live_adapter.cpp:1830-1844`) walks occupied seats and would otherwise
   try to open peer channels to a bot. It must skip bot seats. So must
   `survivingEndpoints()`, the proposer rule, the liveness trackers and the
   preflight graph — a bot is a racer, never an endpoint.
4. **Descriptor.** No format change. Bot seats are ordinary occupied slots, so
   `mdkr_match_launch_descriptor_validate` already enforces their character
   uniqueness against the human picks (`match_launch_descriptor.c:42-45`), and
   `get_character_id_from_slot()` already returns the authored character
   (`game/src/menu.c:18088-18107`) through the online-to-engine mapping.
5. **Arming.** At `MDKR_ENGINE_READY`, before the first drain, the launcher
   calls `schedule_ai_takeover(slot, firstAuthoredTick)` for every bot slot in
   the descriptor. Bot slots are in `remote_slot_mask` on every endpoint
   (`match_transport.c:155`), which is what makes the transport author the
   neutral placeholder pad and advance confirmation (`:336-347`), so a bot seat
   never registers an input gap.
6. **Simulation.** From tick one `mdkr_match_input_runtime_slot_ai_controlled()`
   is true for that slot, `racer.c:5006` hands the kart to `update_AI_racer`,
   and the seat races as a computer racer while keeping its canonical identity,
   its results attribution and its place in the roster.

### 4.4 How determinism is preserved

Four independent reasons, each already load-bearing for the shipping two-player
path:

- **The decision is authored, not observed.** The set of bot slots and their
  activation tick come from the launch descriptor. Its own trailing word is an
  FNV-1a checksum against corruption, not authentication
  (`match_launch_descriptor.c:17-25,75`); the agreement that matters is the
  preflight barrier, where every peer attests a 32-byte descriptor digest over
  the authenticated transcript and any disagreement returns
  `MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH`
  (`platform/net/match_preflight.c:1040-1042`). No peer decides anything at
  runtime, so there is no proposal, no round trip, and no N8-D-r2 hold
  asymmetry.
- **The mask is re-derived on every boundary, including replay.**
  `mdkr_match_input_runtime_begin_tick()` runs on the authored path and inside
  the correction replay loop, and failure aborts the tick rather than defaulting
  to "no AI" (`rollback_game_runtime.c:961-974`, `:1038-1046`).
- **The AI reads only snapshotted state.** `update_AI_racer` draws from
  `rand_range`, and both RNG words are registered snapshot authority
  (`rollback_game_authority.c:491-492`). The per-race seed is folded from the
  frozen manifest before the level loads (`game/src/online/online_race_boot.c:71-78`).
  AI nodes, checkpoints, racer structs and the object pool are all registered
  ranges. There is no clock, no environment and no controller read on that path.
- **Seat indexing is identity across endpoints.** Canonical slot equals engine
  port equals `playerIndex` (`rollback_game_runtime.c:276-299`).

The bot is therefore never "simulated per peer". It is one term of the shared
simulation that every peer already resimulates identically, selected by a bit
that every peer read out of the same artifact, whose identity every peer
attested to before tick one.

### 4.5 A human joining a bot seat mid-session: refused

Refused during a race, allowed between races. Four independent reasons for the
first half, any one of which is sufficient:

- The launch descriptor is frozen and checksummed per epoch; there is no
  mechanism to amend it mid-race.
- The takeover schedule is explicitly immutable with no handback
  (`match_transport.h:124-126`); a second tick for the same slot is a
  `CONFLICT`, and post-takeover ingress for the slot is refused with
  `MDKR_MATCH_INGRESS_TAKEN_OVER` (`match_transport.c:194-202`).
- The engine roster is copy-installed for the engine's bounded lifetime and
  cleared only after its threads join (`platform/net/net_roster_runtime.h:15-17`).
- A late joiner would need the current simulation state, and state resync is
  explicitly post-GO work (`STATUS.md`).

Between races the answer is clean and needs no new machinery: a rematch produces
a new epoch, a new manifest and a new descriptor, and the reducer already
handles a join into a freed seat. The rule to implement is simply that the
leader converts a bot seat back to an open seat in `MDKR_ONLINE_LOBBY` or
`MDKR_ONLINE_RESULTS`, and the next race is built from the new lobby.

### 4.6 The alternative that was considered and rejected

The engine has a second, simpler way to add computer racers: field more karts
than seats and let the ordinary spawn policy fill the tail. The local Adventure
Party already does exactly that
(`game/src/objects.c:4091-4111`, setting `numPlayers = humans` and
`gNumRacers = total` so the spawn loop creates indices `humans..total-1` as
computer players).

It is rejected as the primary mechanism for three reasons. The racer field is
currently derived, not authored — `gNumRacers = numPlayers` for `numPlayers >= 3`
(`game/src/objects.c:4059-4060`) and the bucket at
`game/src/menu.c:18209-18210` picks "no AI" for the two-player case — so
authoring it needs the one spare manifest byte and a manifest version bump.
Non-seat bots are invisible to the lobby, so nobody can see or choose them. And
their characters fall back to `gCharacterIdSlots[slot] = slot`
(`game/src/menu.c:18053-18058`, `:18107`), which can collide with a human's
pick, requiring new collision-free assignment logic that the descriptor
validator already provides for free.

It remains the right mechanism for a *larger field* — six racers with two humans,
two bots and two plain opponents — and the two compose cleanly. It should be
built second, not first.

## 5. Staged plan

Each stage is independently reviewable and none of them enables anything.

**S1 — Make the preflight graph agree at N>=3.** `buildGraph()` is only ever
called from `runPreflight()` (`match_live_adapter.cpp:2591`), and only after the
gate at `:2579` proves every roster peer's channels are ready locally. At that
point the endpoint may lawfully assert the complete graph: set each endpoint's
`reachable_mask` to `((1u << N) - 1u) & ~(1u << i)`, which satisfies
`graph_valid` (`platform/net/match_peer_graph.c:29-32`) and is byte-identical on
every peer, since `meshRoster_` is built from a `std::map` and is therefore
already in canonical endpoint-id order. The assertion can only be wrong about a
pair this endpoint cannot see, and a peer whose own star is incomplete never
builds a graph and never attests — so preflight stalls at
`WAITING_FOR_PEERS` instead of failing with `GRAPH_MISMATCH`. Fail-closed in the
right direction. Fix the generation fallback in the same change: falling back to
the local `meshGeneration_` for a peer whose generation is unknown
(`match_live_adapter.cpp:2688-2692`) produces a value that peer will not
reproduce; refuse to build the graph instead.

**S2 — Per-seat peer loss.** Replace the global `racePeerLost_`
(`match_live_adapter.cpp:3215`) with a per-seat verdict, and change
`main_app.cpp:1592-1599` to end the session only when no remote human seat
remains. Without this, bot-fill still works but a disconnect still ends
everyone's race, and the takeover never actually plays out.

**S3 — Bot seats.** The six steps in 4.3. This is the largest single piece and
the one with the clearest determinism story.

**S4 — Correct the N>=3 identity defects.** `betaLocalEndpoint()`
(`ui_online_room.cpp:1824`) must take the real local endpoint id from the
adapter, and `party_link_resolve_local()` (`party_link.c:141-160`) must be given
an explicit local endpoint id rather than inferring one. Both are wrong today at
three or more and neither refuses.

**S5 — Per-seat view model.** `MdkrOnlineViewModel`
(`platform/online/lobby_view_model.h:177-199`) exposes only three scalars and no
per-seat data at all, which is why `ui_online_room.cpp` already reaches around
it into the raw lobby at `:2200` and `:2434`. Add a real
`MDKR_ONLINE_MAX_SEATS`-wide array — seat index, endpoint id, is_local, is_host,
is_bot, connected, ready, character, vehicle, points, last placement, label —
so the panel, the accessibility announcer and the browser parity lanes read one
audited projection.

**S6 — Copy and product claims.** Roughly thirty sites in `ui_online_room.cpp`
plus `lobby_view_model.c:789`, and the four strings pinned by
`tests/test_product_claim_boundaries.py:175-183`. Note `lobby_view_model.c:257-260`
already says "4 racer seats"; `ui_online_room.cpp:2285-2311` deliberately
overrides it back down to two and that override is what should be deleted.

**S7 — Evidence.** Extend `threePeerMeshEveryoneReachesEveryone`
(`tests/test_match_peer_transport.cpp:825-831`) to four. Add a genuinely
concurrent four-process networked lane; the existing four-process gate
(`tests/check_online_process_convergence.py`) runs its endpoints
**sequentially** via a blocking `subprocess.run` and constructs no mesh or
signal client at all, so it proves determinism and nothing about networking.
Register the takeover arm — `--ai-takeover-slot` appears in no runner today, so
the four-process takeover evidence behind `STATUS.md:23` was produced manually
and is not defended by the routine suite. And replace
`tests/test_ai_takeover_adapter.py`, which is a source-text grep suite of the
class already ruled against in G-5.

**Not in scope and still owner-reserved:** N8-D-r2, the 3-4P per-peer-bump
rekey (`match_peer_transport.cpp:486,507`), relay or one-hop forwarding for six
NAT pairs, and any change to the `member_count != 2u` handoff gate. The last one
is the actual admission fence and must be the last thing anyone touches.

## 6. Risks

**Six NAT pairs, STUN-only, no fallback.** Two players need one pair to
traverse; four need six, and one failure fails the race. TURN minting exists
server-side (`services/party/src/turn.ts:134-154`) but is unprovisioned, and the
launcher strips every relay entry before the mesh sees it
(`platform/app/online_live_wiring.cpp:96-101`). One-hop forwarding is designed,
sized and unit-testable but explicitly not wired
(`platform/online/match_peer_transport.h:81-84`). Four-player online without one
of the two will have a materially worse connection rate than two-player, and no
amount of seat work changes that.

**The N8-D-r2 divergence is a correctness bug, not a polish item.** Two
survivors committing different canonical input for a departed seat is a silent
desync. It is unreachable at two players and reachable the moment a third joins.
Bot-fill deliberately avoids it; a real disconnect at four players does not.

**`TOO_LATE` / `CONFLICT` is diagnosed and then ignored.** An endpoint that
fails to finalise where its peers did keeps racing a divergent simulation
(`match_live_adapter.cpp:3306-3330`). At two players the cushion is generous
because the sole survivor applies its own tick synchronously; at four the
proposal has to reach everyone inside one input delay, roughly 66-133 ms at
30 Hz.

**One route measurement for three routes.** `routeMeasure_` /
`routeMeasurement_` are single values with one origin endpoint
(`match_live_adapter.cpp:4635-4636`, `platform/net/match_preflight.h:143`). The
answer for a four-way mesh is not a scalar.

**The canonical-layout invariant is unguarded.** If anything ever calls
`cam_set_layout` with a local viewport count during an online race, every peer
forks. There is no structural guard, only the comment at `game/src/tracks.c:1291-1301`.

**Nothing here has run above two.** Every reducer, ring and codec is four wide
and every one of those paths is untested at three and four. The maximum lobby
coverage in the tree is three members
(`tests/test_online_lobby_core.c:970-990`) and the maximum real mesh is three
peers. A green suite today says nothing about four.
