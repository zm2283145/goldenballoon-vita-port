#include "platform/online/lobby_view_model.h"
#include "platform/session/session_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrOnlineCompatibilityV1 compatibility(void) {
    MdkrOnlineCompatibilityV1 value;
    unsigned index;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (index = 0u; index < sizeof(value.build_id); index++) {
        value.build_id[index] = (uint8_t)(index + 1u);
    }
    for (index = 0u; index < sizeof(value.gameplay_digest); index++) {
        value.gameplay_digest[index] = (uint8_t)(0x80u + index);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

static MdkrSessionStep session_command(MdkrSessionCore *core,
                                       MdkrSessionCommandType type,
                                       uint32_t value) {
    MdkrSessionCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    command.expected_generation = core->state.generation;
    command.type = type;
    command.value = value;
    return mdkr_session_core_dispatch(core, &command);
}

static MdkrOnlineStep lobby_command_as(MdkrOnlineLobby *lobby,
                                       uint64_t actor,
                                       uint64_t command_id,
                                       MdkrOnlineCommandType type,
                                       unsigned seat, unsigned value) {
    MdkrOnlineCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.expected_revision = lobby->revision;
    command.command_id = command_id;
    command.actor_endpoint_id = actor;
    command.type = type;
    command.target_endpoint_id = seat;
    command.value = value;
    return mdkr_online_lobby_dispatch(lobby, &command);
}

static MdkrOnlineStep lobby_command(MdkrOnlineLobby *lobby,
                                    uint64_t command_id,
                                    MdkrOnlineCommandType type,
                                    unsigned seat, unsigned value) {
    return lobby_command_as(lobby, 10u, command_id, type, seat, value);
}

static MdkrOnlineStep lobby_join(MdkrOnlineLobby *lobby, uint64_t actor,
                                 unsigned seats,
                                 const MdkrOnlineCompatibilityV1 *compat) {
    MdkrOnlineCommand command;
    memset(&command, 0, sizeof(command));
    command.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    command.expected_revision = lobby->revision;
    command.command_id = 1u;
    command.actor_endpoint_id = actor;
    command.type = MDKR_ONLINE_JOIN;
    command.value = seats;
    command.compatibility = *compat;
    return mdkr_online_lobby_dispatch(lobby, &command);
}

static MdkrOnlineViewInput input_for(const MdkrSessionCore *session,
                                     const MdkrOnlineLobby *lobby) {
    MdkrOnlineViewInput input;
    memset(&input, 0, sizeof(input));
    input.session = &session->state;
    input.lobby = lobby;
    input.local_endpoint_id = 10u;
    input.journey = MDKR_ONLINE_JOURNEY_CREATE;
    return input;
}

static void expect_complete(const MdkrOnlineViewModel *model,
                            const char *message) {
    expect(model->title != NULL && model->title[0] != '\0', message);
    expect(model->explanation != NULL && model->explanation[0] != '\0', message);
    expect(model->primary.visible && model->primary.enabled &&
           model->primary.label != NULL, message);
    expect(model->cancel.visible && model->cancel.enabled &&
           model->cancel.label != NULL, message);
}

static void test_entry_connecting_and_timeouts(void) {
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 1u);
    input = input_for(&session, NULL);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_ENTRY &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CREATE_ROOM &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_JOIN_ROOM &&
           model.local_play_available,
           "home projects both private-room routes without hiding local play");
    expect_complete(&model, "entry copy/control contract is complete");

    expect(session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u).accepted,
           "online journey begins through session reducer");
    input = input_for(&session, NULL);
    input.journey = MDKR_ONLINE_JOURNEY_JOIN;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_CONNECTING && model.timeout.present &&
           model.timeout.primary.action == MDKR_ONLINE_VIEW_ACTION_RETRY &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_RETURN_HOME,
           "joining wait has progress, cancel and bounded timeout recovery");
    expect_complete(&model, "connecting copy/control contract is complete");
}

