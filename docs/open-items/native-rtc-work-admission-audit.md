# Native RTC work-admission failure audit

Status: **RTC-ALLOC-01 remains open.** Source inspection confirms fallible work
admission inside `noexcept`, additional allocation during execution, and
ownership-sensitive continuations and teardown. RTC-ALLOC-01b and -01f now have
bounded implemented dependency candidates. The accepted-continuation portion of
-01c now also has an additive prepared-dispatch candidate; generic admission,
SCTP refusal ownership and terminal retirement are not cleared. A dependent
per-resource retirement candidate now covers reserved dispatch and exact lower
edge ownership; its stop/destructor bodies and behavioral qualification remain
open. -01a, the
remaining -01c prerequisites, -01d and -01e remain open.
No allocation-failure fixture or application was executed for this checkpoint.

## Scope and evidence identity

Reviewed on 2026-09-06 against libdatachannel **0.24.5**, commit
`443f6934d9007eb7076ab7825ba330f355fcbead`, pinned and checked by
`cmake/datachannel.cmake:13`. The shipped graph enables WebSocket and Mbed TLS,
disables GnuTLS/libnice, and disables media. ICE uses the pinned libjuice
submodule, commit `3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6`.

References beginning `src/`, `include/`, or `deps/` are relative to that
libdatachannel source tree. Other references are project-relative. Dependency
line numbers describe the inspected queue/transport files, not generated patches.
The local initialization and cleanup-worker amendments do not fix these work
admission paths. The newer independent work-admission amendment below addresses
callable extraction and insertion accounting. The dependent prepared-dispatch
amendment now addresses accepted Processor continuation, not all admission.
Startup evidence is recorded in
[the initialization audit](native-rtc-initialization-audit.md).

This is a defensive, source-backed failure/lifetime review. It contains no
private runtime artifacts and makes no claim of a reproduced resource-exhaustion
incident, executed fault injection, or complete native multiplayer qualification.

## Confirmed findings

### RTC-ALLOC-01a — allocating submission is declared noexcept (P1)

`src/impl/threadpool.hpp:52`, `:55`, and `:59` declare `enqueue` and both
`schedule` overloads `noexcept`; their definitions retain it. The time-point
implementation at `:96` binds arguments, allocates a packaged task and its future
state, creates a callable, and inserts into the allocating priority queue
(`:100`–`:111`). An allocation exception cannot reach the caller's ordinary
recovery boundary: the `noexcept` boundary terminates the process.

These are normal production calls, not an unused API: certificate creation,
TCP connect/retry, TLS/DTLS receive, handshake timers, and transport teardown all
use this scheduler. Removing the declarations alone is insufficient for the
following reasons.

### RTC-ALLOC-01b — dequeue copies an already-admitted callable (P1; candidate implemented)

The pinned baseline `src/impl/threadpool.cpp:80` used
`auto func = std::move(mTasks.top().func)`. `priority_queue::top()` returns a
const reference, so this copies the `std::function` instead of moving it. Such a
copy can allocate; whether a particular callable fits an implementation's small
buffer does not establish a portable allocation-free contract. `runOne()` and
`run()` have no exception boundary around dequeue (`:57`–`:70`), so a dequeue
exception escapes the native worker entry.

`cmake/patches/libdatachannel-work-admission.patch` now uses the shared
`MdkrScheduledWork` in `platform/online/queued_work_admission.h`. Its mutable
callable swaps into an empty result without copying; the unchanged time key
preserves heap ordering, and the popped node no longer owns the captured work.
Execution/destruction of the returned callable remains outside the scheduler
lock. This removes that extra allocation, not allocating submission or Processor
continuation. The production-helper fixture and actual dependency integration
compile; allocation-refusal assertions and runtime extraction remain unexecuted.

### RTC-ALLOC-01c — Processor continuation can terminate or strand join (P1; partial candidate)

