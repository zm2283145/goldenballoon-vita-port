#include "platform/online/lobby_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
    for (index = 0u; index < sizeof(value.build_id); index++)
        value.build_id[index] = (uint8_t)(index + 1u);
    for (index = 0u; index < sizeof(value.gameplay_digest); index++)
        value.gameplay_digest[index] = (uint8_t)(0xa0u + index);
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

static MdkrOnlineCommand command(
    const MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type) {
    MdkrOnlineCommand value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    value.expected_revision = lobby->revision;
    value.command_id = command_id;
    value.actor_endpoint_id = actor;
    value.type = type;
    return value;
}

static const char *phase_name(MdkrOnlinePhase phase) {
    static const char *const names[] = {
        "invalid", "lobby", "loading", "racing", "results", "closed"};
    return phase <= MDKR_ONLINE_CLOSED ? names[phase] : names[0];
}

static const char *error_name(MdkrOnlineError error) {
    static const char *const names[] = {
        "ok", "protocol", "stale_revision", "stale_command",
        "command_conflict", "invalid_state", "unauthorized", "not_found",
        "already_joined", "incompatible", "capacity", "not_ready",
        "disconnected", "selection_conflict", "illegal_vehicle"};
    return error <= MDKR_ONLINE_ERROR_ILLEGAL_VEHICLE ? names[error] : "unknown";
}

static MdkrOnlineCommandType command_type(const char *name) {
    static const char *const names[] = {
        "invalid", "join", "leave", "disconnect", "reconnect", "set_ready",
        "set_vote", "begin_loading", "ack_loaded", "begin_race",
        "publish_results", "rematch", "transfer_leader", "close",
        "set_character", "set_vehicle", "cancel_loading", "set_mode",
        "set_config_track", "set_cup"};
    unsigned index;
    for (index = 1u; index <= MDKR_ONLINE_SET_CUP; index++) {
        if (strcmp(name, names[index]) == 0) return (MdkrOnlineCommandType)index;
    }
    return (MdkrOnlineCommandType)0;
}

static void append_text(char *output, size_t capacity, size_t *used,
                        const char *format, ...) {
    va_list arguments;
    int count;
    if (*used >= capacity) return;
    va_start(arguments, format);
    count = vsnprintf(output + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= capacity - *used) {
        *used = capacity;
        return;
    }
    *used += (size_t)count;
}

static void canonical_parity_state(const MdkrOnlineLobby *lobby,
                                   MdkrOnlineStep result, char *output,
                                   size_t capacity) {
    size_t used = 0u;
    unsigned index;
    bool first = true;
    output[0] = '\0';
    append_text(output, capacity, &used,
                "%u,%u,%u,%s,%u,%u,%s,%llu,%u,",
                result.accepted ? 1u : 0u, result.duplicate ? 1u : 0u,
                result.leader_changed ? 1u : 0u, error_name(result.error),
                lobby->revision, lobby->match_epoch, phase_name(lobby->phase),
                (unsigned long long)lobby->leader_endpoint_id,
                lobby->leader_generation);
    if (lobby->selected_track == MDKR_ONLINE_NO_VOTE)
        append_text(output, capacity, &used, "-,%u,", lobby->selected_vehicle_mask);
    else
        append_text(output, capacity, &used, "%u,%u,", lobby->selected_track,
                    lobby->selected_vehicle_mask);
    for (index = 0u; index < MDKR_ONLINE_MAX_ENDPOINTS; index++) {
        const MdkrOnlineMember *item = &lobby->members[index];
        if (!item->occupied) continue;
        append_text(output, capacity, &used, "%s%llu:%u:%u:%u:%u:%llu",
                    first ? "" : ";", (unsigned long long)item->endpoint_id,
                    item->connected ? 1u : 0u, item->ready ? 1u : 0u,
                    item->loaded ? 1u : 0u, item->seat_count,
                    (unsigned long long)item->last_command_id);
        first = false;
    }
    append_text(output, capacity, &used, ",");
    first = true;
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        const MdkrOnlineSeat *item = &lobby->seats[index];
        if (!item->occupied) continue;
        append_text(output, capacity, &used, "%s%llu:%u:%u:", first ? "" : ";",
                    (unsigned long long)item->endpoint_id, item->local_index,
                    item->selection_revision);
        if (item->vote_track == MDKR_ONLINE_NO_VOTE)
            append_text(output, capacity, &used, "-:");
        else
            append_text(output, capacity, &used, "%u:", item->vote_track);
        if (item->character_id == MDKR_ONLINE_NO_CHARACTER)
            append_text(output, capacity, &used, "-:");
        else
            append_text(output, capacity, &used, "%u:", item->character_id);
        if (item->vehicle_id == MDKR_ONLINE_NO_VEHICLE)
            append_text(output, capacity, &used, "-");
        else
            append_text(output, capacity, &used, "%u", item->vehicle_id);
        first = false;
    }
    append_text(output, capacity, &used, ",%u:", lobby->mode);
    if (lobby->configured_track == MDKR_ONLINE_NO_VOTE)
        append_text(output, capacity, &used, "-:");
    else
        append_text(output, capacity, &used, "%u:", lobby->configured_track);
    if (lobby->cup_id == MDKR_ONLINE_NO_CUP)
        append_text(output, capacity, &used, "-:%u", lobby->race_index);
    else
        append_text(output, capacity, &used, "%u:%u", lobby->cup_id,
                    lobby->race_index);
    append_text(output, capacity, &used, ",");
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++)
        append_text(output, capacity, &used, "%s%u", index == 0u ? "" : ":",
                    lobby->points[index]);
    append_text(output, capacity, &used, ",");
    for (index = 0u; index < MDKR_ONLINE_MAX_SEATS; index++) {
        if (lobby->last_placements[index] == MDKR_ONLINE_NO_PLACEMENT)
            append_text(output, capacity, &used, "%s-", index == 0u ? "" : ":");
        else
            append_text(output, capacity, &used, "%s%u", index == 0u ? "" : ":",
                        lobby->last_placements[index]);
    }
}