static void test_room_selection_and_release_gate(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 2u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    expect(mdkr_online_lobby_init(&lobby, 99u, 10u, &compat, 1u),
           "single-endpoint lobby initializes");
    expect(lobby_join(&lobby, 20u, 1u, &compat).accepted,
           "friend joins release-gate fixture");

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_OPEN);
    input = input_for(&session, &lobby);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_ROOM &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP &&
           !model.secondary.enabled,
           "joined room advances to setup while invite preparation stays explicit");
    input.invite_state = MDKR_ONLINE_INVITE_READY;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE &&
           model.secondary.enabled,
           "room enables secondary sharing only after launcher owns an invite");
    input.invite_state = MDKR_ONLINE_INVITE_REFRESH_AVAILABLE;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE &&
           model.secondary.enabled &&
           strcmp(model.secondary.label, "New Invitation") == 0,
           "expired leader invite has a clear replacement recovery");
    input.invite_state = MDKR_ONLINE_INVITE_READY;
    input.local_endpoint_id = 20u;
    expect(mdkr_online_view_model_build(&input, &model) &&
           !model.local_member_is_leader &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS &&
           model.secondary.enabled,
           "guests keep connection details instead of host invite custody");
    input.local_endpoint_id = 10u;

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_PREFLIGHT);
    input.invite_state = MDKR_ONLINE_INVITE_PREPARING;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_PREFLIGHT && model.timeout.present &&
           model.verification_phrase[0] == '\0' &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS &&
           model.announcement == MDKR_ONLINE_ANNOUNCE_POLITE,
           "preflight has non-blocking status and timeout recovery");
    expect_complete(&model, "preflight copy/control contract is complete");

    {
        char mutable_phrase[] =
            "Nimble-Pilot Jolly-Star Sunny-Falcon";
        input.verification_phrase = mutable_phrase;
        expect(mdkr_online_view_model_build(&input, &model) &&
               model.kind == MDKR_ONLINE_VIEW_PREFLIGHT &&
               !model.timeout.present &&
               strcmp(model.verification_phrase, mutable_phrase) == 0 &&
               model.primary.action ==
                   MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE &&
               model.secondary.action ==
                   MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH &&
               strcmp(model.primary.label, "Words Match") == 0 &&
               strcmp(model.secondary.label, "Words Differ") == 0 &&
               strstr(model.explanation, "every display") != NULL,
               "authenticated phrase requires one explicit human confirmation");
        mutable_phrase[0] = 'T';
        expect(strcmp(model.verification_phrase,
                      "Nimble-Pilot Jolly-Star Sunny-Falcon") == 0,
               "view model owns an immutable bounded phrase copy");
    }
    {
        static const char *invalid_phrases[] = {
            "Nimble Pilot Jolly-Star Sunny-Falcon",
            "nimble-Pilot Jolly-Star Sunny-Falcon",
            "Nimble-Pilot  Jolly-Star Sunny-Falcon",
            "Nimble-Pilot Jolly-Star Sunny-Falcon Extra-Word",
            "Nimble-Pilot Jolly-Star Sunny-<Falcon",
        };
        MdkrOnlineViewModel sentinel;
        unsigned phrase;
        memset(&sentinel, 0x5a, sizeof(sentinel));
        for (phrase = 0u;
             phrase < sizeof(invalid_phrases) / sizeof(invalid_phrases[0]);
             phrase++) {
            model = sentinel;
            input.verification_phrase = invalid_phrases[phrase];
            expect(!mdkr_online_view_model_build(&input, &model) &&
                   memcmp(&model, &sentinel, sizeof(model)) == 0,
                   "malformed verification phrase rejects fail-atomically");
        }
    }
    {
        char unterminated_phrase[64];
        MdkrOnlineViewModel sentinel;
        memset(unterminated_phrase, 'A', sizeof(unterminated_phrase));
        memset(&sentinel, 0x4c, sizeof(sentinel));
        model = sentinel;
        input.verification_phrase = unterminated_phrase;
        expect(!mdkr_online_view_model_build(&input, &model) &&
               memcmp(&model, &sentinel, sizeof(model)) == 0,
               "unterminated verification phrase rejects at the 64-byte bound");
    }
    input.verification_phrase = NULL;
    input.failure = MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
           model.announcement == MDKR_ONLINE_ANNOUNCE_ASSERTIVE &&
           model.verification_phrase[0] == '\0' &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_RETRY &&
           strcmp(model.primary.label, "Reconnect Securely") == 0 &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM &&
           strstr(model.explanation, "Do not continue") != NULL,
           "phrase mismatch stops progression with explicit secure recovery");
    input.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;

    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
    input.verification_phrase = "Nimble-Pilot Jolly-Star Sunny-Falcon";
    {
        MdkrOnlineViewModel sentinel;
        memset(&sentinel, 0x6b, sizeof(sentinel));
        model = sentinel;
        expect(!mdkr_online_view_model_build(&input, &model) &&
               memcmp(&model, &sentinel, sizeof(model)) == 0,
               "stale phrase cannot escape the preflight view");
    }
    input.verification_phrase = NULL;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_SELECTING &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER,
           "selection points to the first incomplete launcher-owned choice");
