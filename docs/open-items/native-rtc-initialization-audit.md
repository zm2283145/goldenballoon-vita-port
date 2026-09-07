# Native RTC initialization failure audit

Status: RTC-INIT-01 through RTC-INIT-04 now have implemented source candidates;
behavioral qualification remains required. This audit is not a runtime pass or
evidence that native multiplayer release qualification is complete.

## Scope and source identity

Reviewed on 2026-09-06 against libdatachannel **0.24.5**, commit
`443f6934d9007eb7076ab7825ba330f355fcbead`, pinned in
`cmake/datachannel.cmake:13`. The shipped dependency configuration enables
WebSocket and Mbed TLS, disables GnuTLS and libnice, and disables media
(`cmake/datachannel.cmake:78`). ICE therefore uses libjuice; the optional
DTLS-SRTP/media initialization path is not part of this assessment.

Dependency-relative references below resolve inside the pinned libdatachannel
source, including its named submodules. `src/impl/init.cpp` line references use
the baseline after the local reserved-cleanup-worker amendment, before the newer
initialization/stage amendments; other finding references use that pinned baseline.
Current disposition below distinguishes the new code from those original defects.
Private runtime evidence is not included here.

The reserved-worker amendment addresses a different defect: creating a cleanup
thread from the final token's noexcept destructor. Its helper reserves cleanup
and publication workers before initialization. The parent integration reported
successful optimized and ASan/UBSan **builds** of the game and helper fixture;
this is not an executed fault-injection or runtime pass.

**The existing initialization-refusal fixture proves cancellation of the
reserved helper only. It does not prove rollback of partially initialized RTC
subsystems.**

## Current candidate disposition

| Finding | Implemented candidate | Remaining proof |
| --- | --- | --- |
| RTC-INIT-01 | `libdatachannel-initialization.patch` and shared `rtc_initialization_transaction.h`: publish/arm the reserved retirement future before acquisition; commit ready only after all starts; track partial pool and successful subsystem ownership; retire in dependency order; retain failure/ownership on refused cleanup; admit a new epoch only after successful completed retirement. `token()` retains live weak owners first, and `preload()` restores the same owner/future instead of creating another token. | Actual dependency fault injection, concurrent owner/preload/cleanup journeys, successful retry and failed-retirement refusal on all shipping platforms. |
| RTC-INIT-02 | `libdatachannel-startup-stages.patch` calls the shared `mdkrStartOwnedPolling`: stage allocations, publish before thread creation, and restore empty stopped ownership on construction refusal. | Run the helper fixtures and actual PollService refusal/recovery; no join of a nonexistent worker. |
| RTC-INIT-03 | The same stage patch calls `mdkrStartOwnedRegistry`, allocating before C startup. The initialization transaction marks SCTP owned before settings conversion. | Allocation and settings-refusal coverage against the actual dependency, including owned SCTP retirement. No new C-internal error/exit guarantee. |
| RTC-INIT-04 | Connection configuration, WebSocket construction, callback setup and open share an exception boundary. Publication follows registration; failure invalidates this generation, closes outside the lock and restores the existing network retry delay. Outer transport state publishes only after successful initialization; failure returns the existing UI false path. | Forced constructor/registration/open failures, synchronous close/rejection, stale-generation teardown, retry and rendered recovery. Source contracts are not runtime fault injection. |

The native optimized and ASan/UBSan game and actual shared
stage/transaction/cleanup-worker fixtures compile. Strict C++17 syntax,
Python AST parsing and patch dry runs pass;
no new assertions or applications executed. The transaction fixture uses real
reserved workers, futures and the production state helper, with subsystem
counters, to cover partial starts, blocked rollback, exception preservation,
fresh admission and failed retirement at every owned stage. It also rejects
deferred completion without executing it on the admission thread, duplicate
initialization, omitted stage retirement and implicit retry after failure.
These assertions remain unexecuted; they do not substitute for actual RTC
subsystem injection. The separate existing C/queued-work limits below remain open.
The fixture distinguishes sockets-only rollback and cleanup of an acquired pool
with no started workers. Its pending-admission assertion runs on the owner thread;
it does not prove nonblocking concurrent public APIs. Actual token/preload/Cleanup
calls first acquire the initialization mutex, which retirement holds through joins
and subsystem cleanup, so those callers can still wait for that lock.