static unsigned split_parity_fields(char *line, char *fields[], unsigned max) {
    char *cursor = line;
    char *tab;
    unsigned field_count = 0u;
    while (field_count < max) {
        fields[field_count++] = cursor;
        tab = strchr(cursor, '\t');
        if (tab == NULL) break;
        *tab = '\0';
        cursor = tab + 1;
    }
    return field_count;
}

static MdkrOnlineStep replay_parity_command(
    MdkrOnlineLobby *lobby, const MdkrOnlineCompatibilityV1 *compat,
    char *const fields[]) {
    MdkrOnlineCommand value = command(
        lobby, strtoull(fields[2], NULL, 10), strtoull(fields[3], NULL, 10),
        command_type(fields[1]));
    value.expected_revision = (uint32_t)strtoul(fields[4], NULL, 10);
    value.value = (uint32_t)strtoul(fields[5], NULL, 10);
    value.target_endpoint_id = strtoull(fields[6], NULL, 10);
    if (strcmp(fields[7], "same") == 0) value.compatibility = *compat;
    else if (strcmp(fields[7], "unsupported_rom") == 0) {
        value.compatibility = *compat;
        value.compatibility.rom_revision = 3u;
    } else if (strcmp(fields[7], "mismatch") == 0) {
        value.compatibility = *compat;
        value.compatibility.gameplay_digest[3] ^= 1u;
    }
    return mdkr_online_lobby_dispatch(lobby, &value);
}

static void test_shared_service_parity_trace(void) {
    const char *path = MDKR_SOURCE_DIR "/tests/fixtures/online_lobby_reducer_v1.tsv";
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    FILE *file = fopen(path, "rb");
    char line[4096];
    unsigned line_number = 0u;
    expect(file != NULL, "shared reducer parity trace opens");
    if (file == NULL) return;
    expect(mdkr_online_lobby_init(&lobby, 42u, 100u, &compat, 1u),
           "shared reducer parity lobby initializes");
    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[9];
        unsigned field_count;
        MdkrOnlineStep result;
        char actual[2048];
        char message[256];
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        field_count = split_parity_fields(line, fields, 9u);
        snprintf(message, sizeof(message), "parity trace line %u has nine fields",
                 line_number);
        expect(field_count == 9u && strchr(fields[8], '\t') == NULL, message);
        if (field_count != 9u) continue;
        result = replay_parity_command(&lobby, &compat, fields);
        canonical_parity_state(&lobby, result, actual, sizeof(actual));
        snprintf(message, sizeof(message), "shared parity row %s matches", fields[0]);
        expect(strcmp(actual, fields[8]) == 0, message);
        if (strcmp(actual, fields[8]) != 0)
            fprintf(stderr, "  expected: %s\n  actual:   %s\n", fields[8], actual);
    }
    fclose(file);
}

/* `mdkr_online_lobby_core_test --rewrite-parity-trace` regenerates the ninth
 * (expected canonical result/state) column of the shared C/TypeScript fixture
 * from the native reducer, preserving every input column and comment. The
 * command script stays hand-authored; the reducer owns the expected truth. */