#if MDKR_ENABLE_ONLINE_BETA
    /* never-silent hole: selection arms the same 30 s view-timeout card
     * the other lobby surfaces carry, so an endless wait always offers a working
     * escape rather than a dead spinner. Beta-gated (OFF/release view model is
     * byte-identical). */
    expect(model.timeout.present &&
           model.timeout.title != NULL &&
           strcmp(model.timeout.title, "Selection Took Too Long") == 0 &&
           model.timeout.explanation != NULL &&
           model.timeout.primary.action == MDKR_ONLINE_VIEW_ACTION_LEAVE_ROOM &&
           model.timeout.primary.label != NULL &&
           strcmp(model.timeout.primary.label, "Leave Room") == 0,
           "selecting arms the never-silent view-timeout card (beta)");
#endif
    expect(lobby_command(&lobby, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 2u).accepted,
           "character selection accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE,
           "vehicle follows character in one-decision selection flow");
    expect(lobby_command(&lobby, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u).accepted,
           "vehicle selection accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK,
           "track vote follows canonical racer choices");
    expect(lobby_command(&lobby, 3u, MDKR_ONLINE_SET_VOTE, 0u, 5u).accepted,
           "track vote accepted");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_READY,
           "complete selection enables Ready");
    expect(lobby_command(&lobby, 4u, MDKR_ONLINE_SET_READY, 0u, 1u).accepted,
           "member becomes ready");
    expect(lobby_command_as(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 3u).accepted &&
           lobby_command_as(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted &&
           lobby_command_as(
               &lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE, 1u, 5u).accepted &&
           lobby_command_as(
               &lobby, 20u, 5u, MDKR_ONLINE_SET_READY, 1u, 1u).accepted,
           "friend completes selections and becomes ready");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.status != NULL && strcmp(model.status, "Everyone Ready") == 0 &&
           model.primary.action != MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "service state cannot expose Start before local rollback GO");
    input.race_admission_enabled = true;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "reviewed local release policy can expose Start without view rewiring");
}

