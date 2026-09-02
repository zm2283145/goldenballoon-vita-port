/*
 * online multiplayer end-to-end race driver.
 *
 * One narrated child process that runs the REAL online stack against a live
 * local MatchRoom Worker (wrangler dev --local): the production
 * MdkrOnlineRoomTransport (HTTP create/join/code/command + the /connect state
 * WebSocket), the production MdkrOnlineMeshSignalBackend (the signal client
 * over /api/match/{roomId}/signal), the peer mesh (real libdatachannel
 * DTLS), the live adapter (selections, phrase, preflight consensus,
 * descriptor install through the clamp) and the per-tick race feed.
 *
 * Two of these processes -- one creator, one joiner-by-code -- race >=1800
 * authored ticks feeding each other real remote input over the mesh, then each
 * prints the FNV state hash of its own confirmed canonical-input timeline. The
 * harness asserts the two hashes are byte-identical.
 *
 * Owned by tests/check_online_live_transport_e2e.py, NOT a ctest: it needs the
 * live Worker the check provides. Every observable transition is one
 * `[E2E] key=value` line on stdout the check tails.
 *
 *   --origin <url>       loopback Worker origin (http://127.0.0.1:PORT).
 *   --journey create|join
 *   --join-code <6d>     required for --journey join (the creator's fallback).
 *   --character <0..9>   this endpoint's racer (must differ between processes).
 *   --track <0..N>       track vote.
 *   --ticks <n>          confirmed authored ticks to race (default 1800).
 *   --timeout-ms <t>     overall budget (default 180000).
 *
 * Exit codes: 0 success, 1 usage, 2 setup/room error, 3 timeout.
 */
#include "online/match_live_adapter.h"
#include "online/match_live_transport.h"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Options {
    std::string origin;
    std::string journey;
    std::string joinCode;
    unsigned character = 1u;
    unsigned track = 5u;
    uint32_t ticks = 1800u;
    uint64_t timeoutMs = 180000u;
};

bool parseOptions(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--origin" && has) o.origin = argv[++i];
        else if (a == "--journey" && has) o.journey = argv[++i];
        else if (a == "--join-code" && has) o.joinCode = argv[++i];
        else if (a == "--character" && has)
            o.character = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--track" && has)
            o.track = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--ticks" && has)
            o.ticks = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--timeout-ms" && has)
            o.timeoutMs = std::strtoull(argv[++i], nullptr, 10);
        else {
            std::fprintf(stderr, "usage: %s --origin URL --journey create|join "
                         "[--join-code C] [--character N] [--track N] "
                         "[--ticks N] [--timeout-ms T]\n", argv[0]);
            return false;
        }
    }
    if (o.origin.empty() || (o.journey != "create" && o.journey != "join")) {
        std::fprintf(stderr, "--origin and --journey create|join are required\n");
        return false;
    }
    if (o.journey == "join" && o.joinCode.empty()) {
        std::fprintf(stderr, "--journey join requires --join-code\n");
        return false;
    }
    return true;
}

MdkrOnlineCompatibilityV1 compatibilityFixture() {
    MdkrOnlineCompatibilityV1 c;
    std::memset(&c, 0, sizeof(c));
    c.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(c.build_id); ++i)
        c.build_id[i] = static_cast<uint8_t>(i + 1u);
    for (unsigned i = 0u; i < sizeof(c.gameplay_digest); ++i)
        c.gameplay_digest[i] = static_cast<uint8_t>(0x80u + i);
    c.rom_revision = 1u;
    c.cadence_hz = 30u;
    return c;
}

void emit(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::printf("[E2E] ");
    std::vprintf(fmt, args);
    std::printf("\n");
    std::fflush(stdout);
    va_end(args);
}

MdkrOnlineViewModel viewOf(IMdkrOnlineAdapter *a) {
    MdkrOnlineViewModel m{};
    a->view(&m);
    return m;
}

uint64_t g_deadline = 0u;