static int rewrite_parity_trace(void) {
    const char *path = MDKR_SOURCE_DIR "/tests/fixtures/online_lobby_reducer_v1.tsv";
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    FILE *file = fopen(path, "rb");
    char *content;
    char *cursor;
    long size;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        if (file != NULL) fclose(file);
        return 1;
    }
    content = malloc((size_t)size + 1u);
    if (content == NULL || fread(content, 1u, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "FAIL: cannot load %s\n", path);
        fclose(file);
        free(content);
        return 1;
    }
    fclose(file);
    content[size] = '\0';
    if (!mdkr_online_lobby_init(&lobby, 42u, 100u, &compat, 1u)) {
        fprintf(stderr, "FAIL: parity lobby does not initialize\n");
        free(content);
        return 1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "FAIL: cannot rewrite %s\n", path);
        free(content);
        return 1;
    }
    cursor = content;
    while (*cursor != '\0') {
        char *line = cursor;
        char *fields[9];
        char *newline = strchr(cursor, '\n');
        unsigned field_count;
        MdkrOnlineStep result;
        char actual[2048];
        if (newline != NULL) *newline = '\0';
        cursor = newline != NULL ? newline + 1 : cursor + strlen(cursor);
        line[strcspn(line, "\r")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            fprintf(file, "%s\n", line);
            continue;
        }
        field_count = split_parity_fields(line, fields, 9u);
        if (field_count < 8u) {
            fprintf(stderr, "FAIL: parity row %s lacks its input columns\n",
                    fields[0]);
            fclose(file);
            free(content);
            return 1;
        }
        result = replay_parity_command(&lobby, &compat, fields);
        canonical_parity_state(&lobby, result, actual, sizeof(actual));
        fprintf(file, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", fields[0],
                fields[1], fields[2], fields[3], fields[4], fields[5],
                fields[6], fields[7], actual);
    }
    fclose(file);
    free(content);
    printf("rewrote %s\n", path);
    return 0;
}

static MdkrOnlineStep join(
    MdkrOnlineLobby *lobby, uint64_t actor, unsigned seats,
    const MdkrOnlineCompatibilityV1 *compat) {
    MdkrOnlineCommand value = command(lobby, actor, 1u, MDKR_ONLINE_JOIN);
    value.value = seats;
    value.compatibility = *compat;
    return mdkr_online_lobby_dispatch(lobby, &value);
}

static MdkrOnlineStep select_value(
    MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type, unsigned seat, unsigned selected) {
    MdkrOnlineCommand value = command(lobby, actor, command_id, type);
    value.target_endpoint_id = seat;
    value.value = selected;
    return mdkr_online_lobby_dispatch(lobby, &value);
}

static void test_service_fingerprint_vector(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;

    expect(mdkr_online_lobby_init(&lobby, 1u, 100u, &compat, 1u),
           "fingerprint fixture creates a lobby");
    value = command(&lobby, 100u, 1u, MDKR_ONLINE_SET_CHARACTER);
    value.target_endpoint_id = 0u;
    value.value = 0u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "fingerprint fixture command is accepted");
    expect(lobby.members[0].last_command_fingerprint ==
               UINT64_C(1764495763471733581),
           "non-join command fingerprint matches the service reducer vector");
}

static void test_lifecycle_and_votes(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    expect(mdkr_online_lobby_init(&lobby, 0x1234u, 10u, &compat, 1u),
           "leader creates a private lobby");
    expect(mdkr_online_lobby_valid(&lobby), "initial lobby validates");
    expect(join(&lobby, 20u, 1u, &compat).accepted,
           "compatible endpoint joins");

    expect(select_value(
               &lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 9u).accepted &&
           select_value(
               &lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 1u).accepted,
           "leader selects a unique character and hovercraft");
    expect(select_value(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 7u).accepted &&
           select_value(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 1u).accepted,
           "peer selects a unique character and hovercraft");

    value = command(&lobby, 10u, 3u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader seat votes");
    value = command(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 1u;
    value.value = 7u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer seat votes");

    value = command(&lobby, 10u, 4u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader becomes ready");
    value = command(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer becomes ready");

    value = command(&lobby, 20u, 6u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 0x06u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "only the leader can begin loading");
    value = command(&lobby, 10u, 5u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 0x06u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.phase == MDKR_ONLINE_LOADING &&
           lobby.match_epoch == 1u &&
           (lobby.selected_track == 5u || lobby.selected_track == 7u),
           "ready lobby selects a deterministic vote and enters loading");

    value = command(&lobby, 10u, 6u, MDKR_ONLINE_ACK_LOADED);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "leader acknowledges load");
    value = command(&lobby, 20u, 7u, MDKR_ONLINE_ACK_LOADED);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "peer acknowledges load");
    value = command(&lobby, 10u, 7u, MDKR_ONLINE_BEGIN_RACE);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_RACING,
           "leader starts only after every load acknowledgement");
    value = command(&lobby, 10u, 8u, MDKR_ONLINE_PUBLISH_RESULTS);
    value.value = UINT32_C(0xFFFF0100);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_RESULTS,
           "results are an explicit leader transition");
    value = command(&lobby, 10u, 9u, MDKR_ONLINE_REMATCH);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.phase == MDKR_ONLINE_LOBBY &&
           lobby.match_epoch == 1u && lobby.selected_track == MDKR_ONLINE_NO_VOTE &&
           lobby.selected_vehicle_mask == 0u &&
           !lobby.members[0].ready && !lobby.members[1].ready,
           "rematch preserves room/epoch history but clears round readiness");
}