The pinned baseline `src/impl/processor.hpp:58` independently declares allocating
admission `noexcept`. Its task creates `scope_guard` from `std::bind` at `:62`; the guard
stores a `std::function` (`include/rtc/utils.hpp:36`). If this conversion's
allocation fails before the guard exists, the packaged task records the
exception in a future the Processor discarded. No continuation is installed,
`mPending` remains true, and `Processor::join()` waits for a condition no task
will settle (`src/impl/processor.cpp:17`). Peer destruction calls that join
(`src/impl/peerconnection.cpp:82`).

In that baseline, when guard creation succeeds, its implicit-noexcept destructor calls
`Processor::schedule()`. That function pops the next accepted task before
allocating its pool submission (`src/impl/processor.cpp:24`). A newly catchable
submission failure would still escape the guard destructor. Catching and
discarding it instead would lose accepted work and can retain pending state.

Required correction: allocation-free continuation cleanup and dispatch of
already-accepted work, transactional initial admission, and preserved exactly-once
FIFO processing. Pending/notification state must be settled even when a task body
throws. Do not replace this with a naive drain loop: destroying a callable that
holds the last peer owner while its Processor remains pending can make that
owner's destructor wait on the currently executing worker.

SCTP adds a distinct admission constraint. `SctpTransport::enqueueRecv()` and
`enqueueFlush()` (`src/impl/sctptransport.cpp:555`–`:574`) retain a strong owner
from the usrsctp upcall, increment a pending counter, then move that owner into
the Processor. Both explicitly require that the retained owner not be released
on the upcall. Its destructor also calls `mProcessor.join()` (`:321`–`:324`).
A prepared-task API that simply throws before admission would unwind that
retained owner on the callback; counter rollback alone is not a lifetime fix.
Preparation/admission must be paired with already-reserved safe ownership
retirement, including rejection before task construction and accepted-task
completion. The continuation itself must be nonallocating after admission.

The new `cmake/patches/libdatachannel-prepared-work.patch` and shared
`platform/online/prepared_work_queue.h` implement only the accepted-work portion:
a concrete callable is prepared in an owned node before publication, then an
intrusive FIFO transfers accepted nodes without another bind, function conversion,
task/future allocation or queue growth. A typed continuation advances FIFO after
standard or non-standard task exceptions. Final pending state is settled before
releasing the callable's last retained owner, outside both scheduler and Processor
locks; no Processor access follows that release. Bounded admission waits release
the mutex needed by completion to make space. Task failures retain a nonallocating
counter rather than requiring logging before progress.

The pool merges immediate publication timestamps with timed priorities, includes
the accepted lane in join's quiescence predicate, uses typed busy-worker guards,
and releases cancelled timed captures outside its lock. It never cancels accepted
Processor nodes. Publication requires a live RTC epoch: every inspected peer/SCTP
Processor call retains its strong owner, while the teardown Processor is immortal.
Transport, peer, WebSocket and certificate owners retain initialization tokens;
remaining delayed transport captures are weak. The plain LogCounter capture has
no transport-destruction callback. Under these actual callers, Init's subsequent
zero-worker timed clear cannot first release a live transport and publish fresh
prepared work. Arbitrary ownerless or post-join publishers are outside this
contract; the active-worker clear fixture does not establish their safety.

Generic pool and Processor allocating `noexcept` admission intentionally remains
unchanged pending SCTP/terminal reservation work. This partial candidate is not
refusal recovery for preparation itself, allocation-free user task bodies,
mutex/OS failure recovery or RTC-ALLOC-01 closure. Final integrated optimized,
ASan/UBSan and Windows cross-builds compile/link the game and affected fixtures;
both new fixtures' assertions remain unexecuted at this checkpoint.
`datachannel_prepared_work` tests the actual shared helper;
`datachannel_prepared_dispatch` links the actual patched ThreadPool/Processor.
The latter covers timer ordering in both directions, standard/non-standard task
exceptions, bounded-admission wakeup, final-owner join, active-worker timed clear
and repeated idle-pool join. These are authored assertions, not observed passes.
The refusal seam intercepts ordinary throwing `new`/`new[]`, not `malloc`,
over-aligned allocation or exception-runtime allocation. Explicitly throwing
`bad_alloc` tests exception continuation, not a refused task-body allocation.
The bounded-admission case's 100 ms no-return window is a black-box observation,
not deterministic proof that the submitter entered its condition wait.