/* Pump the adapter until `done` holds or the overall budget elapses. */
bool pumpUntil(IMdkrOnlineAdapter *a, const std::function<bool()> &done) {
    while (nowMs() < g_deadline) {
        a->service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    a->service();
    return done();
}

MdkrOnlineAdapterCommand cmd(IMdkrOnlineAdapter *a, MdkrOnlineViewAction action,
                             unsigned seat = 0u, unsigned value = 0u) {
    static uint64_t nextId = 1u;
    MdkrOnlineAdapterCommand c;
    c.expectedRevision = a->revision();
    c.requestId = nextId++;
    c.action = action;
    c.seat = seat;
    c.value = value;
    return c;
}

/* Wait for a specific primary action to be offered, then submit it. */
bool submitWhenOffered(IMdkrOnlineAdapter *a, MdkrOnlineViewAction action,
                       unsigned seat, unsigned value) {
    if (!pumpUntil(a, [&]() { return viewOf(a).primary.action == action; })) {
        return false;
    }
    return a->submit(cmd(a, action, seat, value)).accepted;
}

/* FNV-1a fold of one confirmed canonical frame over the active slots -- the
 * ROM-free engine advance the two processes must agree on. */
void foldFrame(uint64_t &hash, uint32_t tick, uint8_t activeMask,
               const MdkrInputSet &frame) {
    auto mix = [&hash](uint64_t v) {
        for (unsigned b = 0u; b < 8u; ++b) {
            hash ^= (v >> (b * 8u)) & 0xffu;
            hash *= UINT64_C(1099511628211);
        }
    };
    mix(tick);
    for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
        if ((activeMask & (1u << slot)) == 0u) continue;
        mix(frame.slots[slot].buttons);
        mix(static_cast<uint64_t>(static_cast<uint8_t>(frame.slots[slot].stick_x)));
        mix(static_cast<uint64_t>(static_cast<uint8_t>(frame.slots[slot].stick_y)));
    }
}