`verify_datachannel_startup.cmake` checks exact final `init.cpp`/`init.hpp` bytes
and the independent startup-stage and bounded work-admission patches, including source-directory overrides.
The application recipe recognizes the final transaction before considering the
older overlapping cleanup patch; it never reverses an old patch into newer code.
FetchContent fixes the dependency checkout to LF (`core.autocrlf=false`,
`core.eol=lf`) so fresh Windows clones cannot silently rewrite byte-pinned source.
Existing overrides still need reviewed LF bytes; Windows-native qualification
is not established by macOS compilation.

### RTC-CALLBACK-01 — in-flight publication and delayed decisions

The connection transaction rejects callbacks entering after generation
invalidation and now rechecks ownership after `socket->open()`. A separate
source review found that `socketMessage()` checks generation/shutdown before
parsing, while `handleBootstrap()`, `parseRoom()` and `handleRoomState()` later
commit under separate locks without repeating the connection-generation check.
An already-entered callback could therefore outlive invalidation and publish stale
room state. The new source candidate carries socket generations to actual state,
cache, roster and event commits, and invalidates the socket generation at close.
Direct-peer commits separately require the current peer identity; completed
healthy controllers deliberately survive signaling reconnects.

Peer creation now reserves an identified controller-lifecycle setup attempt before
fallible Peer/config allocation, then publishes its placeholder before RTC setup,
rechecks admission before publishing its resources, and removes/closes refused
candidates outside the state lock. An unfinished placeholder from a retired
socket cannot suppress its replacement when a new roster/hello arrives. The
related delayed-decision review also found:

- A forced retry could replace a newer peer with the same controller ID. Even
  the same peer could authenticate or resend its offer before replacement.
  Admission now checks exact peer identity, current eligibility and unchanged
  offer timestamp/attempt count together under the state lock.
- A queued ping-expiry verdict could disconnect a peer whose pong already
  arrived. Disconnect commit now checks the same outstanding timestamp/nonce.
- A give-up event could be published after a later successful ready event.
  The give-up verdict and its queue publication now share the original lock.

The production predicates are shared with `party_callback_identity`, whose
deterministic capture/change/commit model covers retirement, replacement,
unfinished setup, completed-peer survival and revoked retry/liveness decisions.
`party_open_transaction` adds source bindings to the actual commit sites.
These fixtures are implemented but unexecuted and do not drive real RTC callbacks.
Already-admitted I/O may finish on a captured owner after retirement; this change
rejects stale state/event publication, not synchronous OS/RTC cancellation or
every callback allocation failure. Real interleaving/recovery qualification
remains required.
The existing event queue can coalesce/drop events under saturation, and an
allocation failure can prevent publication after a state update. The identity
checks do not establish guaranteed event delivery. Peer setup refusal now has
an autonomous bounded recovery candidate in `party_peer_setup_retry.h`: three
setup failures per admitted controller lifecycle, with 300ms/600ms backoff before
the first two retries. Reservation precedes fallible allocation; generation and
transport-wide revision reject stale completions. Duplicate hello, ordinary
roster refresh and signaling reconnect do not replenish setup failure budget.
An offer emitted before later setup failure counts toward the three unanswered
offers; a previously authenticated peer can begin a new unanswered-offer episode
without resetting its lifecycle setup-failure counter. Exhaustion reporting stays
pending if event allocation fails. `party_peer_setup_retry` and source bindings
are implemented and optimized compilation passes, but real allocation refusal,
interleaving, timer retry and rendered exhaustion remain unexecuted.
The integrated native game and callback-identity fixture compile in optimized
and ASan/UBSan profiles. Independent source review found no additional defect
in these final ownership/decision changes; that scoped result is not a runtime
concurrency or regression verdict.

### RTC-SIGNAL-01 — native signaling worker-start refusal poisoned Connecting

The related first-party source sweep found `MdkrMatchSignalClient::connect()`
publishing `Connecting` before an unguarded `std::thread` construction. A
construction exception escaped the false-return contract used by
`ReconnectingSignalFeed::start()`/`replaceClient()`; the retained client would
also report success on its next call despite having no socket worker.