static void test_atomic_compatibility_capacity_and_idempotency(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    expect(mdkr_online_lobby_init(&lobby, 9u, 100u, &compat, 2u),
           "two-seat leader lobby initializes");
    before = lobby;
    value = command(&lobby, 200u, 1u, MDKR_ONLINE_JOIN);
    value.value = 1u;
    value.compatibility = compat;
    value.compatibility.gameplay_digest[3] ^= 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INCOMPATIBLE &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "gameplay mismatch rejects atomically");

    expect(join(&lobby, 200u, 2u, &compat).accepted,
           "second two-seat endpoint fills room");
    before = lobby;
    step = join(&lobby, 300u, 1u, &compat);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_CAPACITY &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "seat overflow rejects atomically");

    value = command(&lobby, 100u, 1u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted, "new command accepted");
    before = lobby;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.duplicate &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "exact retry returns cached success without a second mutation");
    value.value = 6u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_COMMAND_CONFLICT &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "same id with different payload fails closed");
    value.command_id = 2u;
    value.expected_revision = lobby.revision - 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_REVISION,
           "compare-and-swap revision rejects stale concurrent UI state");
    value.command_id = 3u;
    value.expected_revision = lobby.revision;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "higher command id advances the actor high-water mark");
    before = lobby;
    value.command_id = 2u;
    value.expected_revision = lobby.revision - 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_REVISION &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "stale revision wins over an unseen lower command id");
    value.expected_revision = lobby.revision;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_STALE_COMMAND &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "lower command id is stale only at the current revision");
}

static void test_disconnect_and_leader_custody(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 77u, 50u, &compat, 1u);
    join(&lobby, 20u, 1u, &compat);
    value = command(&lobby, 20u, 2u, MDKR_ONLINE_DISCONNECT);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.seat_count == 2u && !lobby.members[1].connected,
           "disconnect preserves canonical seat custody");
    before = lobby;
    value = command(&lobby, 20u, 3u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_DISCONNECTED &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "disconnected member cannot mutate lobby");
    value = command(&lobby, 20u, 3u, MDKR_ONLINE_RECONNECT);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.members[1].connected && !lobby.members[1].ready,
           "reconnect restores membership but requires fresh readiness");

    value = command(&lobby, 50u, 1u, MDKR_ONLINE_TRANSFER_LEADER);
    value.target_endpoint_id = 20u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.leader_changed &&
           lobby.leader_endpoint_id == 20u && lobby.leader_generation == 2u,
           "leader custody transfer is explicit and generation tracked");
    value = command(&lobby, 50u, 2u, MDKR_ONLINE_LEAVE);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.member_count == 1u && lobby.seat_count == 1u,
           "nonleader leave releases only its own seats");
    before = lobby;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.duplicate &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "leave retry survives member removal through receipt window");
}

static void test_validator_positive_controls(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    mdkr_online_lobby_init(&lobby, 1u, 10u, &compat, 2u);
    expect(mdkr_online_lobby_valid(&lobby), "validator accepts real lobby");
    lobby.seats[1].local_index = 0u;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects duplicate endpoint-local seat index");
}

static void test_loading_cancel_is_leader_owned_and_repeatable(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 66u, 10u, &compat, 1u);
    join(&lobby, 20u, 1u, &compat);
    select_value(&lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 1u);
    select_value(&lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u);
    select_value(&lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 2u);
    select_value(&lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u);
    value = command(&lobby, 10u, 3u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 0u;
    value.value = 5u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 20u, 4u, MDKR_ONLINE_SET_VOTE);
    value.target_endpoint_id = 1u;
    value.value = 5u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 10u, 4u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 20u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    mdkr_online_lobby_dispatch(&lobby, &value);
    value = command(&lobby, 10u, 5u, MDKR_ONLINE_BEGIN_LOADING);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted,
           "cancel fixture reaches loading");

    value = command(&lobby, 20u, 6u, MDKR_ONLINE_CANCEL_LOADING);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "nonleader cannot cancel the shared loading barrier");
    value = command(&lobby, 10u, 6u, MDKR_ONLINE_CANCEL_LOADING);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && lobby.phase == MDKR_ONLINE_LOBBY &&
               !lobby.members[0].ready && !lobby.members[1].ready &&
               lobby.selected_track == MDKR_ONLINE_NO_VOTE,
           "leader cancel returns the intact room to a fresh round");
}