static void test_loading_racing_and_results(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 3u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    mdkr_online_lobby_init(&lobby, 100u, 10u, &compat, 1u);
    lobby_join(&lobby, 20u, 1u, &compat);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
    lobby_command(&lobby, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 1u);
    lobby_command(&lobby, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u);
    lobby_command(&lobby, 3u, MDKR_ONLINE_SET_VOTE, 0u, 5u);
    lobby_command(&lobby, 4u, MDKR_ONLINE_SET_READY, 0u, 1u);
    lobby_command_as(&lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 2u);
    lobby_command_as(&lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u);
    lobby_command_as(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE, 1u, 5u);
    lobby_command_as(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY, 1u, 1u);
    expect(lobby_command(&lobby, 5u, MDKR_ONLINE_BEGIN_LOADING, 0u, 1u).accepted,
           "lobby begins loading");
    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_LOADING);
    expect(session_command(&session, MDKR_SESSION_COMMAND_REQUEST_RACE, 0u).accepted,
           "session begins matching engine loan");
    input = input_for(&session, &lobby);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_LOADING && model.timeout.present &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_RETURN_TO_LOBBY,
           "load barrier has cancel and room-preserving timeout");

    session.state.room = MDKR_ROOM_COUNTDOWN;
    expect(mdkr_session_state_valid(&session.state) &&
           mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_COUNTDOWN && model.timeout.present,
           "countdown has explicit mismatch recovery");
    session.state.room = MDKR_ROOM_LOADING;

    lobby_command(&lobby, 6u, MDKR_ONLINE_ACK_LOADED, 0u, 0u);
    lobby_command_as(&lobby, 20u, 6u, MDKR_ONLINE_ACK_LOADED, 0u, 0u);
    lobby_command(&lobby, 7u, MDKR_ONLINE_BEGIN_RACE, 0u, 0u);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_READY);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_RACING);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RACING &&
           model.cancel.action == MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE &&
           strstr(model.explanation, "non-pausing") != NULL,
           "race chrome states non-pausing behavior and deliberate leave");

    /* Packed placements: seat 0 first, seat 1 second, seats 2/3 unoccupied. */
    lobby_command(&lobby, 8u, MDKR_ONLINE_PUBLISH_RESULTS, 0u, 0xFFFF0100u);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                    MDKR_ENGINE_FINISHED);
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RESULTS &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN &&
           model.secondary.action == MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK,
           "results keep the party and offer a clear rematch path");
    expect_complete(&model, "leader results copy/control contract is complete");

    /* R1 precedence: the view builder gives ANY failure precedence over RESULTS,
     * so a mid-race PeerLost's lingering loss-mapped failure would hijack a
     * genuinely captured finish. This is exactly why the launcher clears that
     * latch on the results-capture path -- with it set the recovery card wins,
     * with it cleared (NONE) the published RESULTS screen fronts. */
    input.failure = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_CHECK;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RECOVERY,
           "a lingering failure hijacks RESULTS (why the launcher clears it)");
    input.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RESULTS,
           "cleared failure lets the published RESULTS screen front");

    /* Only the room leader can send REMATCH: a guest's results view must
     * never offer a dead Race Again button, and it names the next actor. */
    input.local_endpoint_id = 20u;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RESULTS &&
           !model.local_member_is_leader &&
           model.primary.action ==
               MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS &&
           model.secondary.action != MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN &&
           model.status != NULL &&
           strcmp(model.status, "Waiting for the Host") == 0,
           "guest results wait on the host instead of offering REMATCH");
    expect_complete(&model, "guest results copy/control contract is complete");
    input.local_endpoint_id = 10u;
}