The candidate catches worker-start refusal, restores `Idle` before reporting
the existing `signal_transport_lost` refusal, and leaves diagnostics best effort
if their allocation fails. It does not enqueue a terminal event for an attempt
that never started. A one-shot C++ test seam enters the production failure
boundary before launch; the existing signal-client fixture asserts repeated
refusal, empty event state, successful same-client loopback retry, welcome and
idempotent close. The new assertions remain unexecuted. Subsequent factory,
worker-reporting and outbound-admission candidates are described below; these
do not establish allocation safety for all their callers or dependencies. The
separate [Windows ownership audit](windows-network-lifetime-audit.md) now traces
first-party socket-start references, failure handling and cold LAN enumeration;
those findings now have separate checked shared-lease candidates, not an implied
fix from worker-admission rollback. Windows acceptance remains open.
The integrated game and signal-client fixture compile in optimized and
ASan/UBSan profiles. The injected refusal exercises the production catch, not
an actual OS thread refusal or diagnostic-allocation failure; same-client retry
also does not qualify the reconnect wrapper's complete replacement ladder.

The signal close path additionally contained an optional-event allocation gap:
publishing Closed before its rejection event could strand the still-owned
joinable worker if reporting threw. Mandatory stopping and worker transfer now
precede best-effort event construction; join and network-lease release follow
even on injected reporting refusal. The added actual-client close-event fixture
has compiled in the optimized checkpoint but has not executed. This is not
coverage of every running-worker or outbound-admission allocation failure.

### RTC-SIGNAL-02 — factory, outbound admission and terminal-report recovery

The later first-party review confirmed that outbound queue insertion preceded
allocating correlation-record insertion. Refusal could leave an untracked frame
queued without consuming its sequence. The candidate prepares serialization,
inserts tracking, then inserts outbound work under the same mutex; refusal rolls
tracking back and does not consume a sequence. Factory refusal now returns null
with a best-effort ordinary code, and abandoned state construction wipes its
owned credential before releasing its socket-library lease.

The socket worker now contains exceptions after its local transport and locks
unwind. If terminal-event construction itself fails, a static failure-code
pointer retains the verdict without allocating. Draining prepares diagnostic
storage and vector capacity before consuming queued events; refused preparation
leaves the queue and pending terminal code available for a later drain. The
optional close-report fallback retains its terminal failure too.

`platform/online/scoped_string_wipe.h` now guards the worker's local credential
offer, HTTP request and comparison offer before secret bytes are copied. Final
capacity is reserved first, and the shared RAII helper invokes
`mbedtls_platform_zeroize` on ordinary return or stack unwind; explicit early
erasure is idempotent. The actual signal fixture includes a synthetic-buffer
early-unwind/explicit-wipe assertion using this helper. This is not a claim to
erase already-released string-growth copies or all process memory.

Actual-client fixtures cover three send-admission refusal stages and retry/
correlation, factory refusal/retry, worker publication plus terminal-report and
drain refusal, and close notification refusal. Six `match_signal_admission_source`
methods bind the production admission/reporting and local secret-buffer guards.
Final integrated optimized, ASan/UBSan and Windows cross-builds compile/link the
game plus five affected fixtures with the latest signal guards, queue/terminal
refusal coverage and LAN timeout/capture-retirement follow-ups. No new assertions
executed. Independent bounded source review found no additional introduced issue;
this is not runtime acceptance. The Windows profile enables native beta and
retains its historical nonempty cloud origin, so it is not the intended final
partyless artifact. Queue/backpressure policy, caller-wide allocation handling,
mutex/OS failure and vendor RTC-ALLOC-01a/c/d/e are not cleared by these changes.

The independent [work-admission amendment](native-rtc-work-admission-audit.md)
now addresses RTC-ALLOC-01b callable extraction and -01f insertion accounting.
RTC-ALLOC-01a, -01c, -01d and -01e remain unresolved; the initialization and
first-party recovery candidates cannot intercept dependency `noexcept` termination.

The same sweep found different existing commit discipline in native match code:
`match_signal_client.cpp` repeats its allowed-phase check after parsing while
holding its state mutex, and each client owns only one socket-worker lifetime.
`match_peer_transport.cpp` serializes state changes on its launcher pump and
checks each queued event's peer attempt before applying it; teardown increments
attempts and the closed pump returns immediately. These scoped source checks do
not establish complete allocation safety, queue-owner destruction safety or
runtime interleaving qualification for either subsystem.

## Confirmed findings

### RTC-INIT-01 — failed initialization poisons subsequent admission (P1)