int race(IMdkrOnlineAdapter *adapter, const Options &options) {
    MdkrOnlineLiveRaceInfo info{};
    if (!pumpUntil(adapter, [&]() {
            return mdkr_online_live_adapter_race_info(adapter, &info) &&
                   info.ready;
        })) {
        emit("result=error message=race transport never came up");
        return 2;
    }
    emit("installed epoch=%u firstTick=%u active=%u local=%u remote=%u",
         info.matchEpoch, info.firstTick, info.activeSlotMask,
         info.localSlotMask, info.remoteSlotMask);

    const uint32_t target = info.firstTick + options.ticks - 1u;
    /* Keep both endpoints' drain positions tight around the shared confirmed
     * frontier. The retained input window is 30 authored ticks
     * (MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS); a larger lead lets a peer's
     * frontier retransmit age past that window and be rejected out-of-window,
     * wedging convergence. 8 keeps the worst-case retransmit age (~2*throttle)
     * comfortably inside 30 while still pipelining ahead of the input delay. */
    const uint32_t throttle = 8u;
    /* The gameplay state channel is unordered + maxRetransmits 0 (lossy by
     * design): blasting a send every loop overruns the SCTP send buffer and
     * wedges delivery. Pace one authored tick per interval -- brisk (finishes
     * 1800 ticks in ~15 s) but well within what loopback DTLS carries without
     * loss, while service() keeps draining/flushing every millisecond. */
    const uint64_t advanceIntervalMs = 6u;
    uint64_t lastAdvanceMs = 0u;
    uint32_t cursor = info.firstTick;
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t lastNarrate = 0u;

    while (nowMs() < g_deadline) {
        /* Pump ordering is load-bearing (match_live_adapter.h integration
         * contract): service() folds remote input BEFORE this frame's
         * advance/drain, keeping every remote input at most one service()
         * old. Do not reorder. */
        adapter->service();
        MdkrOnlineLiveRaceInfo cur{};
        mdkr_online_live_adapter_race_info(adapter, &cur);
        const uint64_t now = nowMs();
        /* Flow control: pace the send AND never lead the confirmed frontier by
         * more than the throttle, so a sent future frame always lands inside
         * the peer's input window and no unrecoverable gap can open. */
        if (now - lastAdvanceMs >= advanceIntervalMs) {
            if (cur.nextTick <= target && cur.nextTick - cursor < throttle) {
                mdkr_online_live_adapter_race_advance(adapter);
            }
            /* Retransmit a window that also covers the PEER's frontier. The two
             * confirmed frontiers stay within `throttle` of each other, and the
             * peer needs OUR input for the tick just past ITS frontier -- which
             * may be below ours. Sweep bundles (each 3 frames) down from our
             * frontier across the whole coupling window so a datagram dropped
             * anywhere in it gets another chance without a permanent wedge. */
            const uint32_t top = cursor + info.inputDelay;
            const uint32_t low = cursor > info.firstTick + throttle
                                     ? cursor - throttle
                                     : info.firstTick;
            for (uint32_t t = top;;) {
                mdkr_online_live_adapter_race_resend(adapter, t);
                if (t <= low) break;
                t = t >= low + 3u ? t - 3u : low;
            }
            lastAdvanceMs = now;
        }
        MdkrInputSet frame;
        while (mdkr_online_live_adapter_race_inputs_for_tick(adapter, cursor,
                                                             &frame)) {
            if ((frame.confirmed_mask & info.activeSlotMask) !=
                info.activeSlotMask) {
                break;
            }
            foldFrame(hash, cursor, info.activeSlotMask, frame);
            ++cursor;
        }
        if (cursor - info.firstTick >= lastNarrate + 300u) {
            lastNarrate = cursor - info.firstTick;
            MdkrOnlineLiveRaceStats st{};
            mdkr_online_live_adapter_race_stats(adapter, &st);
            emit("progress ticks=%u nextTick=%u envs=%llu accepted=%u "
                 "corrected=%u oow=%llu drained=%u", cursor - info.firstTick,
                 cur.nextTick, (unsigned long long)st.inputEnvelopesReceived,
                 st.transportAccepted, st.transportCorrected,
                 (unsigned long long)st.transportOutOfWindow,
                 st.transportDrained);
        }
        if (cursor > target) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /* Linger: this endpoint reached the target, but the peer may be a few ticks
     * behind. Keep the mesh alive and keep retransmitting the tail so the peer
     * can confirm its final ticks before we tear the transport down. Exit early
     * once the peer has clearly finished too (no more of our tail is needed). */
    if (cursor > target) {
        const uint32_t tailStart =
            target > info.firstTick + throttle ? target - throttle
                                               : info.firstTick;
        const uint64_t lingerUntil = nowMs() + 8000u;
        while (nowMs() < lingerUntil && nowMs() < g_deadline) {
            adapter->service();
            for (uint32_t t = tailStart; t <= target; ++t) {
                mdkr_online_live_adapter_race_resend(adapter, t);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
    }

    const uint32_t confirmed = cursor - info.firstTick;
    if (cursor <= target) {
        MdkrOnlineLiveRaceStats st{};
        mdkr_online_live_adapter_race_stats(adapter, &st);
        MdkrOnlineLiveRaceInfo cur{};
        mdkr_online_live_adapter_race_info(adapter, &cur);
        emit("result=timeout ticks=%u nextTick=%u envs=%llu accepted=%u "
             "corrected=%u dup=%u oow=%llu drained=%u rejState=%llu "
             "ignoredSignals=%llu", confirmed, cur.nextTick,
             (unsigned long long)st.inputEnvelopesReceived,
             st.transportAccepted, st.transportCorrected,
             st.transportDuplicates, (unsigned long long)st.transportOutOfWindow,
             st.transportDrained, (unsigned long long)st.meshRejectedState,
             (unsigned long long)st.meshIgnoredStaleSignals);
        return 3;
    }
    emit("result=ok ticks=%u hash=%016llx", confirmed,
         static_cast<unsigned long long>(hash));
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) return 1;
    g_deadline = nowMs() + options.timeoutMs;

    std::string error;
    std::unique_ptr<MdkrOnlineRoomTransport> room =
        mdkr_online_room_http_transport_create(options.origin, &error);
    if (!room) {
        emit("result=error message=room transport refused: %s", error.c_str());
        return 2;
    }
    std::unique_ptr<MdkrOnlineMeshSignalBackend> mesh =
        mdkr_online_mesh_signal_backend_create(options.origin);

    MdkrOnlineLiveAdapterOptions adapterOptions;
    adapterOptions.sessionId = UINT64_C(0x4f4e4c494e45);
    adapterOptions.compatibility = compatibilityFixture();
    adapterOptions.journey = options.journey == "create"
                                 ? MDKR_ONLINE_JOURNEY_CREATE
                                 : MDKR_ONLINE_JOURNEY_JOIN;
    adapterOptions.localSeatCount = 1u;
    adapterOptions.joinCode = options.joinCode;
    for (unsigned i = 0u; i < MDKR_MATCH_LOCAL_PLAYER_SLOTS; ++i)
        adapterOptions.localRoster.player_identity[i] = MDKR_MATCH_IDENTITY_RETAIL;
    adapterOptions.raceAdmissionEnabled = true;
    adapterOptions.romVerified = true;
    adapterOptions.inputDelay = 2u;
    adapterOptions.room = room.get();
    adapterOptions.meshBackend = mesh.get();

    std::unique_ptr<IMdkrOnlineAdapter> adapter =
        mdkr_online_live_adapter_create(adapterOptions, &error);
    if (!adapter) {
        emit("result=error message=adapter create failed: %s", error.c_str());
        return 2;
    }
    IMdkrOnlineAdapter *a = adapter.get();
    emit("origin=%s journey=%s", options.origin.c_str(),
         options.journey.c_str());

    /* 1. Create or join the room. */
    const MdkrOnlineViewAction entryAction =
        adapterOptions.journey == MDKR_ONLINE_JOURNEY_CREATE
            ? MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM
            : MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM;
    if (!a->submit(cmd(a, entryAction)).accepted) {
        emit("result=error message=entry action refused");
        return 2;
    }
    if (!pumpUntil(a, [&]() {
            return viewOf(a).kind == MDKR_ONLINE_VIEW_ROOM ||
                   viewOf(a).kind == MDKR_ONLINE_VIEW_RECOVERY;
        })) {
        emit("result=error message=room never opened");
        return 2;
    }
    if (viewOf(a).kind == MDKR_ONLINE_VIEW_RECOVERY) {
        emit("result=error message=room entry failed failure=%d",
             (int)viewOf(a).failure);
        return 2;
    }

    if (adapterOptions.journey == MDKR_ONLINE_JOURNEY_CREATE) {
        MdkrOnlineRoomHttpInvite invite{};
        (void)mdkr_online_room_http_transport_invite(room.get(), &invite);
        emit("room=%s code=%s", invite.roomId.c_str(),
             invite.fallbackCode.c_str());
    } else {
        emit("joined");
    }

    /* 2. Wait for both endpoints present. */
    if (!pumpUntil(a, [&]() { return viewOf(a).member_count >= 2u; })) {
        emit("result=error message=second endpoint never joined");
        return 2;
    }
    emit("members=2");

    /* 3. Secure-setup: bring up the mesh, compute + confirm the phrase. */
    if (!a->submit(cmd(a, MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP)).accepted) {
        emit("result=error message=check setup refused");
        return 2;
    }
    if (!pumpUntil(a, [&]() {
            return viewOf(a).verification_phrase[0] != '\0' ||
                   viewOf(a).kind == MDKR_ONLINE_VIEW_RECOVERY;
        })) {
        emit("result=error message=verification phrase never appeared");
        return 2;
    }
    if (viewOf(a).kind == MDKR_ONLINE_VIEW_RECOVERY) {
        emit("result=error message=secure setup failed failure=%d",
             (int)viewOf(a).failure);
        return 2;
    }
    emit("phrase=%s", viewOf(a).verification_phrase);

    if (!a->submit(cmd(a, MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE)).accepted) {
        emit("result=error message=confirm phrase refused");
        return 2;
    }
    if (!pumpUntil(a, [&]() {
            return viewOf(a).kind == MDKR_ONLINE_VIEW_SELECTING;
        })) {
        emit("result=error message=never reached selection");
        return 2;
    }
    emit("selecting");

    /* 4. Selections + ready. */
    if (!submitWhenOffered(a, MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER, 0u,
                           options.character) ||
        !submitWhenOffered(a, MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE, 0u, 0u) ||
        !submitWhenOffered(a, MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK, 0u,
                           options.track) ||
        !submitWhenOffered(a, MDKR_ONLINE_VIEW_ACTION_READY, 0u, 1u)) {
        emit("result=error message=selection/ready flow stalled");
        return 2;
    }
    if (!pumpUntil(a, [&]() { return viewOf(a).ready_count >= 2u; })) {
        emit("result=error message=both endpoints never readied");
        return 2;
    }
    emit("ready=2");

    /* 5. The leader starts the race; both follow the LOADING phase. */
    if (adapterOptions.journey == MDKR_ONLINE_JOURNEY_CREATE) {
        if (!submitWhenOffered(a, MDKR_ONLINE_VIEW_ACTION_START_RACE, 0u, 1u)) {
            emit("result=error message=start race refused");
            return 2;
        }
    }
    emit("loading");

    /* 6. Preflight consensus, install, and the tick race. */
    return race(a, options);
}