static void test_host_config_and_tournament(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrSessionCore session;
    MdkrOnlineLobby lobby;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;
    uint64_t host_cmd = 1u;
    uint64_t guest_cmd = 2u; /* the JOIN consumed command id 1 */
    unsigned round;

    mdkr_session_core_init(&session, 5u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    mdkr_online_lobby_init(&lobby, 101u, 10u, &compat, 1u);
    lobby_join(&lobby, 20u, 1u, &compat);
    session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                    MDKR_ROOM_SELECTING);
    input = input_for(&session, &lobby);
    input.race_admission_enabled = true;

    /* A host-configured single race removes the per-seat track vote step:
     * character + vehicle go straight to Ready. */
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_CONFIG_TRACK,
                         0u, 5u).accepted,
           "leader configures the session track");
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_CHARACTER,
                         0u, 1u).accepted &&
           lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_VEHICLE,
                         0u, 0u).accepted,
           "leader picks racer and vehicle");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_SELECTING &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_READY,
           "a host-configured track skips the track vote straight to Ready");

    /* Trophy Tournament never asks for a track vote either. */
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_MODE,
                         0u, 1u).accepted,
           "leader switches the session to Trophy Tournament");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_READY,
           "tournament mode skips the track vote straight to Ready");

    /* Everyone ready but no cup chosen: Start Race would be refused, so the
     * view names the missing host decision instead of a dead button. */
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_READY,
                         0u, 1u).accepted,
           "leader readies without a cup");
    expect(lobby_command_as(&lobby, 20u, guest_cmd++,
                            MDKR_ONLINE_SET_CHARACTER, 1u, 2u).accepted &&
           lobby_command_as(&lobby, 20u, guest_cmd++,
                            MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted &&
           lobby_command_as(&lobby, 20u, guest_cmd++,
                            MDKR_ONLINE_SET_READY, 1u, 1u).accepted,
           "guest completes selections and readies");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.status != NULL &&
           strcmp(model.status, "Pick a Cup to Start") == 0 &&
           model.primary.action !=
               MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "everyone ready without a cup names the missing host decision");

    /* Choosing the cup clears Ready (expected); re-ready arms Start Race. */
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_CUP,
                         0u, 1u).accepted,
           "leader picks the Snowflake Mountain Cup");
    expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_READY,
                         0u, 1u).accepted &&
           lobby_command_as(&lobby, 20u, guest_cmd++,
                            MDKR_ONLINE_SET_READY, 1u, 1u).accepted,
           "both players re-ready after the cup choice");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_START_RACE,
           "a chosen cup re-arms Start Race for the leader");

    /* Four scheduled rounds; the leader's seat wins every race 9-7. */
    for (round = 0u; round < 4u; round++) {
        if (round != 0u) {
            expect(session_command(&session,
                                   MDKR_SESSION_COMMAND_RETURN_TO_LOBBY,
                                   0u).accepted,
                   "session returns to the lobby between rounds");
            expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_REMATCH,
                                 0u, 0u).accepted,
                   "leader advances the tournament round");
            expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_SET_READY,
                                 0u, 1u).accepted &&
                   lobby_command_as(&lobby, 20u, guest_cmd++,
                                    MDKR_ONLINE_SET_READY, 1u, 1u).accepted,
                   "both players re-ready for the next round");
        }
        expect(lobby_command(&lobby, host_cmd++, MDKR_ONLINE_BEGIN_LOADING,
                             0u, 0x07u).accepted,
               "tournament round begins loading");
        session_command(&session, MDKR_SESSION_COMMAND_SET_ROOM_PHASE,
                        MDKR_ROOM_LOADING);
        expect(session_command(&session, MDKR_SESSION_COMMAND_REQUEST_RACE,
                               0u).accepted,
               "session boots the round's engine loan");
        lobby_command(&lobby, host_cmd++, MDKR_ONLINE_ACK_LOADED, 0u, 0u);
        lobby_command_as(&lobby, 20u, guest_cmd++, MDKR_ONLINE_ACK_LOADED,
                         0u, 0u);
        lobby_command(&lobby, host_cmd++, MDKR_ONLINE_BEGIN_RACE, 0u, 0u);
        session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                        MDKR_ENGINE_READY);
        session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                        MDKR_ENGINE_RACING);
        lobby_command(&lobby, host_cmd++, MDKR_ONLINE_PUBLISH_RESULTS, 0u,
                      0xFFFF0100u);
        session_command(&session, MDKR_SESSION_COMMAND_SET_ENGINE_PHASE,
                        MDKR_ENGINE_FINISHED);

        if (round == 0u) {
            /* Mid-series results: race_index/points-aware copy, leader gets
             * the scheduled Next Race, the guest waits on the host. */
            expect(lobby.race_index == 0u && lobby.points[0] == 9u &&
                   lobby.points[1] == 7u,
                   "authentic DKR points land after round 1");
            expect(mdkr_online_view_model_build(&input, &model) &&
                   model.kind == MDKR_ONLINE_VIEW_RESULTS &&
                   model.primary.action ==
                       MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN &&
                   strcmp(model.primary.label, "Next Race") == 0 &&
                   model.status != NULL &&
                   strcmp(model.status, "Standings Updated") == 0,
                   "leader tournament results advance the scheduled series");
            expect_complete(&model, "tournament results contract is complete");
            input.local_endpoint_id = 20u;
            expect(mdkr_online_view_model_build(&input, &model) &&
                   model.primary.action ==
                       MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS &&
                   model.status != NULL &&
                   strcmp(model.status, "Waiting for the Host") == 0,
                   "guest tournament results wait on the host");
            input.local_endpoint_id = 10u;
        }
    }

    /* Final-round results: the champion is named and the leader can wrap the
     * series into a fresh tournament. */
    expect(lobby.race_index == 3u && lobby.points[0] == 36u &&
           lobby.points[1] == 28u,
           "cumulative points reach the four-round totals");
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RESULTS &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN &&
           strcmp(model.primary.label, "New Tournament") == 0 &&
           model.status != NULL &&
           strcmp(model.status, "You Are the Champion") == 0,
           "final round crowns the leading seat and offers a new tournament");
    input.local_endpoint_id = 20u;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.primary.action ==
               MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS &&
           model.status != NULL &&
           strcmp(model.status, "Your Friend Takes the Trophy") == 0,
           "the guest sees the champion result without a dead rematch");
    input.local_endpoint_id = 10u;

    /* The series wrap: REMATCH after race 4 resets to race 1, zero points. */
    expect(session_command(&session, MDKR_SESSION_COMMAND_RETURN_TO_LOBBY,
                           0u).accepted &&
           lobby_command(&lobby, host_cmd++, MDKR_ONLINE_REMATCH,
                         0u, 0u).accepted &&
           lobby.race_index == 0u && lobby.points[0] == 0u &&
           lobby.points[1] == 0u,
           "New Tournament wraps the series back to race 1 with fresh points");
}