`Init::doInit()` sets `mInitialized = true` before acquiring any subsystem
(`src/impl/init.cpp:111`). A later exception does not restore it. Construction
of `TokenPayload` fails before publishing its reserved completion future or
arming cleanup (`src/impl/init.cpp:45`). No global token is committed, and the
unarmed reserved helper cancels without invoking RTC cleanup.

Consequences confirmed by the source paths:

- A later token request can enter `doInit()`, see the stale true value, and
  return without initializing the missing subsystems.
- `Init::cleanup()` can return the previous already-ready `mCleanupFuture`
  despite leaked partial initialization (`src/impl/init.cpp:62`, `:88`).
- Native peer creation catches construction failure and has retry behavior
  (`platform/online/match_peer_transport.cpp:680`, `:690`, `:944`). Thus poisoned
  retry is a real production pathway, not just a hypothetical direct API call.

Required fix: commit ready state only after successful initialization; preserve
the exact ownership of partial work; publish and observe its retirement; refuse
new admission while rollback is pending or has failed.

### RTC-INIT-02 — PollService cannot safely roll back failed thread start (P1)

`src/impl/pollservice.cpp:33` installs socket-map/interrupter resources, marks
`mStopped = false`, then constructs the polling thread. Thread construction can
throw with no joinable thread. `join()` later calls `mThread.join()` without a
joinability check (`:40`), before releasing the map/interrupter. Calling today's
generic cleanup after that failed start is therefore not a valid rollback.

Required fix: make `start()` strongly exception-safe. Stage owned resources
before publication; if thread construction fails, restore stopped state and
release the staged resources without joining a nonexistent thread. Preserve
the single successful start/stop lifecycle. Do not treat merely setting a
boolean as proof that the worker exists.

### RTC-INIT-03 — SCTP allocation occurs after starting the C subsystem (P1)

`SctpTransport::Init()` invokes `usrsctp_init()` before allocating
`InstancesSet` (`src/impl/sctptransport.cpp:87`, `:98`). Allocation failure leaves
the C subsystem started. `SetSettings()` also follows initialization and uses
checked integer conversions that can throw (`:101`; `src/impl/utils.hpp:72`).

Required fix: allocate the C++ instance registry before starting usrsctp, and
track SCTP ownership before any subsequent settings conversion can throw.
Release SCTP only when this transaction acquired it; do not confuse failure of
the earlier C++ allocation with a started C subsystem.

### RTC-INIT-04 — Phone Party constructor failure escapes its open-error path (P2)

`TransportState::connect()` constructs `rtc::WebSocket` outside the try/catch
that protects `socket->open()` (`platform/party/libdatachannel_party_transport.cpp:460`,
`:490`). `LibDatachannelPartyTransport::open()` and
`MdkrNativePartyHost::open()` do not catch that construction failure; the UI
invokes the latter directly (`platform/app/ui_phone_party.cpp:575`). The ordinary
false-return recovery message therefore does not contain this exception path.

Required fix: once dependency rollback is truthful, contain construction and
callback-registration failures at the transport boundary and restore its
unopened state. Preserve local-controller operation and provide the existing
actionable failure presentation. An outer catch alone does not repair
RTC-INIT-01 or establish that global resources were released.

### RTC-ALLOC-01 — queued-work allocation can terminate across `noexcept` (separate follow-up)

The pinned `ThreadPool::enqueue()` and both `schedule()` overloads are declared
`noexcept` (`src/impl/threadpool.hpp:85`, `:90`, `:96`). The final overload
constructs a bound callable, allocates a shared packaged task/future, and pushes
the task into its queue (`:100`–`:111`). Failure of those allocations escapes a
`noexcept` boundary and terminates the process; the try/catch inside the packaged
callable only handles later job execution, not its own allocation or admission.

The expanded [work-admission audit](native-rtc-work-admission-audit.md) traces
six related classes through actual queue execution, Processor continuation,
transport teardown, receive counters and queue accounting. It records why
isolated signature changes or dropping rejected work cannot clear this finding.

This path is used by certificate generation (`src/impl/certificate.cpp:598`)
and the processor that schedules transport teardown (`src/impl/processor.cpp:22`;
`src/impl/websocket.cpp:512`; `src/impl/peerconnection.cpp:408`). It is a confirmed
exception-safety limitation, not an executed failure result. Strong startup
`spawn()` handling and transactional `Init` rollback do not fix it.