static void test_selection_ownership_conflicts_and_readiness(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;

    mdkr_online_lobby_init(&lobby, 88u, 10u, &compat, 2u);
    join(&lobby, 20u, 1u, &compat);
    before = lobby;
    value = command(&lobby, 10u, 1u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_NOT_READY &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "ready rejects incomplete per-seat selections atomically");

    expect(select_value(
               &lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 9u).accepted &&
           select_value(
               &lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 1u).accepted,
           "first local seat selection is accepted");
    before = lobby;
    step = select_value(
        &lobby, 10u, 3u, MDKR_ONLINE_SET_CHARACTER, 1u, 9u);
    expect(!step.accepted &&
           step.error == MDKR_ONLINE_ERROR_SELECTION_CONFLICT &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "duplicate character rejects atomically");
    expect(select_value(
               &lobby, 10u, 3u, MDKR_ONLINE_SET_CHARACTER, 1u, 8u).accepted &&
           select_value(
               &lobby, 10u, 4u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted &&
           select_value(
               &lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 2u, 7u).accepted &&
           select_value(
               &lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 2u, 2u).accepted,
           "remaining owned seats select unique characters and vehicles");

    before = lobby;
    step = select_value(
        &lobby, 10u, 5u, MDKR_ONLINE_SET_VEHICLE, 2u, 0u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED &&
           memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "endpoint cannot mutate another endpoint's seat");

    value = command(&lobby, 10u, 5u, MDKR_ONLINE_SET_READY);
    value.value = 1u;
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
           lobby.members[0].ready,
           "complete owned selections enable readiness");
    expect(select_value(
               &lobby, 10u, 6u, MDKR_ONLINE_SET_VEHICLE, 0u, 2u).accepted &&
           !lobby.members[0].ready &&
           lobby.seats[0].selection_revision == 3u,
           "selection changes clear readiness and advance seat revision");

    lobby.members[0].ready = true;
    lobby.seats[0].vehicle_id = MDKR_ONLINE_NO_VEHICLE;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects ready state with an incomplete selection");
}

static MdkrOnlineStep simple(
    MdkrOnlineLobby *lobby, uint64_t actor, uint64_t command_id,
    MdkrOnlineCommandType type, uint32_t value) {
    MdkrOnlineCommand item = command(lobby, actor, command_id, type);
    item.value = value;
    return mdkr_online_lobby_dispatch(lobby, &item);
}

/* Leader 10 (seat 0) and guest 20 (seat 1) with complete selections. */
static void build_two_seat_room(
    MdkrOnlineLobby *lobby, const MdkrOnlineCompatibilityV1 *compat,
    uint64_t *host_cid, uint64_t *guest_cid) {
    mdkr_online_lobby_init(lobby, 4242u, 10u, compat, 1u);
    join(lobby, 20u, 1u, compat);
    select_value(lobby, 10u, 1u, MDKR_ONLINE_SET_CHARACTER, 0u, 1u);
    select_value(lobby, 10u, 2u, MDKR_ONLINE_SET_VEHICLE, 0u, 0u);
    select_value(lobby, 20u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 2u);
    select_value(lobby, 20u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u);
    *host_cid = 3u;
    *guest_cid = 4u;
}

static bool run_round_to_racing(
    MdkrOnlineLobby *lobby, uint64_t *host_cid, uint64_t *guest_cid) {
    bool ok = true;
    ok = simple(lobby, 10u, (*host_cid)++, MDKR_ONLINE_SET_READY, 1u).accepted && ok;
    ok = simple(lobby, 20u, (*guest_cid)++, MDKR_ONLINE_SET_READY, 1u).accepted && ok;
    ok = simple(lobby, 10u, (*host_cid)++, MDKR_ONLINE_BEGIN_LOADING, 1u).accepted && ok;
    ok = simple(lobby, 10u, (*host_cid)++, MDKR_ONLINE_ACK_LOADED, 0u).accepted && ok;
    ok = simple(lobby, 20u, (*guest_cid)++, MDKR_ONLINE_ACK_LOADED, 0u).accepted && ok;
    ok = simple(lobby, 10u, (*host_cid)++, MDKR_ONLINE_BEGIN_RACE, 0u).accepted && ok;
    return ok;
}