### RTC-ALLOC-01d — teardown submission owns thread-sensitive resources (P1; partial candidate)

In the inspected baseline, `PeerConnection::closeTransports()` (`src/impl/peerconnection.cpp:382`) and
`WebSocket::closeTransports()` (`src/impl/websocket.cpp:488`) exchange transport
pointers out of the live object, disconnect callbacks, then capture those owners
in an allocating `TearDownProcessor::enqueue` call (`:407` and `:511`). The code
explicitly defers destruction to permit termination from a transport's own
thread. The teardown closure also retains an initialization token.

After merely removing `noexcept`, failed submission would destroy the captured
transport owners while unwinding the originating callback thread. ICE destruction
resets its libjuice agent (`src/impl/icetransport.cpp:183`); agent destruction
joins a resolver when present and destroys its connection implementation
(`deps/libjuice/src/agent.c:160`). The selected ICE configurations use POLL/MUX
(`src/impl/icetransport.cpp:96`), so this audit does **not** claim the separate
libjuice thread-mode self-join occurs on every shipped connection. The loss of
the expressly required off-thread ownership boundary is nevertheless concrete.

The public peer and WebSocket destructors catch `std::exception`
(`src/peerconnection.cpp:40`, `src/websocket.cpp:29`). Those catches neither
intercept current `noexcept` termination nor restore already-transferred owners.

Required correction: reserve a terminal teardown record and dispatch capacity
before admitting thread-sensitive transport resources. Closing must fill and
publish this record without allocation, retain the epoch token until resource
retirement, and retire exactly once on an appropriate worker. Inline callback
cleanup, emergency detached threads, and silently dropped owners are not safe
fallbacks. Reopening a WebSocket needs a fresh per-cycle reservation before new
transport admission; an old cycle's retirement must not consume the new record.

One reservation used only by `closeTransports()` is insufficient. The peer
factory helper publishes its transport before `start()`, clears the member on
start failure, and clears/stops it if closing wins the race
(`src/impl/peerconnection.cpp:137`–`:153`). The local strong owner can therefore
become the last owner outside `closeTransports()`. WebSocket's corresponding
helper has the same publication and close-race paths and also stops on start
failure (`src/impl/websocket.cpp:203`–`:223`). Any implementation must cover
factory-local ownership, callback setup, partial start failure, concurrent close,
and rejected publication, not only owners already installed in a live member.

WebSocket coverage must include outbound `open()` and each reopen cycle,
`setTcpTransport()`, proxy/TLS/WS factories, and accepted sockets passed directly
to `setTcpTransport()` by `WebSocketServer::runLoop()`
(`src/impl/websocketserver.cpp:73`–`:87`), which bypass outbound `open()`.
The inspected baseline close array exchanges WS/TLS/TCP, but not `mProxyTransport`
(`src/impl/websocket.cpp:487`–`:521`); proxy ownership must be included in the
retirement inventory rather than assumed to disappear with the lower chain.
This inventory is not a claim that the first-party shipping configuration
exercises every proxy or accepted-WebSocket path. Reserve the off-thread ownership mechanism before
admitting any thread-sensitive resource, including the SCTP upcall owners above,
and define allocation-free handoff for every terminal or refused-admission path.

#### Per-resource reservation and lower-edge correction candidate