/* subtask 3: pin the join/preflight failure family's REACHABLE view-model
 * contract -- each typed failure's stable primary recovery action plus complete,
 * distinct copy -- for the bad-code / room-full / expired / version-mismatch
 * cases the room drives. The richer per-failure SENTENCES added (the
 * distinct build / ROM / settings / update explanations) live in
 * platform/app/ui_online_room.cpp `betaFailureCopy`, which a pure-C view-model
 * test cannot reach and which this task must not add a hook to (that file is
 * under concurrent review). So this asserts what IS reachable here: the failure
 * enum -> primary-action mapping (the wiring the UI copy layers onto) and that
 * the view model's own title/explanation are present and jargon-free. The UI
 * sentence-level copy is exercised by the gallery render lane
 * (check_browser_online_room_gallery.py) instead. */
static void test_failure_primary_actions_reachable(void) {
    static const struct {
        MdkrOnlineViewFailure failure;
        MdkrOnlineViewAction primary;
        const char *label;
    } cases[] = {
        /* bad join code / invite problems. */
        { MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED,
          MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE, "invite-expired" },
        { MDKR_ONLINE_VIEW_FAILURE_INVITE_ROTATED,
          MDKR_ONLINE_VIEW_ACTION_ENTER_ANOTHER_CODE, "invite-rotated" },
        /* room full / expired / host gone -> stay playable locally. */
        { MDKR_ONLINE_VIEW_FAILURE_ROOM_FULL,
          MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, "room-full" },
        { MDKR_ONLINE_VIEW_FAILURE_ROOM_EXPIRED,
          MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, "room-expired" },
        { MDKR_ONLINE_VIEW_FAILURE_HOST_CLOSED,
          MDKR_ONLINE_VIEW_ACTION_PLAY_HERE, "host-closed" },
        /* version / content mismatch -> each names its own distinct fix. */
        { MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD,
          MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME, "different-build" },
        { MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM,
          MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM, "different-rom" },
        { MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS,
          MDKR_ONLINE_VIEW_ACTION_USE_ROOM_SETTINGS, "different-settings" },
        { MDKR_ONLINE_VIEW_FAILURE_UPDATE_REQUIRED,
          MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME, "update-required" },
    };
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;
    unsigned index;

    mdkr_session_core_init(&session, 6u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    input = input_for(&session, NULL);

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        input.failure = cases[index].failure;
        expect(mdkr_online_view_model_build(&input, &model) &&
               model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
               model.failure == cases[index].failure &&
               model.primary.action == cases[index].primary,
               cases[index].label);
        expect_complete(&model, cases[index].label);
    }

    /* The mismatch family must not collapse to one generic action: ROM and
     * settings each name their own distinct fix, distinct from the update path. */
    {
        MdkrOnlineViewModel a, b, c;
        input.failure = MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_ROM;
        expect(mdkr_online_view_model_build(&input, &a), "different-rom builds");
        input.failure = MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_SETTINGS;
        expect(mdkr_online_view_model_build(&input, &b),
               "different-settings builds");
        input.failure = MDKR_ONLINE_VIEW_FAILURE_DIFFERENT_BUILD;
        expect(mdkr_online_view_model_build(&input, &c), "different-build builds");
        expect(a.primary.action != b.primary.action &&
               a.primary.action != c.primary.action &&
               b.primary.action != c.primary.action,
               "ROM / settings / build mismatches each get a distinct primary");
    }
    input.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
}