static void test_host_authority_config_is_leader_owned(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    before = lobby;
    step = simple(&lobby, 20u, guest_cid, MDKR_ONLINE_SET_MODE, 1u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "nonleader cannot set the session mode");
    step = simple(&lobby, 20u, guest_cid, MDKR_ONLINE_SET_CONFIG_TRACK, 3u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED,
           "nonleader cannot configure the track");
    step = simple(&lobby, 20u, guest_cid, MDKR_ONLINE_SET_CUP, 0u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_UNAUTHORIZED &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "nonleader cannot pick a cup and rejections stay atomic");

    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_SET_MODE, 2u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "mode above tournament is rejected");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_SET_CONFIG_TRACK, 256u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "configured track outside the race schedule is rejected");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_SET_CUP, 5u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "cup id above the schedule is rejected atomically");

    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_READY,
                      1u).accepted,
           "config fixture reaches an all-ready lobby");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CONFIG_TRACK, 9u);
    expect(step.accepted && lobby.configured_track == 9u &&
               !lobby.members[0].ready && !lobby.members[1].ready,
           "leader configures a track and readiness is invalidated");

    lobby.points[1] = 7u;
    lobby.last_placements[0] = 1u;
    lobby.race_index = 2u;
    expect(mdkr_online_lobby_valid(&lobby),
           "session config and progress are legal lobby state");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_MODE, 1u);
    expect(step.accepted && lobby.mode == 1u && lobby.race_index == 0u &&
               lobby.points[0] == 0u && lobby.points[1] == 0u &&
               lobby.last_placements[0] == MDKR_ONLINE_NO_PLACEMENT,
           "set mode restarts the series scoreboard");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CUP, 2u);
    expect(step.accepted && lobby.cup_id == 2u && lobby.race_index == 0u &&
               lobby.configured_track == 9u,
           "set cup keeps the single-race track configuration intact");

    lobby.points[0] = MDKR_ONLINE_MAX_TOURNAMENT_POINTS + 1u;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects points beyond a full cup of firsts");
    lobby.points[0] = 0u;
    lobby.last_placements[0] = MDKR_ONLINE_PLACEMENT_COUNT;
    expect(!mdkr_online_lobby_valid(&lobby),
           "validator rejects placements beyond the field size");
}

static void test_begin_loading_prefers_configured_track(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_READY,
                      1u).accepted,
           "configured-track fixture reaches an all-ready lobby");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_BEGIN_LOADING, 1u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_NOT_READY,
           "no votes and no configured track still refuses to load");

    expect(select_value(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_VOTE, 0u,
                        5u).accepted &&
               select_value(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_VOTE, 1u,
                            5u).accepted,
           "both seats vote for the legacy plurality track");
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CONFIG_TRACK,
                  13u).accepted,
           "leader overrides the vote with a configured track");
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_READY,
                      1u).accepted,
           "room re-readies under the configured track");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_BEGIN_LOADING, 1u);
    expect(step.accepted && lobby.selected_track == 13u,
           "host-authority track wins over the seat votes");
}

static void test_publish_results_placement_validation(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CONFIG_TRACK,
                  9u).accepted,
           "placement fixture configures a track");
    expect(run_round_to_racing(&lobby, &host_cid, &guest_cid),
           "placement fixture reaches racing");

    before = lobby;
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF0000));
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "duplicate placements among occupied seats are rejected");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFFFF00));
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "an occupied seat cannot publish the no-placement byte");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF0108));
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "placements beyond the eight-racer field are rejected");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0x00FF0100));
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE &&
               memcmp(&lobby, &before, sizeof(lobby)) == 0,
           "unoccupied seats must publish the no-placement byte");

    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF0100));
    expect(step.accepted && lobby.phase == MDKR_ONLINE_RESULTS &&
               lobby.last_placements[0] == 0u && lobby.last_placements[1] == 1u &&
               lobby.last_placements[2] == MDKR_ONLINE_NO_PLACEMENT &&
               lobby.points[0] == 0u && lobby.points[1] == 0u,
           "single-race results record placements without trophy points");
    step = simple(&lobby, 10u, host_cid++, MDKR_ONLINE_REMATCH, 0u);
    expect(step.accepted && lobby.race_index == 0u &&
               lobby.last_placements[0] == MDKR_ONLINE_NO_PLACEMENT &&
               lobby.last_placements[1] == MDKR_ONLINE_NO_PLACEMENT,
           "single-race rematch clears the last placements");
}