The additive `libdatachannel-transport-retirement.patch` and production
`platform/online/transport_retirement.h` now reserve separate stop and deletion
records before constructing each admitted transport. A standard shared-pointer
deleter publishes serial deletion even if control-block allocation is refused;
the epoch lives through deletion, not in a surviving weak control block. Complete
object destruction occurs inside the teardown task body before its continuation.
Serialization leaves another RTC worker available for a destructor's Processor
join. An accepted socket transfers ownership after successful object construction,
before control-block creation, preventing a second close during later unwind.
Selected ICE/SCTP resource initialization follows shared ownership; partial SCTP
socket/address/registry ownership is explicit. Optional libnice is not qualified.

Publication uses exact-owner compare/exchange. A per-record start gate prevents
an early stop overtaking an admitted start or a new start following terminal stop.
Peer and WebSocket close paths use the reserved records; the WebSocket inventory
includes proxy ownership. Foreign internal control blocks retain the old grouped
asynchronous fallback, without claiming allocation-free admission for that path.

Independent review found and corrected a necessary adjacent ownership defect:
an unstarted publication loser must not clear or stop the winner's shared lower
transport. Receive registration, removal and chain-stop authority are now bound
to the exact upper owner. A replacement admitted before retirement revokes old
authority; a successful retirement claim permanently seals the edge before
queued stop, so later registration cannot race into that stop.

The same review found callback/pending lock inversion and destruction of retired
callback captures under the pending lock. All pending/edge operations now acquire
the existing recursive callback lock first, release the pending lock before
invocation, and move retired captures/packets out for destruction after pending
unlock. This preserves callback draining; it does not certify arbitrary
self-replacing user callbacks, cross-transport callback cycles or all WebSocket
epoch callbacks. Existing per-message pending replay semantics are unchanged.

Two actual-vendor fixtures are added: ten helper/dispatcher groups and eleven
Transport-edge groups. Strict C++17 syntax and optimized, ASan/UBSan and Windows
cross-builds of the game, both fixtures, prepared dispatcher and Workshop model
pass. Executable assertions remain unexecuted. Windows retains its historical
cloud origin, not final partyless artifact provenance; BasisU GCC warnings remain.
The edge fixture covers reserved and foreign cascades, constructor
unwind, stale/losing owners, seal-before-dispatch, and callback/capture reentry.
Its concurrent removal case is interleaving stress, not a deterministic old-code
deadlock control. Neither fixture drives actual peer/WebSocket publication races,
ICE/SCTP partial initialization, or real network sessions. Full RTC-ALLOC closure
still requires allocating admission, receive/SCTP counter rollback, stop/destructor
body corrections, fault controls and final-platform behavioral qualification.

### RTC-ALLOC-01e — receive admission and destructors need separate contracts (P1)

DTLS increments `mPendingRecvCount` before enqueueing
(`src/impl/dtlstransport.cpp:31`); TLS does likewise after retaining itself
(`src/impl/tlstransport.cpp:25`). Propagating or swallowing refusal without
repairing that state can suppress subsequent receives.

The shipped MbedTLS DTLS destructor (`src/impl/dtlstransport.cpp:437`) calls
`stop()` → `enqueueRecv()` (`:470`), which attempts submission even when its weak
owner cannot be retained. The TLS destructor also calls `stop()` (`:367`), but
its `enqueueRecv()` checks a strong lock first; these are not identical paths.
Additionally, the base transport destructor stops a still-owned lower transport
(`src/impl/transport.cpp:16`), which can itself schedule work. Changing the pool
signature therefore does not remove allocation from implicit-noexcept teardown.

Required correction: distinguish normal receive admission from terminal stop,
undo uncommitted receive counters, and make destructor/mandatory stop behavior
allocation-safe. Failure must drive a coherent terminal transport state through
the safe teardown mechanism, not recursively enqueue the same failing work.

#### Selected DTLS receive accounting and terminal-wake follow-up (open)

The next read-only prerequisite review identified two concrete gaps in the
selected MbedTLS path, unchanged by the retirement amendment:

- `DtlsTransport::doRecv()` decrements `mPendingRecvCount`, but its handshake
  timer invokes `doRecv()` directly without reserving an increment. A timer
  consumes a count it did not acquire, so the counter can become negative and
  cease to represent queued work. Counter rollback on submission failure alone
  would not correct this; ordinary work and timer wakes need one explicit
  accounting/admission contract.
- `DtlsTransport::incoming(nullptr)` stops the input queue and returns without
  scheduling terminal processing. Unlike TLS's corresponding path, it provides
  no wake when the connected transport is idle with no other pending work.
  Closure propagation must not depend on a later packet or unrelated timer.

These are source-confirmed accounting/missing-wake findings, not executed
reproductions. Fix them with receive admission and mandatory terminal dispatch,
including owner-expired destructor behavior, before claiming -01e resolved.
Do not merely add another allocating enqueue inside an implicit-noexcept stop
path or relax queue correctness to make the new fixtures pass.

Next implementation sequence (reviewed proposal, not implemented):

1. Reserve distinct terminal completion work before DTLS/TLS/SCTP publication.
   Stop, null input and admission refusal must settle an idempotent terminal
   state without calling `doRecv` inline or recursively allocating another wake.
   Separate quiescent destructor cleanup from ordinary receive scheduling.
2. Make immediate receive/flush preparation fallible before nonallocating
   publication. Pair each accepted wake with its counter ownership, roll back
   refusal, and use reserved terminal work so coalesced input cannot be stranded.
   Timers must use the same accounting contract rather than bare `doRecv`.
3. Cover all SCTP receive, flush, `close` and `closeStream` admissions. Mandatory
   state/counter/waiter updates precede optional reporting. Refused upcall work
   must release through reserved deletion, never drain it synchronously while
   the upcall still holds the registry's shared lock.
4. Qualify pre-publication refusal/retry, timer-only and overlapping wakes, idle
   null-input closure, concurrent/repeated stop, last-owner upcall refusal,
   connecting-versus-connected terminal states, SCTP waiter notification, and
   destructor cleanup under ordinary allocation refusal. Only then widen the
   generic pool/Processor exception contracts with the remaining caller audit.

The selected DTLS/TLS destructors do not themselves join workers. SCTP does,
but the inspected Processor admissions retain strong owners and the prepared
continuation settles before releasing them. No remaining Processor self-join
was demonstrated in that updated graph; preserve this ownership ordering rather
than describing all receive/destructor risks as the same self-join defect.

### RTC-ALLOC-01f — queue accounting commits before insertion (P2; candidate implemented)

The pinned baseline `Queue::push` and `tryPush` incremented `mAmount` before `mQueue.emplace`
(`src/impl/queue.hpp:97`, `:106`). Refused allocation leaves an increased amount
without an inserted element. Compute the amount first, insert successfully,
then commit the amount. The same work-admission patch now calls production
`mdkrCommitQueueInsertion` after the unchanged full/stopped checks. Measurement
precedes movement and amount commits only after successful `emplace`. The helper
does not strengthen arbitrary element throwing-move or measurement contracts.
Its fixture uses real deque allocation refusal and checks measurement/move
refusal plus successful retry; these assertions have not executed.

The amendment touches only `threadpool.hpp`, `threadpool.cpp` and `queue.hpp`,
preserving their MPL headers. CMake applies it independently of the startup
patches and verifies it read-only for source overrides. Exact patch/helper/recipe
identities and the combined notice are bound by the third-party notice gate and
all desktop package validators. No dependency upgrade or blanket RTC-ALLOC
closure is implied.

## Caller recovery map