static void test_failure_catalog_and_atomicity(void) {
    static const char *forbidden[] = { "ICE", "STUN", "TURN", "HTTP",
                                       "provider", "quota", "unknown error" };
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;
    unsigned failure;
    unsigned term;

    mdkr_session_core_init(&session, 4u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    input = input_for(&session, NULL);
    for (failure = MDKR_ONLINE_VIEW_FAILURE_INVITE_EXPIRED;
         failure <= MDKR_ONLINE_VIEW_FAILURE_COUNT + 1u; failure++) {
        input.failure = (MdkrOnlineViewFailure)failure;
        memset(&model, 0, sizeof(model));
        expect(mdkr_online_view_model_build(&input, &model),
               "every typed or unrecognized adapter failure projects safely");
        expect_complete(&model, "recovery copy/control contract is complete");
        expect(model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
               model.announcement == MDKR_ONLINE_ANNOUNCE_ASSERTIVE &&
               model.local_play_available && model.timeout.present,
               "recovery is assertive, bounded and preserves local escape");
        for (term = 0u; term < sizeof(forbidden) / sizeof(forbidden[0]); term++) {
            expect(strstr(model.title, forbidden[term]) == NULL &&
                   strstr(model.explanation, forbidden[term]) == NULL,
                   "player copy excludes provider jargon and raw diagnostics");
        }
    }
    input.failure = (MdkrOnlineViewFailure)999;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.failure == MDKR_ONLINE_VIEW_FAILURE_SERVICE_UNAVAILABLE,
           "unrecognized failure fails safely into one actionable copy path");

    {
        MdkrOnlineCompatibilityV1 compat = compatibility();
        MdkrOnlineLobby lobby;
        MdkrOnlineViewModel sentinel;
        mdkr_online_lobby_init(&lobby, 88u, 10u, &compat, 1u);
        input.failure = MDKR_ONLINE_VIEW_FAILURE_NONE;
        input.lobby = &lobby; /* Session is still JOIN/room NONE: mismatched. */
        memset(&sentinel, 0x5a, sizeof(sentinel));
        model = sentinel;
        expect(!mdkr_online_view_model_build(&input, &model) &&
               memcmp(&model, &sentinel, sizeof(model)) == 0,
               "mismatched session/lobby snapshots reject fail-atomically");
    }
}

#if MDKR_ENABLE_ONLINE_BETA
/* The beta online-live engine session routes an abnormal race end to its own
 * recovery card (platform/app/main_app.cpp -> set_race_end_failure). Both cards
 * must read as a named title + plain cause + a Return to Lobby primary, and
 * must NOT reuse the generic CONNECTION_CHECK "could not establish a playable
 * connection" copy. */