static void test_tournament_cup_progression_and_scoring(void) {
    static const uint16_t dino_domain[MDKR_ONLINE_CUP_ROUNDS] = {5u, 3u, 29u, 7u};
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;
    unsigned round;

    expect(mdkr_online_cup_track(0u, 0u) == 5u &&
               mdkr_online_cup_track(4u, 3u) == 15u &&
               mdkr_online_cup_track(5u, 0u) == MDKR_ONLINE_NO_VOTE &&
               mdkr_online_cup_track(0u, 4u) == MDKR_ONLINE_NO_VOTE,
           "cup schedule accessor exposes the table and bounds it");

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_MODE, 1u).accepted,
           "leader arms tournament mode");
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_READY,
                      1u).accepted,
           "tournament fixture reaches an all-ready lobby");
    step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_BEGIN_LOADING, 1u);
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_NOT_READY,
           "tournament without a cup refuses to load");
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CUP, 0u).accepted &&
               !lobby.members[0].ready && !lobby.members[1].ready,
           "picking the cup invalidates readiness");

    for (round = 0u; round < MDKR_ONLINE_CUP_ROUNDS; round++) {
        char message[128];
        expect(run_round_to_racing(&lobby, &host_cid, &guest_cid),
               "tournament round reaches racing");
        snprintf(message, sizeof(message),
                 "tournament round %u races the cup schedule track", round);
        expect(lobby.race_index == round &&
                   lobby.selected_track == dino_domain[round], message);
        if (round == 0u) {
            step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_SET_MODE, 0u);
            expect(!step.accepted &&
                       step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
                   "session config is frozen outside the lobby phase");
        }
        expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_PUBLISH_RESULTS,
                      UINT32_C(0xFFFF0100)).accepted,
               "tournament round publishes a constant first/second");
        snprintf(message, sizeof(message),
                 "tournament points accrue 9/7 through round %u", round);
        expect(lobby.points[0] == (round + 1u) * 9u &&
                   lobby.points[1] == (round + 1u) * 7u &&
                   lobby.last_placements[0] == 0u &&
                   lobby.last_placements[1] == 1u, message);
        expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_REMATCH, 0u).accepted,
               "tournament rematch returns to the lobby");
        if (round + 1u < MDKR_ONLINE_CUP_ROUNDS) {
            snprintf(message, sizeof(message),
                     "rematch advances the series to race %u", round + 1u);
            expect(lobby.race_index == round + 1u &&
                       lobby.last_placements[0] == 0u &&
                       lobby.points[0] == (round + 1u) * 9u, message);
        }
    }
    expect(lobby.race_index == 0u && lobby.points[0] == 0u &&
               lobby.points[1] == 0u &&
               lobby.last_placements[0] == MDKR_ONLINE_NO_PLACEMENT &&
               lobby.last_placements[1] == MDKR_ONLINE_NO_PLACEMENT,
           "finishing the cup wraps the series back to a fresh scoreboard");
}

/* Finding: seat representation after LEAVE. Any removal that vacates seats
 * must compact the seat array order-preserving and shift points[] and
 * last_placements[] with the same permutation (zeroing the vacated tail) so
 * PUBLISH_RESULTS packed bytes validate and attribute identically in the
 * native and service reducers. */
static void test_leave_compaction_rejoin_and_attribution(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_MODE, 1u).accepted &&
               simple(&lobby, 10u, host_cid++, MDKR_ONLINE_SET_CUP, 0u).accepted,
           "compaction fixture arms a tournament cup");
    expect(run_round_to_racing(&lobby, &host_cid, &guest_cid),
           "compaction fixture reaches racing");
    expect(simple(&lobby, 10u, host_cid++, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF0100)).accepted &&
               lobby.points[0] == 9u && lobby.points[1] == 7u,
           "compaction fixture publishes round one");

    /* Leader 10 (seat 0) leaves during RESULTS: seat 1 shifts down one slot
     * together with its series state, and leadership re-elects by endpoint. */
    value = command(&lobby, 10u, host_cid++, MDKR_ONLINE_LEAVE);
    step = mdkr_online_lobby_dispatch(&lobby, &value);
    expect(step.accepted && step.leader_changed &&
               lobby.leader_endpoint_id == 20u,
           "leader leave in results re-elects the surviving endpoint");
    expect(lobby.seat_count == 1u && lobby.seats[0].occupied &&
               lobby.seats[0].endpoint_id == 20u && !lobby.seats[1].occupied &&
               !lobby.seats[2].occupied && !lobby.seats[3].occupied,
           "leave compacts the seat array order-preserving");
    expect(lobby.points[0] == 7u && lobby.last_placements[0] == 1u,
           "series state shifts with the surviving seat");
    expect(lobby.points[1] == 0u && lobby.points[2] == 0u &&
               lobby.last_placements[1] == MDKR_ONLINE_NO_PLACEMENT &&
               lobby.last_placements[2] == MDKR_ONLINE_NO_PLACEMENT,
           "vacated tail series entries are zeroed");

    expect(simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_REMATCH, 0u).accepted &&
               lobby.race_index == 1u,
           "surviving leader rematches into round two");
    expect(join(&lobby, 30u, 1u, &compat).accepted &&
               lobby.seats[1].occupied && lobby.seats[1].endpoint_id == 30u,
           "fresh endpoint joins mid-tournament onto the compacted tail");
    expect(lobby.points[1] == 0u &&
               lobby.last_placements[1] == MDKR_ONLINE_NO_PLACEMENT,
           "newcomer does not inherit the leaver's series state");
    expect(select_value(
               &lobby, 30u, 2u, MDKR_ONLINE_SET_CHARACTER, 1u, 1u).accepted &&
               select_value(
                   &lobby, 30u, 3u, MDKR_ONLINE_SET_VEHICLE, 1u, 0u).accepted,
           "newcomer completes seat selections");
    expect(simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 30u, 4u, MDKR_ONLINE_SET_READY, 1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_BEGIN_LOADING,
                      1u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_ACK_LOADED,
                      0u).accepted &&
               simple(&lobby, 30u, 5u, MDKR_ONLINE_ACK_LOADED, 0u).accepted &&
               simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_BEGIN_RACE,
                      0u).accepted,
           "post-rejoin round reaches racing");
    step = simple(&lobby, 20u, guest_cid, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF01FF));
    expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE,
           "packed bytes shaped for the old in-place seat hole are refused");
    expect(simple(&lobby, 20u, guest_cid++, MDKR_ONLINE_PUBLISH_RESULTS,
                  UINT32_C(0xFFFF0100)).accepted,
           "post-rejoin publish validates against the compacted seats");
    expect(lobby.points[0] == 16u && lobby.points[1] == 7u &&
               lobby.last_placements[0] == 0u && lobby.last_placements[1] == 1u,
           "trophy points attribute to the compacted seat order");
}