Required separate review: define queue-admission failure ownership and caller
recovery for ordinary work and teardown work. Merely deleting `noexcept` would
move the exception into callers that may also be destructing owners. Any future
correction must preserve queued-task/token ownership, avoid inline cleanup under
RTC locks or on a worker cleanup must join, and demonstrate failure handling
through the actual production admission path. No scheduling change is included
in the current startup amendment; do not claim all RTC allocation-failure classes
are handled.

### RTC-API-01 — live SCTP setting updates are not transactional (lower priority)

`Init::setSctpSettings()` applies settings directly when `mGlobal` is present,
then stores the requested settings (`src/impl/init.cpp:100`–`:105`).
`SctpTransport::SetSettings()` interleaves individual sysctl writes and checked
conversions (`src/impl/sctptransport.cpp:101`–`:147`). A later conversion failure
can therefore leave earlier live values changed while the stored next-init
configuration remains the old value. A startup rollback covers initialization's
use of settings, not this separate live-update API.

Source search found no first-party `SetSctpSettings` call in the game/platform
code. This is a lower-priority library API follow-up, not evidence of a currently
reachable launcher setting defect. If that API is adopted, validate/convert the
entire requested configuration before applying it and define partial C-setter
failure semantics; keep that scope separate from the startup transaction.

## Per-subsystem ownership and failure matrix

| Shipped stage | Actual contract / partial state | Retirement requirement |
| --- | --- | --- |
| libdatachannel Windows `WSAStartup` | `src/impl/init.cpp:118` throws on nonzero return. Ownership begins only on success. POSIX does not enter this stage. | Balance only the successful library-owned startup after all dependent sockets/workers retire. |
| `ThreadPool::spawn` | `src/impl/threadpool.cpp:28` creates workers one by one; a later allocation or launch failure leaves earlier joinable workers. Its worker-vector lock unwinds on exception. | Track an attempted spawn after obtaining the singleton. Join every started worker and clear queued work; do not infer zero workers from the thrown call. Do not allocate a previously unconstructed singleton just to roll it back. |
| `PollService::start` | Map allocation, interrupter construction, and thread creation can fail at distinct points. Current stopped/thread state is inconsistent after launch refusal. | First fix RTC-INIT-02. On success, join and destroy interrupter/map before Windows socket-library cleanup. |
| `PollInterrupter` | `src/impl/pollinterrupter.cpp:21` owns a pipe on POSIX or a UDP socket/address lookup on Windows. The Windows constructor catches its later setup failures and releases addresses/socket; pipe creation failure precedes fd ownership. | Its owning object must be destroyed on all failed PollService starts as well as ordinary shutdown. |
| usrsctp + C++ instance registry | C startup is followed by a throwing C++ allocation today. `usrsctp_init` returns void; `SetSettings` can throw after startup. | Move C++ allocation before C startup. Track returned C startup separately from subsequent settings success; use `SctpTransport::Cleanup()` only for acquired state. It may wait for SCTP shutdown; no bounded-duration guarantee. |
| Mbed TLS DTLS globals | `src/impl/dtlstransport.cpp:447` / `:451` are no-ops for this build. | No global ownership to roll back. Per-connection Mbed TLS objects have separate lifetimes. |
| Mbed TLS WebSocket TLS globals | `src/impl/tlstransport.cpp:309` / `:313` are no-ops. | No global ownership to roll back. |
| libjuice ICE globals | `src/impl/icetransport.cpp:42` / `:46` are no-ops. | Do not invent a global libjuice resource here; individual ICE transports retain their existing teardown obligations. |

The C API boundary limits the guarantee: usrsctp's own Windows startup calls
`WSAStartup` and invokes `exit(-1)` on failure
(`deps/usrsctp/usrsctplib/user_socket.c:81`). It balances its own startup in
`usrsctp_finish()` (`:2047`). A C++ catch cannot intercept that process exit.
This audit does not claim transactional handling of every internal usrsctp
allocation, platform synchronization failure, or process-terminating C failure.
That would require a separately reviewed usrsctp amendment, not an invented
success result from its void initialization API.

## Lock and callback constraints

- `Init::token()` and `preload()` hold `mMutex` while constructing the token
  (`src/impl/init.cpp:70`, `:80`). Calling `doCleanup()` inline from their failed
  constructor would reacquire that same mutex (`:151`) and deadlock.