static void test_race_scoped_recovery_cards(void) {
    MdkrSessionCore session;
    MdkrOnlineViewInput input;
    MdkrOnlineViewModel model;

    mdkr_session_core_init(&session, 7u);
    session_command(&session, MDKR_SESSION_COMMAND_BEGIN_ONLINE, 0u);
    input = input_for(&session, NULL);

    input.failure = MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
           model.failure == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT &&
           model.title != NULL &&
           strcmp(model.title, "Opponent Disconnected") == 0 &&
           model.explanation != NULL &&
           strstr(model.explanation, "lost connection") != NULL &&
           strstr(model.explanation, "This room is done") != NULL &&
           strstr(model.explanation, "create or join a new one") != NULL &&
           /* Working primary: reuse HOST_CLOSED's client-side room exit, not a
            * dead Return to Lobby the wedged reducer can never honor. */
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE &&
           model.primary.label != NULL &&
           strcmp(model.primary.label, "Play Here") == 0 &&
           !model.secondary.visible,
           "mid-race opponent disconnect is a truthful dead-end with a working "
           "room exit");
    expect_complete(&model,
                    "opponent-left recovery copy/control contract is complete");
    expect(strstr(model.explanation,
                  "could not establish a playable connection") == NULL,
           "opponent-left card is distinct from the CONNECTION_CHECK copy");
    /* Real em dash, never ASCII "--" (the a11y announcer reads it raw). */
    expect(strstr(model.explanation, "--") == NULL,
           "opponent-left copy uses a real em dash, not ASCII hyphens");

    input.failure = MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
           model.failure == MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED &&
           model.title != NULL &&
           strcmp(model.title, "Your Opponent Couldn't Start") == 0 &&
           model.explanation != NULL &&
           strstr(model.explanation, "canceled before it began") != NULL &&
           strstr(model.explanation, "fresh invite") != NULL &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE &&
           model.primary.label != NULL &&
           strcmp(model.primary.label, "Play Here") == 0 &&
           !model.secondary.visible,
           "start-barrier abort is a truthful dead-end with a working room exit");
    expect_complete(
        &model,
        "opponent-never-started recovery copy/control contract is complete");
    expect(strstr(model.explanation,
                  "could not establish a playable connection") == NULL,
           "opponent-never-started card is distinct from CONNECTION_CHECK copy");

    input.failure = MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE;
    expect(mdkr_online_view_model_build(&input, &model) &&
           model.kind == MDKR_ONLINE_VIEW_RECOVERY &&
           model.failure == MDKR_ONLINE_VIEW_FAILURE_CONNECTION_UNPLAYABLE &&
           model.title != NULL &&
           strcmp(model.title, "Connection Became Unplayable") == 0 &&
           model.explanation != NULL &&
           strstr(model.explanation, "degraded past recovery") != NULL &&
           strstr(model.explanation, "This room is done") != NULL &&
           model.primary.action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE &&
           model.primary.label != NULL &&
           strcmp(model.primary.label, "Play Here") == 0 &&
           !model.secondary.visible,
           "mid-race transport breakdown is a truthful dead-end with a working "
           "room exit");
    expect_complete(
        &model,
        "connection-unplayable recovery copy/control contract is complete");
    /* Truthfully distinct from the pre-connection CONNECTION_CHECK copy: a
     * playable connection DID exist, so never "could not establish". */
    expect(strstr(model.explanation,
                  "could not establish a playable connection") == NULL,
           "connection-unplayable card is distinct from CONNECTION_CHECK copy");
    expect(strstr(model.explanation, "--") == NULL,
           "connection-unplayable copy uses a real em dash, not ASCII hyphens");
}
#endif

int main(void) {
    test_entry_connecting_and_timeouts();
    test_room_selection_and_release_gate();
    test_loading_racing_and_results();
    test_host_config_and_tournament();
    test_failure_primary_actions_reachable();
    test_failure_catalog_and_atomicity();
#if MDKR_ENABLE_ONLINE_BETA
    test_race_scoped_recovery_cards();
#endif
    if (failures != 0) {
        fprintf(stderr, "%d online lobby view-model test(s) failed\n", failures);
        return 1;
    }
    puts("online lobby view-model tests passed");
    return 0;
}
