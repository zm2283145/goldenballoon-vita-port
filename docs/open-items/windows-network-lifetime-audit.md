# Windows first-party network lifetime audit

Status: WIN-NET-01 through WIN-NET-03 have implemented source candidates;
Windows behavioral and package qualification remain open. This documentation
checkpoint ran no tests, apps, or builds and does not establish release readiness.

## Historical findings and current disposition

| ID | Confirmed original defect | Implemented candidate / remaining proof |
| --- | --- | --- |
| WIN-NET-01 | `MdkrMatchSignalClient::create` called unchecked `WSAStartup` per accepted client, with no balancing first-party cleanup. Replacements and subsequent construction failures accumulated successful references. | Signal state now acquires a checked shared lease; close joins its worker before releasing it, and abandoned resolvers retain copies. Repeated-client, refusal/retry and Windows release evidence remain required. |
| WIN-NET-02 | Room `ensureNetStartup` and LAN `ensureSocketsInitialized` ignored and permanently cached startup failure. These were bounded process references, not WIN-NET-01's per-client growth. | Those helpers are removed. HTTP/WS sockets and the LAN listener now own checked leases instead of implicit process initialization. Actual Windows startup/cleanup faults, listener retry and terminal outcomes remain unqualified. |
| WIN-NET-03 | Windows LAN address enumeration used `gethostname`/`getaddrinfo` before owning Winsock; launcher availability checking preceded server startup. This could disable cold-start phone hosting as “no network,” not ordinary local-controller gameplay. | Enumeration acquires its own checked lease before lookup; RAII releases address results before the lease. Cold-start Windows LAN phone hosting with no prior cloud session still needs observed acceptance. |

## Implemented ownership policy

`platform/net/network_lifetime.h` supplies `MdkrSharedNetworkLease` and
`MdkrWinsockReferencePolicy`; `native_socket_lifetime.h` supplies the small OS
adapter and a no-allocation non-Windows path. Allocation precedes successful OS
acquisition. Copies share one acquired reference without new allocation; unrelated
owners may acquire separately and each successful reference is balanced by its
last copy. There is no static process lease or guessed third-party reference count.
Mutation of the same lease remains caller-serialized.

Startup checks its return value and negotiated Winsock 2.2 version; unsupported
successful acquisition is released. Cleanup failure is sticky, prevents fresh
native acquisition, and contributes to `Launcher::networkShutdownFailed()`.
The Windows adapter defines lean Windows headers and `NOMINMAX` before Winsock;
this is a source portability correction, not a Windows build/runtime result.

Room/signal `ResolveTask` owns its copied host/port, address-result owner, network
lease and `AsyncWorkBudget::Permit`. Destruction releases results before the
lease and the lease before the permit. A cancelled client can join its socket
worker while OS lookup remains alive; that lookup retains its prerequisite.
Eight limits outstanding first-party lookups, not connections/players, and does
not cancel `getaddrinfo` or count third-party resolver work.

LAN startup stages its listener in a local socket owner while a local lease
remains alive. Refused setup/thread launch restores unpublished listener state;
the fd closes before the local prerequisite is released. Successful state owns
the lease until accept/connection workers are joined. WebSocket aliases receive
their own copies before publication and can safely outlive server stop. Windows
address results and POSIX interface results also use RAII for allocation unwind.

`Launcher::pollNetworkShutdown` waits for room/phone owner retirement and zero
first-party resolver work before starting RTC cleanup. Resolver cleanup-failure
evidence is published before its last permit disappears. No new producer may
start then. Both the cleanup future and sticky first-party failure contribute to
the final failure verdict; failed cleanup cannot authorize successful relaunch.

RTC, libjuice and usrsctp retain separate ownership. The initialization patch
balances its own successful `Stage::Sockets` acquisition, not first-party leases.
First-party connectors do not rely on `mbedtls_net_connect` startup;
`mbedtls_net_free` closes the socket, not Winsock.