| Actual path | Existing boundary and necessary treatment |
| --- | --- |
| Certificate: `src/impl/certificate.cpp:598` → peer constructor `src/impl/peerconnection.cpp:61` | Native creation catches at `platform/online/match_peer_transport.cpp:690`; Phone Party catches at `platform/party/libdatachannel_party_transport.cpp:1125`. Pre-admission refusal should reach these paths without committing a peer. Failure inside an accepted certificate task must remain an exceptional future, consumed by the existing certificate users (`src/impl/peerconnection.cpp:242`, `:1063`). |
| TCP connect/retry: `src/impl/tcptransport.cpp:153`, `:199`, `:227`, `:470` | A caller can already be Connecting, executing an ignored-future task, or processing a poll callback. Every refusal needs per-connection state recovery; a stored-but-unobserved exception is insufficient. |
| Poll callback dispatch: `src/impl/pollservice.cpp:165` | Its outer exception handler surrounds the entire run loop (`:175`, `:211`). Letting submission refusal escape a callback can stop service for unrelated sockets. Contain failure per connection, not just at that outer boundary. |
| MbedTLS handshake timer: `src/impl/dtlstransport.cpp:546` | Scheduling is inside receive-error handling; preserve the resulting failed/disconnected state handling rather than lose the deadline silently. Non-timer receive entry points need their own counter/admission repair. |
| WebSocket connection deadline: `src/impl/websocket.cpp:269`, `:531` | Setup catches exceptions and calls `remoteClose`; that recovery is safe only after mandatory teardown no longer depends on a fresh allocation. |
| WebSocket close deadline: `src/impl/wstransport.cpp:92`, `:105` | `mCloseSent` is committed before scheduling. A missing deadline can leave graceful close waiting indefinitely; refusal must invoke a coherent terminal close path. |
| Peer notifications: `src/impl/peerconnection.cpp:1333`, `:1358`, `:1376` | State can commit before notification admission. Swallowing rejection loses required notification/continuation. Define failure state and notification policy, while keeping mandatory retirement independent of optional event allocation. |
| Log counter timer: `src/impl/logcounter.cpp:21` | Count increments before scheduling. Logging is optional, but refusal must not escape callbacks or permanently prevent future counter publication. Media-disabled reachability differs from the mandatory transport paths; inspect actual callers before claiming this is exercised in a release session. |

The first-party signal-client thread-start transaction is a separate owner-led
change, not evidence that vendor queue submission is fixed. Its optional close
event now cannot prevent mandatory worker joining. Additional first-party
factory/outbound/worker-reporting candidates retain ordinary refusal and terminal
evidence; their focused optimized/sanitizer builds are not vendor queue coverage.
The separate
[Windows network lifetime audit](windows-network-lifetime-audit.md) records
implemented shared lease candidates and their unqualified Windows acceptance.
Bounded first-party peer-setup recovery also cannot catch vendor `noexcept`
termination; it handles only failures that reach its recovery boundary.

## Implementation order and ownership boundaries

1. Qualify the implemented prepared-task seam and accepted continuation above.
   Preparation still allocates under the legacy `noexcept` API; accepted transfer
   uses owning intrusive nodes. Verify owner destruction, timer ordering, bounded
   admission, join and epoch behavior through the actual patched classes. Measure
   queue/load impact before claiming performance parity or improvement.
2. Reserve teardown state/capacity before resource acquisition. Integrate its
   exactly-once ownership with peer/WebSocket cycles and existing RTC epoch
   cleanup, all factory/start-failure/close-race paths, accepted/proxy/reopened
   WebSockets, and SCTP callback-retained owners. Test this independently before
   making refusal newly observable; a single close-only reservation is not enough.
3. Make Processor initial refusal recoverable only after the ownership prerequisite
   is implemented. Qualify the accepted-continuation candidate's FIFO, non-overlap,
   task-exception progress, final-owner destruction, join and teardown ordering.
4. Remove incorrect submission `noexcept` only together with explicit handling
   for every shipped caller above. Separate failed admission from accepted-task
   failure; do not promise an exceptional future can itself be allocated after
   allocation has already failed.