- The final token may also be released by an RTC teardown task. Both WebSocket
  and PeerConnection teardown enqueue a captured initialization token
  (`src/impl/websocket.cpp:512`, `src/impl/peerconnection.cpp:408`), and the
  processor schedules those tasks on the RTC pool (`src/impl/processor.cpp:22`).
  Inline global cleanup can consequently join its own pool.
- Do not wait for the reserved rollback worker while retaining `Init::mMutex`:
  the worker must acquire that mutex before touching global state.
- Individual stage cleanup is not automatically equivalent to calling the
  recursive `doCleanup()`. A synchronous startup-only rollback would need a
  separate proof that no owner or callback task was admitted and that every
  joined thread is independent of the held mutex. Do not apply that assumption
  to the ordinary final-token path.
- Preserve live-epoch reuse before rejecting new initialization. Teardown paths
  obtain an initialization token while their existing owner is still alive;
  `mWeak.lock()` must succeed for that same epoch even after `cleanup()` has
  reset `mGlobal`. Rejecting all pending futures before checking the live weak
  owner would obstruct valid teardown.
- `preload()` must also reuse a live weak token instead of constructing another
  payload when `mGlobal` is absent. The reviewed code only checks `mGlobal`, so a
  preload while an external token survives can publish a second cleanup future
  for already initialized resources. A new epoch is admissible only after the
  old weak owner expired and its exact completion future is ready and successful.
  Repeated `cleanup()` calls must preserve the same pending or failed future.

## Required ordering (implemented candidate; qualification pending)

1. Give `PollService::start()` a strong failure contract and front-load the
   SCTP C++ registry allocation. Add fault seams for these exact production
   operations before relying on them in an initialization transaction.
2. Replace the eager ready flag with explicit initialization/rollback state and
   per-subsystem ownership. Ready is committed only after every shipped stage
   succeeds. Mark pool ownership before its possibly partial spawn, PollService
   ownership after its now-atomic start, Windows ownership only after success,
   and SCTP ownership before settings application.
3. Publish the pre-reserved completion future before starting RTC acquisition.
   On initialization failure, preserve the initiating exception and schedule
   selective rollback through the already-reserved cleanup worker. The failing
   token constructor must arm that rollback job before unwinding; it must not
   wait under `mMutex`. `doCleanup()` must consume partial ownership even when
   full ready state was never committed.
4. Block new `token()`/`preload()` admission while rollback is pending. Permit a
   fresh initialization only after rollback actually succeeded. A rollback
   failure remains observable and must not silently restore ready/retry state.
   Ensure `rtc::Cleanup()` returns this transaction's pending/failed future,
   never the earlier already-ready future.
5. Retire workers before their supporting SCTP/socket resources, preserving the
   existing dependency ordering. Keep exact ownership if cleanup itself fails;
   do not clear all stage flags and claim success after an incomplete rollback.
6. Contain Phone Party's constructor/registration exceptions at its transport
   boundary; then verify normal native peer retry against the recovered global
   state. Keep the amendment scoped to the reviewed shipped graph or explicitly
   review additional TLS/ICE/media variants before claiming their coverage.

## Required regression evidence

Use the actual production transaction/worker helper and patched stage-start
implementation, with injectable operations. A second mock that merely throws
from an outer cleanup callback is insufficient.

- Failure before Windows acquisition, after Windows acquisition, on the first
  and a later pool worker, at PollService map/interrupter/thread creation, at
  SCTP registry allocation, and during settings application.
- Exact acquired-resource counts, one-time release, no join of a nonexistent
  polling thread, and no lost joinable partial-pool handle.
- Rollback that blocks on an explicit fixture latch: concurrent retry must not
  acquire new state, and cleanup future must remain pending until release.
- First initialization failure followed by successful rollback and a real
  second initialization; all required stages must run again.
- Rollback exception propagation: cleanup remains a failure and relaunch is
  refused. Do not reuse the previous successful completion future.
- Constructor unwinding while `Init::mMutex` is held; no wait or recursive
  cleanup under that lock. Test RTC-worker final-owner destruction separately.
- Phone transport constructor refusal reaches a usable error state rather than
  escaping the UI call; native peer retry does not consume poisoned globals.
- Subsequent optimized/sanitizer behavioral runs on the actual native dependency
  and Windows socket graph. Compilation and source bindings alone are not
  behavioral acceptance.

The initial audit was read-only. The subsequent source amendments above change
the dependency integration and first-party transport boundary; no executable
tests were performed for this checkpoint.