## Related close/admission boundaries

The signaling close candidate now commits Closed/stopping and transfers the
joinable worker before attempting its optional pending-connect event. Reporting
allocation refusal is contained; joining and lease release still occur. The
actual-client fixture includes a one-shot close-event refusal, but has not run.
The related factory, atomic outbound/correlation admission and nonallocating
terminal-report fallback candidates are detailed in the
[signaling audit](native-rtc-initialization-audit.md#rtc-signal-02--factory-outbound-admission-and-terminal-report-recovery).
Integrated optimized, ASan/UBSan and Windows game/fixture builds pass, not runtime
fault injection or final Windows artifact qualification.

The separate LAN admission candidate now stages accepted sockets through
connection allocation, registry insertion and handler launch; refusal rolls back
the registry and closes the fd. Reaping retains join ownership without an
allocating temporary collection. A common connection finalizer seals WebSocket
sends, contains consumer exceptions and synchronizes fd invalidation with stop.
The listener is nonblocking and passed as an immutable worker argument; oversized
POSIX fd-set indices are refused before use. A server callback can request stop
but cannot become its own join owner; the launcher completes joining.
Mandatory receive/send timeouts and the required no-SIGPIPE option where defined
must succeed before worker admission. The callback worker marker remains active
through explicit release of captured WebSocket callbacks, so a capture destructor
that requests stop cannot become its own joining owner.

Six actual-server fixture groups cover listener refusal/restart, accepted
admission refusal, upgrade allocation refusal, consumer exceptions, and callback
stop plus callback-capture destruction; accepted refusal includes timeout setup.
They are authored but unexecuted. Strict production/fixture syntax and final
integrated builds pass; Windows/runtime acceptance remains pending. Exceptional
destructor join or self-join-contract failure deliberately retains server state
and records failed network cleanup rather than destroying a joinable owner.
That fallback is incomplete cleanup, never success. Broader LAN/RTC callback and
allocation qualification remains open.

## Verification and remaining proof

The final optimized, ASan/UBSan and Windows cross-build checkpoints compile/link
the game plus `network_lifetime`, `party_peer_setup_retry`,
`datachannel_work_admission`, `match_signal_client` and `lan_party_server`.
All include the signal local-buffer guards, queued/terminal refusal coverage and
LAN mandatory-timeout/callback-capture-retirement follow-ups. Independent bounded
source review found no additional introduced issue. The Windows build retains
native beta and its historical nonempty cloud origin; it is not the intended
final partyless artifact. Existing BasisU GCC and libdatachannel function-cast
warnings remain, not suppressed or cleared. No new behavioral assertions have
executed, and cross-compilation is not a Windows-native acceptance result.

- `network_lifetime` uses the production shared lease/policy with fake OS calls:
  allocation-before-start, refused-start retry, negotiated-version rejection,
  cleanup-failure latching, copied ownership, result-before-release, concurrent
  independent copies, repeated sessions and non-Windows no-op behavior.
- `network_lifetime_source` has eight methods binding those helpers to the actual
  OS adapter, resolver member order, socket close order, LAN lookup/listener/aliases,
  mandatory timeout admission, callback-capture retirement and launcher failure
  verdict. Source contracts are not actual OS fault injection.
- Still required: execute these fixtures and real Windows cold-start phone
  hosting, reconnect, LAN/cloud switching, retained-alias close, listener refusal
  and retry, abandoned resolver teardown, and failure/no-relaunch journeys.
  Include packaged Windows and relevant RTC-disabled configurations.
- Keep broader caller/queue allocation handling and LAN runtime qualification,
  RTC-ALLOC-01a/c/d/e and internal C startup exits explicit. There is no hard
  OS/DNS completion deadline or blanket network cleanup guarantee.

No dependency upgrade, network request, publishing, or GitHub mutation was
performed for this documentation checkpoint.