5. Qualify the implemented queue-accounting candidate and repair failure-reporting paths. Optional logging or UI
   event construction must not become the next unhandled operation inside
   recovery. Retain private non-allocating terminal state even if rich reporting
   cannot be produced.
6. Integrate through reviewed source-form dependency amendments and exact pin /
   notice checks. Do not hand-edit fetched source trees, bypass dependency
   authentication, relax admission, or claim the separate initialization
   transaction fixes these execution-time failures.

These are related but separable implementation units. A narrow dequeue correction
has been implemented independently; it must not be labeled a comprehensive
RTC-ALLOC-01 fix while -01a/-01c/-01d/-01e remain. Final integrated optimized,
ASan/UBSan and Windows cross-builds compile/link the game and
`datachannel_work_admission` fixture successfully. This is not a runtime
fault-injection pass or evidence for the unresolved paths; the Windows profile
also retains a historical cloud origin rather than proving the final partyless
artifact. Existing dependency compiler warnings remain.

Those three-profile builds precede the newer prepared-dispatch amendment.
The final optimized, ASan/UBSan and Windows cross-build checkpoints compile/link
the game, both prepared fixtures, the existing work fixture and Workshop model.
Generated CTest registration is present for all four fixtures in each profile.
The first
optimized attempt failed because the new vendor fixture lacked private header
include paths; CMake was corrected before the successful rerun. All final builds
include searchable mapping/detail focus, reinspection and the corrected
invalid-height diagnostic. Windows retains its historical cloud origin, not
final partyless artifact provenance. No runtime results are claimed.
The composed verifier now pins exact final `threadpool.hpp/.cpp`,
`processor.hpp/.cpp` and amended `queue.hpp` bytes alongside Init. It replaces
the overlapping old reverse-work check with the dependent prepared-patch check.
The shared helper, both patch forms and updated recipes are included in exact
source-form notices and desktop notice-hash gates. Source authentication does
not qualify runtime behavior or resolve the remaining classes.

## Required verification, not yet executed

Use actual production helpers or patched dependency classes, not a parallel mock
implementation. Failure injection must identify the intended admission phase;
an arbitrary global allocation failure can strike logging or fixture setup
instead and does not prove the desired branch.

- Binding/task/future/queue-insertion refusal leaves unaccepted ownership,
  accounting, and pending state unchanged; a successful retry runs exactly once.
- An accepted large callable is extracted without copying or allocation, with
  timer priorities preserved. Already-admitted execution does not require a new
  continuation allocation.
- Guard setup and task-body exceptions cannot strand `mPending`; FIFO and
  non-overlap persist, and `join()` completes. Include the last owner being held
  only by the current callback, and concurrent admission during completion.
- Peer and WebSocket teardown dispatched from an RTC callback remains off that
  callback thread with allocation refused after resource admission. Verify
  exactly-once stop/destruction, ordering, retained initialization token, and
  terminal cleanup future completion. Cover repeated close, factory/start refusal,
  concurrent close before publication completes, proxy/accepted WebSockets and
  reopen. Exercise SCTP upcall admission refusal without dropping its last owner
  on the upcall or stranding its Processor join.
- Receive rejection repairs admission counters; connect, handshake-timer and
  close-timer rejection produce terminal outcomes without recursive scheduling.
  One rejected connection does not stop the shared poll service.
- Certificate submission refusal reaches first-party construction recovery;
  accepted certificate-body failure is observed through its future. No partially
  initialized peer is published as usable.
- Queue full/stopped/refused-insertion cases preserve size/amount and owner
  release semantics. Optional reporting failure preserves authoritative failure
  state without a second escaping exception.
- Run focused fixtures under optimized and sanitizer builds, then validate
  real native multiplayer and Phone Party connect/retry/leave/Quit journeys on
  shipping platforms. Compilation and source-binding checks are prerequisites,
  not substitutes for those behavioral results.