static void test_leave_in_lobby_compacts_middle_seat(void) {
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineCommand value;

    mdkr_online_lobby_init(&lobby, 5u, 10u, &compat, 1u);
    join(&lobby, 20u, 1u, &compat);
    join(&lobby, 30u, 1u, &compat);
    expect(select_value(
               &lobby, 30u, 2u, MDKR_ONLINE_SET_CHARACTER, 2u, 3u).accepted,
           "third seat marks itself before the middle seat leaves");
    value = command(&lobby, 20u, 2u, MDKR_ONLINE_LEAVE);
    expect(mdkr_online_lobby_dispatch(&lobby, &value).accepted &&
               lobby.seat_count == 2u &&
               lobby.seats[0].endpoint_id == 10u &&
               lobby.seats[1].occupied && lobby.seats[1].endpoint_id == 30u &&
               lobby.seats[1].character_id == 3u && !lobby.seats[2].occupied,
           "lobby-phase leave shifts later seats down order-preserving");
}

/* Finding: SET_CONFIG_TRACK accepted any value <= 255, letting a hostile
 * leader configure a hub/cutscene id whose failure only surfaced at engine
 * boot. Only the 20 race tracks of the cup schedule are configurable. */
static void test_config_track_requires_known_race_track(void) {
    static const uint32_t hostile_ids[] = {0u, 34u, 26u};
    static const char *const hostile_names[] = {
        "hub id 0", "trophy-race id 34", "battle id 26"};
    MdkrOnlineCompatibilityV1 compat = compatibility();
    MdkrOnlineLobby lobby;
    MdkrOnlineLobby before;
    MdkrOnlineStep step;
    uint64_t host_cid;
    uint64_t guest_cid;
    unsigned cup;
    unsigned round;
    unsigned index;

    build_two_seat_room(&lobby, &compat, &host_cid, &guest_cid);
    before = lobby;
    for (index = 0u; index < 3u; index++) {
        char message[128];
        step = simple(&lobby, 10u, host_cid, MDKR_ONLINE_SET_CONFIG_TRACK,
                      hostile_ids[index]);
        snprintf(message, sizeof(message),
                 "config track refuses %s atomically", hostile_names[index]);
        expect(!step.accepted && step.error == MDKR_ONLINE_ERROR_INVALID_STATE &&
                   memcmp(&lobby, &before, sizeof(lobby)) == 0, message);
    }
    for (cup = 0u; cup < MDKR_ONLINE_CUP_COUNT; cup++) {
        for (round = 0u; round < MDKR_ONLINE_CUP_ROUNDS; round++) {
            char message[128];
            uint16_t track = mdkr_online_cup_track(cup, round);
            step = simple(&lobby, 10u, host_cid++,
                          MDKR_ONLINE_SET_CONFIG_TRACK, track);
            snprintf(message, sizeof(message),
                     "config track accepts race track %u", track);
            expect(step.accepted && lobby.configured_track == track, message);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--rewrite-parity-trace") == 0)
        return rewrite_parity_trace();
    test_shared_service_parity_trace();
    test_service_fingerprint_vector();
    test_lifecycle_and_votes();
    test_atomic_compatibility_capacity_and_idempotency();
    test_disconnect_and_leader_custody();
    test_validator_positive_controls();
    test_loading_cancel_is_leader_owned_and_repeatable();
    test_selection_ownership_conflicts_and_readiness();
    test_host_authority_config_is_leader_owned();
    test_begin_loading_prefers_configured_track();
    test_publish_results_placement_validation();
    test_tournament_cup_progression_and_scoring();
    test_leave_compaction_rejoin_and_attribution();
    test_leave_in_lobby_compacts_middle_seat();
    test_config_track_requires_known_race_track();
    if (failures != 0) return 1;
    puts("online lobby core contract passed");
    return 0;
}
