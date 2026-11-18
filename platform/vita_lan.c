#include "vita_lan.h"

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)

#include <stdio.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "net/match_input_bundle.h"
#include "net/match_input_repair.h"
#include "net/match_input_runtime.h"
#include "net/match_transport.h"
#include "net/net_roster_runtime.h"
#include "game.h"
#include "online/online_race_pause.h"
#include "menu.h"
#include "session/session_bridge.h"
#include "vita_network_runtime.h"
#include "rollback/rollback_game_runtime.h"
#if defined(MDKR_ENABLE_VITA_RELAY)
#include "vita_relay.h"
#endif

#ifndef MDKR_VERSION_STRING
#define MDKR_VERSION_STRING "dev"
#endif

extern void mdkr_vita_boot_log(const char *message);
extern void mdkr_vita_boot_log_flush(void);
extern int platform_sim_tick_fields(void);

#define MDKR_LAN_PORT 31416
#define MDKR_LAN_PACKET_SIZE 40
#define MDKR_LAN_PACKET_HEADER_SIZE 16
#define MDKR_LAN_MAX_PACKET_SIZE \
    (MDKR_LAN_PACKET_HEADER_SIZE + MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES)
#define MDKR_LAN_PROTOCOL_VERSION 6
#define MDKR_LAN_SEND_INTERVAL_US 500000ULL
#define MDKR_LAN_START_INTERVAL_US 100000ULL
#define MDKR_LAN_START_DELAY_US 500000ULL
#define MDKR_LAN_CONTROL_DELAY_US 600000ULL
#define MDKR_LAN_CONTROL_INTERVAL_US 50000ULL
#define MDKR_LAN_FINISH_SETTLE_US 500000ULL
#define MDKR_LAN_CURSOR_INTERVAL_US 100000ULL
#define MDKR_LAN_CHARACTER_INTERVAL_US 100000ULL
#define MDKR_LAN_REPAIR_REASK_TICKS 8u
#define MDKR_LAN_REPAIR_REASK_US 250000ULL
#define MDKR_LAN_PREDICTION_STALL_TICKS \
    (MDKR_MATCH_INPUT_BUNDLE_FRAMES + 1u)
#define MDKR_LAN_RELAY_PREDICTION_STALL_TICKS \
    (MDKR_MATCH_INPUT_BUNDLE_FRAMES + 1u)
#define MDKR_LAN_DISCOVERY_INTERVAL_US 1000000ULL
#define MDKR_LAN_ROOM_TIMEOUT_US 3000000ULL
#define MDKR_LAN_PEER_TIMEOUT_US 5000000ULL
#define MDKR_LAN_MAX_RECEIVES_PER_UPDATE 12
#define MDKR_LAN_MAX_ROOMS 3
#define MDKR_LAN_ROOM_NAME_BYTES 16
#define MDKR_LAN_CONTROL_PAYLOAD_BYTES 16
/* epoch(4) tick(4) semantic(8) rng_seed rng_draws input world (4x4)
 * racer[8](32) object_count(2) racer_count(1) confirmed(1) validated(4) */
#define MDKR_LAN_DIGEST_PAYLOAD_BYTES 72u
#define MDKR_LAN_DIGEST_STRIDE 1u
#define MDKR_LAN_DIGEST_SEND_BUDGET 4u
#define MDKR_LAN_DIGEST_LOG_LIMIT 24u
#define MDKR_LAN_DEFERRED_INPUT_BUNDLES \
    MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS
#define MDKR_LAN_FAILURE_LOG_PATH \
    "ux0:data/goldenballoon/mdkr_network_failure.log"

#if defined(MDKR_ENABLE_VITA_RELAY)
_Static_assert(MDKR_LAN_MAX_PACKET_SIZE <= MDKR_VITA_RELAY_MAX_PAYLOAD,
               "LAN packets must fit in one relay payload");
#endif

enum MdkrLanControlWireType {
    MDKR_LAN_CONTROL_WIRE_CURSOR = 1,
    MDKR_LAN_CONTROL_WIRE_CURSOR_CLOSE = 2,
    MDKR_LAN_CONTROL_WIRE_RESTART = 3,
    MDKR_LAN_CONTROL_WIRE_SELECT_TRACK = 4,
    MDKR_LAN_CONTROL_WIRE_QUIT = 5,
    MDKR_LAN_CONTROL_WIRE_FINISH_READY = 6
};

enum MdkrLanPacketType {
    MDKR_LAN_PACKET_DISCOVER = 1,
    MDKR_LAN_PACKET_OFFER = 2,
    MDKR_LAN_PACKET_JOIN = 3,
    MDKR_LAN_PACKET_ACCEPT = 4,
    MDKR_LAN_PACKET_PING = 5,
    MDKR_LAN_PACKET_ACK = 6,
    MDKR_LAN_PACKET_SESSION = 7,
    MDKR_LAN_PACKET_READY = 8,
    MDKR_LAN_PACKET_ARM = 9,
    MDKR_LAN_PACKET_ARM_ACK = 10,
    MDKR_LAN_PACKET_GO = 11,
    MDKR_LAN_PACKET_INPUT = 12,
    MDKR_LAN_PACKET_CONTROL = 13,
    MDKR_LAN_PACKET_CHARACTER = 14,
    MDKR_LAN_PACKET_INPUT_REPAIR = 15,
    /* Diagnostic-only final-tick state digest; UDP carrier only. */
    MDKR_LAN_PACKET_STATE_DIGEST = 16
};

enum MdkrLanRole {
    MDKR_LAN_ROLE_NONE = 0,
    MDKR_LAN_ROLE_BROWSER,
    MDKR_LAN_ROLE_HOST
};

enum MdkrLanCarrier {
    MDKR_LAN_CARRIER_NONE = 0,
    MDKR_LAN_CARRIER_UDP,
    MDKR_LAN_CARRIER_RELAY
};

typedef struct MdkrLanRoom {
    SceNetSockaddrIn address;
    unsigned int roomId;
    unsigned long long lastSeenTime;
    char label[32];
} MdkrLanRoom;

static const unsigned char s_packetMagic[4] = { 'D', 'K', 'R', 'L' };
static char s_roomName[MDKR_LAN_ROOM_NAME_BYTES] = "GOLDEN ROOM";

static MdkrVitaLanState s_state;
static enum MdkrLanRole s_role;
static enum MdkrLanCarrier s_carrier;
static char s_status[48] = "LAN";
static char s_roomLabel[48] = "SEARCHING...";
static int s_socket = -1;
static int s_networkOwned;
static SceNetSockaddrIn s_roomAddress;
static SceNetSockaddrIn s_peerAddress;
static MdkrLanRoom s_rooms[MDKR_LAN_MAX_ROOMS];
static unsigned int s_roomCount;
static unsigned int s_roomId;
static unsigned int s_sequence;
static unsigned long long s_lastSendTime;
static unsigned long long s_lastReceiveTime;
static SceWChar16 s_imeTitle[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static SceWChar16 s_imeInitial[MDKR_LAN_ROOM_NAME_BYTES];
static SceWChar16 s_imeOutput[MDKR_LAN_ROOM_NAME_BYTES];
static int s_imeModuleOwned;
static int s_imeTerminated;
static int s_imeApiResult;
static int s_imeDialogResult;
static int s_imeButton;
static MdkrMatchLaunchDescriptorV1 s_launch;
static MdkrSessionBridge s_matchBridge;
static MdkrMatchTransport s_matchTransport;
static unsigned long long s_startTime;
static unsigned long long s_descriptorDigest;
static unsigned int s_firstRaceTick = 1;
static unsigned char s_localSlotMask;
static unsigned char s_remoteSlotMask;
static int s_haveLaunch;
static int s_armReceived;
static int s_finalSessionReady;
static int s_startAcknowledged;
static int s_startScheduled;
static int s_matchRuntimeOwned;
static int s_raceLaunchPending;
static MdkrVitaLanFailure s_failure;
static int s_failurePending;
static char s_failureMessage[128];
static MdkrVitaLanControl s_control;
static unsigned int s_controlTrackId;
static unsigned int s_controlVehicleMask;
static unsigned int s_controlMatchEpoch;
static unsigned int s_controlIdCounter;
static unsigned int s_controlId;
static unsigned int s_lastReceivedControlId;
static unsigned long long s_controlExecuteTime;
static unsigned long long s_lastControlSendTime;
static int s_controlPending;
static unsigned int s_finishLocalTick;
static unsigned int s_finishRemoteTick;
static unsigned long long s_finishLocalReadyTime;
static unsigned long long s_finishRemoteReadyTime;
static unsigned long long s_lastFinishSendTime;
static int s_finishComplete;
static int s_trackPickerActive;
static unsigned int s_trackPickerTrackId;
static unsigned int s_inputSendLogCount;
static unsigned int s_inputReceiveLogCount;
static unsigned int s_inputRejectLogCount;
typedef struct MdkrLanDeferredInputBundle {
    MdkrMatchInputBundle bundle;
    unsigned int sequence;
} MdkrLanDeferredInputBundle;
static MdkrLanDeferredInputBundle
    s_deferredInputBundles[MDKR_LAN_DEFERRED_INPUT_BUNDLES];
static unsigned int s_deferredInputBundleCount;
static unsigned int s_deferredInputLogCount;
static MdkrMatchInputRepairBudget s_repairAnswerBudget;
static unsigned char s_repairRequested[MDKR_NET_INPUT_SLOTS];
static unsigned int s_repairRequestedTick[MDKR_NET_INPUT_SLOTS];
static unsigned int s_repairRequestedAtTick[MDKR_NET_INPUT_SLOTS];
static unsigned long long s_repairRequestedAtTime[MDKR_NET_INPUT_SLOTS];
static unsigned int s_repairLogCount;
static int s_raceInputStalled;
/* Peer state-digest witness (diagnostic only; UDP carrier only). Each side
 * sends the digest of every tick that rollback can no longer rewrite and logs
 * the first tick whose peer digest differs. Never feeds the simulation. */
typedef struct MdkrLanDigestSlot {
    MdkrRollbackTickDigest digest;
    unsigned int validated;
    int valid;
} MdkrLanDigestSlot;
static MdkrLanDigestSlot s_digestLocal[MDKR_ROLLBACK_DIGEST_SLOTS];
static MdkrLanDigestSlot s_digestRemote[MDKR_ROLLBACK_DIGEST_SLOTS];
static unsigned int s_digestNextTick = 1;
static unsigned int s_digestResetValidated;
static int s_digestArmed;
static int s_digestDiverged;
static unsigned int s_digestFirstDivergence;
static unsigned int s_digestMatched;
static unsigned int s_digestMismatched;
static unsigned int s_digestLogCount;
static unsigned int s_raceStallLogCount;
static unsigned char s_character[2];
static unsigned char s_characterConfirmed[2];
static unsigned int s_characterRevision[2];
static unsigned char s_frontendStage;
static unsigned char s_frontendSelection;
static unsigned char s_characterUnlockMask;
static int s_characterUnlocksReady;
static unsigned long long s_lastCharacterSendTime;

static void reset_character_selection(void);

static void reset_finish_barrier(void) {
    s_finishLocalTick = 0;
    s_finishRemoteTick = 0;
    s_finishLocalReadyTime = 0;
    s_finishRemoteReadyTime = 0;
    s_lastFinishSendTime = 0;
    s_finishComplete = 0;
}

static void reset_input_repair(void) {
    memset(&s_repairAnswerBudget, 0, sizeof(s_repairAnswerBudget));
    memset(s_repairRequested, 0, sizeof(s_repairRequested));
    memset(s_repairRequestedTick, 0, sizeof(s_repairRequestedTick));
    memset(s_repairRequestedAtTick, 0, sizeof(s_repairRequestedAtTick));
    memset(s_repairRequestedAtTime, 0, sizeof(s_repairRequestedAtTime));
    s_repairLogCount = 0;
    s_raceInputStalled = 0;
    s_raceStallLogCount = 0;
}

static void log_result(const char *stage, int result) {
    char line[96];
    snprintf(line, sizeof(line), "lan: %s result=0x%08X", stage,
             (unsigned int)result);
    mdkr_vita_boot_log(line);
    mdkr_vita_boot_log_flush();
}

static const char *control_wire_name(unsigned char wireType) {
    switch (wireType) {
        case MDKR_LAN_CONTROL_WIRE_CURSOR:
            return "CURSOR";
        case MDKR_LAN_CONTROL_WIRE_CURSOR_CLOSE:
            return "CURSOR_CLOSE";
        case MDKR_LAN_CONTROL_WIRE_RESTART:
            return "RESTART";
        case MDKR_LAN_CONTROL_WIRE_SELECT_TRACK:
            return "SELECT_TRACK";
        case MDKR_LAN_CONTROL_WIRE_QUIT:
            return "QUIT";
        case MDKR_LAN_CONTROL_WIRE_FINISH_READY:
            return "FINISH_READY";
        default:
            return "UNKNOWN";
    }
}

static void log_control(const char *stage, unsigned char wireType) {
    char line[96];
    snprintf(line, sizeof(line), "lan: %s control=%s wire=0x%02X", stage,
             control_wire_name(wireType), (unsigned int)wireType);
    mdkr_vita_boot_log(line);
    mdkr_vita_boot_log_flush();
}

static void persist_failure_detail(MdkrVitaLanFailure failure,
                                   const char *summary,
                                   const char *detail) {
    char line[256];
    FILE *file;

    snprintf(line, sizeof(line), "lan: failure=%u %.40s detail=%.176s",
             (unsigned int)failure, summary,
             detail != NULL ? detail : "");
    mdkr_vita_boot_log(line);
    mdkr_vita_boot_log_flush();

    file = fopen(MDKR_LAN_FAILURE_LOG_PATH, "w");
    if (file != NULL) {
        fprintf(file, "failure=%u\n%.40s\n%.176s\n",
                (unsigned int)failure, summary,
                detail != NULL ? detail : "");
        fclose(file);
    }
}

static int socket_error(void) {
    int *error = sceNetErrnoLoc();
    return error != NULL ? -*error : -1;
}

static const char *failure_message(MdkrVitaLanFailure failure) {
    switch (failure) {
        case MDKR_VITA_LAN_FAILURE_HOST_LEFT:
            return "HOST LEFT";
        case MDKR_VITA_LAN_FAILURE_PEER_DISCONNECTED:
            return "PEER DISCONNECTED";
        case MDKR_VITA_LAN_FAILURE_VERSION_MISMATCH:
            return "VERSION MISMATCH";
        case MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION:
            return "SYNCHRONIZATION LOST";
        case MDKR_VITA_LAN_FAILURE_STARTUP:
            return "MULTIPLAYER START FAILED";
        case MDKR_VITA_LAN_FAILURE_GENERIC:
            return "MULTIPLAYER ERROR";
        default:
            return "MULTIPLAYER ENDED";
    }
}

static const char *recovery_reason_name(MdkrMatchRecoveryReason reason) {
    switch (reason) {
        case MDKR_MATCH_RECOVERY_INPUT_GAP:
            return "INPUT GAP";
        case MDKR_MATCH_RECOVERY_LATE_INPUT:
            return "LATE INPUT";
        default:
            return "UNKNOWN";
    }
}

static void set_carrier_status(const char *lanStatus,
                               const char *relayStatus) {
    snprintf(s_status, sizeof(s_status), "%s",
             s_carrier == MDKR_LAN_CARRIER_RELAY
                 ? relayStatus
                 : lanStatus);
}

static int ensure_app_util(void) {
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    int value;

    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                                    &value) >= 0) {
        return 0;
    }
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    return sceAppUtilInit(&init, &boot);
}

static void ascii_to_wide(const char *source, SceWChar16 *destination,
                          unsigned int capacity) {
    unsigned int index = 0;
    while (source[index] != '\0' && index + 1 < capacity) {
        destination[index] = (SceWChar16)(unsigned char)source[index];
        index++;
    }
    destination[index] = 0;
}

static int begin_room_name_dialog(void) {
    SceCommonDialogConfigParam common;
    SceImeDialogParam parameter;
    int enterButton;
    int language;
    int status;

    status = ensure_app_util();
    if (status < 0) {
        return status;
    }
    sceCommonDialogConfigParamInit(&common);
    status = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language);
    if (status < 0) {
        return status;
    }
    status = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                                         &enterButton);
    if (status < 0) {
        return status;
    }
    common.language = (SceSystemParamLang)language;
    common.enterButtonAssign = (SceSystemParamEnterButtonAssign)enterButton;
    status = sceCommonDialogSetConfigParam(&common);
    if (status < 0) {
        return status;
    }

    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) != SCE_SYSMODULE_LOADED) {
        status = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        if (status < 0) {
            return status;
        }
        s_imeModuleOwned = 1;
    }

    memset(s_imeTitle, 0, sizeof(s_imeTitle));
    memset(s_imeInitial, 0, sizeof(s_imeInitial));
    memset(s_imeOutput, 0, sizeof(s_imeOutput));
    ascii_to_wide(
        "ROOM NAME", s_imeTitle,
        (unsigned int)(sizeof(s_imeTitle) / sizeof(s_imeTitle[0])));
    ascii_to_wide(
        s_roomName, s_imeInitial,
        (unsigned int)(sizeof(s_imeInitial) / sizeof(s_imeInitial[0])));
    sceImeDialogParamInit(&parameter);
    parameter.supportedLanguages = 0;
    parameter.languagesForced = 0;
    parameter.type = SCE_IME_TYPE_BASIC_LATIN;
    parameter.option = 0;
    parameter.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    parameter.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    parameter.title = s_imeTitle;
    parameter.maxTextLength = MDKR_LAN_ROOM_NAME_BYTES - 1;
    parameter.initialText = s_imeInitial;
    parameter.inputTextBuffer = s_imeOutput;
    status = sceImeDialogInit(&parameter);
    if (status < 0) {
        if (s_imeModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
            s_imeModuleOwned = 0;
        }
        return status;
    }
    s_imeTerminated = 0;
    s_imeApiResult = 0;
    s_imeDialogResult = SCE_COMMON_DIALOG_RESULT_ABORTED;
    s_imeButton = SCE_IME_DIALOG_BUTTON_NONE;
    s_state = MDKR_VITA_LAN_NAMING;
    snprintf(s_status, sizeof(s_status), "LAN: NAME ROOM");
    return 0;
}

static void store_u16_be(unsigned char *output, unsigned int value) {
    output[0] = (unsigned char)(value >> 8);
    output[1] = (unsigned char)value;
}

static void store_u32_be(unsigned char *output, unsigned int value) {
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static void store_u64_be(unsigned char *output, unsigned long long value) {
    unsigned int index;
    for (index = 0; index < 8; index++) {
        output[index] =
            (unsigned char)(value >> (56u - index * 8u));
    }
}

static unsigned int load_u16_be(const unsigned char *input) {
    return ((unsigned int)input[0] << 8) | input[1];
}

static unsigned int load_u32_be(const unsigned char *input) {
    return ((unsigned int)input[0] << 24) |
           ((unsigned int)input[1] << 16) |
           ((unsigned int)input[2] << 8) | input[3];
}

static unsigned long long load_u64_be(const unsigned char *input) {
    unsigned long long value = 0;
    unsigned int index;
    for (index = 0; index < 8; index++) {
        value = (value << 8) | input[index];
    }
    return value;
}

static int address_equal(const SceNetSockaddrIn *left,
                         const SceNetSockaddrIn *right) {
    return left->sin_addr.s_addr == right->sin_addr.s_addr &&
           left->sin_port == right->sin_port;
}

static int packet_header_encode(unsigned char type, unsigned int sequence,
                                unsigned int roomId, unsigned char *output,
                                unsigned int length) {
    if (length < MDKR_LAN_PACKET_HEADER_SIZE ||
        length > MDKR_LAN_MAX_PACKET_SIZE) {
        return 0;
    }
    memset(output, 0, length);
    memcpy(output, s_packetMagic, sizeof(s_packetMagic));
    output[4] = MDKR_LAN_PROTOCOL_VERSION;
    output[5] = type;
    store_u16_be(output + 6, length);
    store_u32_be(output + 8, sequence);
    store_u32_be(output + 12, roomId);
    return (int)length;
}

static int packet_encode(unsigned char type, unsigned int sequence,
                         unsigned int roomId,
                         unsigned char output[MDKR_LAN_PACKET_SIZE]) {
    char version[8] = { 0 };
    packet_header_encode(type, sequence, roomId, output,
                         MDKR_LAN_PACKET_SIZE);
    memcpy(output + 16, s_roomName, strlen(s_roomName));
    snprintf(version, sizeof(version), "%s", MDKR_VERSION_STRING);
    memcpy(output + 32, version, sizeof(version));
    return MDKR_LAN_PACKET_SIZE;
}

static int packet_game_version_matches(const unsigned char *packet) {
    char version[8] = { 0 };
    snprintf(version, sizeof(version), "%s", MDKR_VERSION_STRING);
    return memcmp(packet + 32, version, sizeof(version)) == 0;
}

static int packet_decode(const unsigned char *packet, int length,
                         unsigned char *type, unsigned int *sequence,
                         unsigned int *roomId) {
    if (length < MDKR_LAN_PACKET_HEADER_SIZE ||
        (unsigned int)length > MDKR_LAN_MAX_PACKET_SIZE ||
        memcmp(packet, s_packetMagic, sizeof(s_packetMagic)) != 0 ||
        packet[4] != MDKR_LAN_PROTOCOL_VERSION ||
        load_u16_be(packet + 6) != (unsigned int)length ||
        packet[5] < MDKR_LAN_PACKET_DISCOVER ||
        packet[5] > MDKR_LAN_PACKET_STATE_DIGEST) {
        return 0;
    }
    if ((packet[5] <= MDKR_LAN_PACKET_ACK &&
         (length != MDKR_LAN_PACKET_SIZE ||
          !packet_game_version_matches(packet))) ||
        (packet[5] == MDKR_LAN_PACKET_SESSION &&
         length != MDKR_LAN_PACKET_HEADER_SIZE +
                       MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES) ||
        (packet[5] >= MDKR_LAN_PACKET_READY &&
         packet[5] <= MDKR_LAN_PACKET_GO &&
         length != MDKR_LAN_PACKET_HEADER_SIZE + 8) ||
        (packet[5] == MDKR_LAN_PACKET_INPUT &&
         length != MDKR_LAN_PACKET_HEADER_SIZE +
                       MDKR_MATCH_INPUT_BUNDLE_BYTES) ||
        (packet[5] == MDKR_LAN_PACKET_CONTROL &&
         length != MDKR_LAN_PACKET_HEADER_SIZE +
                       MDKR_LAN_CONTROL_PAYLOAD_BYTES) ||
        (packet[5] == MDKR_LAN_PACKET_CHARACTER &&
         length != MDKR_LAN_PACKET_HEADER_SIZE + 9u) ||
        (packet[5] == MDKR_LAN_PACKET_INPUT_REPAIR &&
         length != MDKR_LAN_PACKET_HEADER_SIZE +
                       MDKR_MATCH_INPUT_REPAIR_BYTES) ||
        (packet[5] == MDKR_LAN_PACKET_STATE_DIGEST &&
         length != MDKR_LAN_PACKET_HEADER_SIZE +
                       MDKR_LAN_DIGEST_PAYLOAD_BYTES)) {
        return 0;
    }
    *type = packet[5];
    *sequence = load_u32_be(packet + 8);
    *roomId = load_u32_be(packet + 12);
    return 1;
}

static int carrier_send(const SceNetSockaddrIn *destination,
                        unsigned char type, const unsigned char *packet,
                        unsigned int length) {
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
        int localSlot = mdkr_vita_relay_local_slot();
        unsigned int remoteSlot;
        int result;
        if (localSlot < 0 || localSlot > 1) {
            return -1;
        }
        remoteSlot = (unsigned int)localSlot ^ 1u;
        result = mdkr_vita_relay_send(
            1u << remoteSlot,
            mdkr_vita_relay_channel_for_lan_packet(type),
            packet, length);
        return result == 1 ? 0 : -1;
    }
#endif
    {
        int result = sceNetSendto(
            s_socket, packet, length, SCE_NET_MSG_DONTWAIT,
            (const SceNetSockaddr *)destination, sizeof(*destination));
        if (result < 0 && (*sceNetErrnoLoc() == SCE_NET_EAGAIN ||
                           *sceNetErrnoLoc() == SCE_NET_EWOULDBLOCK)) {
            return 0;
        }
        return result < 0 ? socket_error() : 0;
    }
}

static int send_payload(const SceNetSockaddrIn *destination,
                        unsigned char type, unsigned int sequence,
                        unsigned int roomId, const unsigned char *payload,
                        unsigned int payloadLength) {
    unsigned char packet[MDKR_LAN_MAX_PACKET_SIZE];
    unsigned int length = MDKR_LAN_PACKET_HEADER_SIZE + payloadLength;
    if ((payloadLength != 0 && payload == NULL) ||
        packet_header_encode(type, sequence, roomId, packet, length) == 0) {
        return -1;
    }
    if (payloadLength != 0) {
        memcpy(packet + MDKR_LAN_PACKET_HEADER_SIZE, payload, payloadLength);
    }
    return carrier_send(destination, type, packet, length);
}

static int send_packet(const SceNetSockaddrIn *destination,
                       unsigned char type, unsigned int sequence,
                       unsigned int roomId) {
    unsigned char packet[MDKR_LAN_PACKET_SIZE];
    unsigned int length =
        (unsigned int)packet_encode(type, sequence, roomId, packet);
    return carrier_send(destination, type, packet, length);
}

_Static_assert(MDKR_LAN_PACKET_HEADER_SIZE + MDKR_LAN_DIGEST_PAYLOAD_BYTES <=
                   MDKR_LAN_MAX_PACKET_SIZE,
               "state digest must fit in one LAN packet");

static void digest_reset(void) {
    memset(s_digestLocal, 0, sizeof(s_digestLocal));
    memset(s_digestRemote, 0, sizeof(s_digestRemote));
    s_digestNextTick = 1u;
    s_digestResetValidated = mdkr_rollback_game_runtime_validated_tick();
    /* After a restart the previous race's runtime may still be live; wait
     * until its tick counter regresses before witnessing the new race. */
    s_digestArmed = s_digestResetValidated == 0u;
    s_digestDiverged = 0;
    s_digestFirstDivergence = 0u;
    s_digestMatched = 0u;
    s_digestMismatched = 0u;
    s_digestLogCount = 0u;
}

static void digest_encode(const MdkrRollbackTickDigest *digest,
                          unsigned int validated,
                          unsigned char output[MDKR_LAN_DIGEST_PAYLOAD_BYTES]) {
    unsigned int index;
    memset(output, 0, MDKR_LAN_DIGEST_PAYLOAD_BYTES);
    store_u32_be(output + 0, s_launch.manifest.match_epoch);
    store_u32_be(output + 4, digest->tick);
    store_u64_be(output + 8, digest->semantic);
    store_u32_be(output + 16, digest->rng_seed);
    store_u32_be(output + 20, digest->rng_draws);
    store_u32_be(output + 24, digest->input);
    store_u32_be(output + 28, digest->world);
    for (index = 0; index < MDKR_ROLLBACK_DIGEST_RACERS; index++) {
        store_u32_be(output + 32 + index * 4u, digest->racer[index]);
    }
    store_u16_be(output + 64, digest->object_count);
    output[66] = digest->racer_count;
    output[67] = digest->confirmed_mask;
    store_u32_be(output + 68, validated);
}

static void digest_decode(const unsigned char *input, unsigned int *epoch,
                          MdkrRollbackTickDigest *digest,
                          unsigned int *validated) {
    unsigned int index;
    memset(digest, 0, sizeof(*digest));
    *epoch = load_u32_be(input + 0);
    digest->tick = load_u32_be(input + 4);
    digest->semantic = load_u64_be(input + 8);
    digest->rng_seed = load_u32_be(input + 16);
    digest->rng_draws = load_u32_be(input + 20);
    digest->input = load_u32_be(input + 24);
    digest->world = load_u32_be(input + 28);
    for (index = 0; index < MDKR_ROLLBACK_DIGEST_RACERS; index++) {
        digest->racer[index] = load_u32_be(input + 32 + index * 4u);
    }
    digest->object_count = (uint16_t)load_u16_be(input + 64);
    digest->racer_count = input[66];
    digest->confirmed_mask = input[67];
    *validated = load_u32_be(input + 68);
}

static void digest_append(char *output, size_t size, const char *field) {
    size_t used = strlen(output);
    if (used < size) {
        snprintf(output + used, size - used, "%s%s",
                 used != 0u ? "," : "", field);
    }
}

static void digest_log_side(const char *side, const MdkrLanDigestSlot *slot) {
    const MdkrRollbackTickDigest *digest = &slot->digest;
    char line[192];
    snprintf(line, sizeof(line),
             "[DESYNC]   %s tick=%u validated=%u sem=%016llx seed=%08x "
             "draws=%u input=%08x world=%08x objs=%u racers=%u conf=%02x",
             side, digest->tick, slot->validated,
             (unsigned long long)digest->semantic,
             (unsigned int)digest->rng_seed,
             (unsigned int)digest->rng_draws, (unsigned int)digest->input,
             (unsigned int)digest->world,
             (unsigned int)digest->object_count,
             (unsigned int)digest->racer_count,
             (unsigned int)digest->confirmed_mask);
    mdkr_vita_boot_log(line);
    snprintf(line, sizeof(line),
             "[DESYNC]   %s racer0-7 %08x %08x %08x %08x %08x %08x %08x %08x",
             side, (unsigned int)digest->racer[0],
             (unsigned int)digest->racer[1], (unsigned int)digest->racer[2],
             (unsigned int)digest->racer[3], (unsigned int)digest->racer[4],
             (unsigned int)digest->racer[5], (unsigned int)digest->racer[6],
             (unsigned int)digest->racer[7]);
    mdkr_vita_boot_log(line);
}

static void digest_compare(unsigned int tick) {
    MdkrLanDigestSlot *local =
        &s_digestLocal[tick % MDKR_ROLLBACK_DIGEST_SLOTS];
    MdkrLanDigestSlot *remote =
        &s_digestRemote[tick % MDKR_ROLLBACK_DIGEST_SLOTS];
    const MdkrRollbackTickDigest *a = &local->digest;
    const MdkrRollbackTickDigest *b = &remote->digest;
    char fields[112];
    char line[192];
    unsigned int index;
    if (!local->valid || !remote->valid || a->tick != tick ||
        b->tick != tick) {
        return;
    }
    fields[0] = '\0';
    /* Ordered by cause: differing canonical inputs point at the input stream,
     * differing RNG/state with equal inputs at a snapshot or determinism gap. */
    if (a->input != b->input) {
        digest_append(fields, sizeof(fields), "input");
    }
    if (a->rng_seed != b->rng_seed) {
        digest_append(fields, sizeof(fields), "rng-seed");
    }
    /* rng_draws is a per-device lifetime counter (menus, attract, earlier
     * races), so it differs on every frame between endpoints. It is still
     * logged by digest_log_side for context but is not a divergence signal. */
    if (a->object_count != b->object_count ||
        a->racer_count != b->racer_count) {
        digest_append(fields, sizeof(fields), "counts");
    }
    for (index = 0; index < MDKR_ROLLBACK_DIGEST_RACERS; index++) {
        if (a->racer[index] != b->racer[index]) {
            char name[12];
            snprintf(name, sizeof(name), "racer%u", index);
            digest_append(fields, sizeof(fields), name);
        }
    }
    if (a->world != b->world) {
        digest_append(fields, sizeof(fields), "world");
    }
    if (a->semantic != b->semantic) {
        digest_append(fields, sizeof(fields), "semantic");
    }
    if (fields[0] == '\0') {
        s_digestMatched++;
    } else {
        s_digestMismatched++;
        if (!s_digestDiverged) {
            s_digestDiverged = 1;
            s_digestFirstDivergence = tick;
            snprintf(line, sizeof(line),
                     "[DESYNC] first divergence at frame %u differs=%s "
                     "matched-before=%u",
                     tick, fields, s_digestMatched);
            mdkr_vita_boot_log(line);
            digest_log_side("local ", local);
            digest_log_side("remote", remote);
            mdkr_vita_boot_log_flush();
            s_digestLogCount++;
        } else if (s_digestLogCount < MDKR_LAN_DIGEST_LOG_LIMIT) {
            snprintf(line, sizeof(line),
                     "[DESYNC] divergence frame=%u differs=%s", tick, fields);
            mdkr_vita_boot_log(line);
            s_digestLogCount++;
        }
    }
    if (tick % 600u == 0u) {
        snprintf(line, sizeof(line),
                 "lan: state-digest frame=%u matched=%u mismatched=%u "
                 "first-divergence=%u",
                 tick, s_digestMatched, s_digestMismatched,
                 s_digestFirstDivergence);
        mdkr_vita_boot_log(line);
    }
    local->valid = 0;
    remote->valid = 0;
}

static void digest_receive(const unsigned char *payload) {
    MdkrRollbackTickDigest digest;
    MdkrLanDigestSlot *slot;
    unsigned int epoch;
    unsigned int validated;
    digest_decode(payload, &epoch, &digest, &validated);
    if (epoch != s_launch.manifest.match_epoch || digest.tick == 0u) {
        return;
    }
    slot = &s_digestRemote[digest.tick % MDKR_ROLLBACK_DIGEST_SLOTS];
    slot->digest = digest;
    slot->validated = validated;
    slot->valid = 1;
    digest_compare(digest.tick);
}

static void digest_service(void) {
    unsigned int validated;
    unsigned int budget;
    if (s_carrier != MDKR_LAN_CARRIER_UDP) {
        return;
    }
    validated = mdkr_rollback_game_runtime_validated_tick();
    if (!s_digestArmed) {
        if (validated != 0u && validated >= s_digestResetValidated) {
            return;
        }
        s_digestArmed = 1;
    }
    /* After sending tick t, validated >= t + FINAL_AGE. Falling below that
     * means a new level started without a restart notification. */
    if (s_digestNextTick > 1u &&
        (unsigned long long)validated + 1u <
            (unsigned long long)s_digestNextTick +
                MDKR_ROLLBACK_DIGEST_FINAL_AGE) {
        digest_reset();
        s_digestArmed = 1;
    }
    if (validated >= MDKR_ROLLBACK_DIGEST_SLOTS &&
        s_digestNextTick <= validated - MDKR_ROLLBACK_DIGEST_SLOTS) {
        s_digestNextTick = validated - MDKR_ROLLBACK_DIGEST_FINAL_AGE;
    }
    for (budget = 0u; budget < MDKR_LAN_DIGEST_SEND_BUDGET &&
                      (unsigned long long)s_digestNextTick +
                              MDKR_ROLLBACK_DIGEST_FINAL_AGE <=
                          validated;
         budget++) {
        const unsigned int tick = s_digestNextTick;
        MdkrLanDigestSlot *slot =
            &s_digestLocal[tick % MDKR_ROLLBACK_DIGEST_SLOTS];
        unsigned char payload[MDKR_LAN_DIGEST_PAYLOAD_BYTES];
        s_digestNextTick += MDKR_LAN_DIGEST_STRIDE;
        if (!mdkr_rollback_game_runtime_final_digest(tick, &slot->digest)) {
            slot->valid = 0;
            continue;
        }
        slot->validated = validated;
        slot->valid = 1;
        digest_encode(&slot->digest, validated, payload);
        (void)send_payload(&s_peerAddress, MDKR_LAN_PACKET_STATE_DIGEST,
                           ++s_sequence, s_roomId, payload,
                           MDKR_LAN_DIGEST_PAYLOAD_BYTES);
        digest_compare(tick);
    }
}

static void fill_identity(const char *label, unsigned char *output,
                          unsigned int length) {
    unsigned int hash = 2166136261u;
    unsigned int index;
    const unsigned char *cursor;
    for (cursor = (const unsigned char *)label; *cursor != '\0'; cursor++) {
        hash ^= *cursor;
        hash *= 16777619u;
    }
    for (cursor = (const unsigned char *)MDKR_VERSION_STRING;
         *cursor != '\0'; cursor++) {
        hash ^= *cursor;
        hash *= 16777619u;
    }
    for (index = 0; index < length; index++) {
        hash ^= hash << 13;
        hash ^= hash >> 17;
        hash ^= hash << 5;
        output[index] = (unsigned char)(hash >> 24);
    }
}

static void fill_launch_identity(MdkrMatchManifestV1 *manifest) {
    fill_identity("vita-lan-build-v2", manifest->build_id,
                  sizeof(manifest->build_id));
    fill_identity("vita-lan-gameplay-v1", manifest->gameplay_digest,
                  sizeof(manifest->gameplay_digest));
}

static int launch_identity_matches_local(
    const MdkrMatchLaunchDescriptorV1 *launch) {
    MdkrMatchManifestV1 expected;
    memset(&expected, 0, sizeof(expected));
    fill_launch_identity(&expected);
    return launch->manifest.protocol_version == MDKR_SESSION_PROTOCOL_VERSION &&
           memcmp(launch->manifest.build_id, expected.build_id,
                  sizeof(expected.build_id)) == 0 &&
           memcmp(launch->manifest.gameplay_digest,
                  expected.gameplay_digest,
                  sizeof(expected.gameplay_digest)) == 0;
}

static int build_fixed_launch(void) {
    MdkrMatchLaunchDescriptorV1 next;
    unsigned int slot;
    memset(&next, 0, sizeof(next));
    next.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    next.manifest.match_epoch = s_roomId;
    if (next.manifest.match_epoch == 0) {
        next.manifest.match_epoch = 1;
    }
    next.manifest.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    fill_launch_identity(&next.manifest);
    next.manifest.slot_owner[0] =
        ((unsigned long long)next.manifest.match_epoch << 32) | 1u;
    next.manifest.slot_owner[1] =
        ((unsigned long long)next.manifest.match_epoch << 32) | 2u;
    next.manifest.rng_seed =
        ((unsigned long long)next.manifest.match_epoch << 32) ^
        0x444B525649544131ULL;
    next.manifest.track_id = 5;
    next.manifest.rom_revision = MDKR_ROM_US_11;
    next.manifest.cadence_hz = 30;
    next.manifest.slot_count = 2;
    next.manifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    next.manifest.vehicle_mask = 0x07u;
    next.manifest.input_delay = 2;
    next.selections[0].selection_revision = 1;
    next.selections[0].character_id = 0;
    next.selections[0].vehicle_id = 0;
    next.selections[1].selection_revision = 1;
    next.selections[1].character_id = 1;
    next.selections[1].vehicle_id = 0;
    for (slot = 2; slot < MDKR_MATCH_SLOTS; slot++) {
        next.selections[slot].character_id = MDKR_MATCH_NO_CHARACTER;
        next.selections[slot].vehicle_id = MDKR_MATCH_NO_VEHICLE;
    }
    if (!mdkr_match_launch_descriptor_validate(&next)) {
        return 0;
    }
    s_launch = next;
    s_descriptorDigest = mdkr_match_launch_descriptor_digest(&s_launch);
    s_haveLaunch = s_descriptorDigest != 0;
    reset_character_selection();
    return s_haveLaunch;
}

static int send_digest_packet(unsigned char type) {
    unsigned char digest[8];
    store_u64_be(digest, s_descriptorDigest);
    return send_payload(&s_peerAddress, type, ++s_sequence, s_roomId,
                        digest, sizeof(digest));
}

static int send_session_packet(void) {
    unsigned char descriptor[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    if (!s_haveLaunch ||
        !mdkr_match_launch_descriptor_encode(
            &s_launch, descriptor, sizeof(descriptor))) {
        return -1;
    }
    return send_payload(&s_peerAddress, MDKR_LAN_PACKET_SESSION,
                        ++s_sequence, s_roomId, descriptor,
                        sizeof(descriptor));
}

static unsigned int local_character_slot(void) {
    return s_role == MDKR_LAN_ROLE_HOST ? 0u : 1u;
}

static void reset_character_selection(void) {
    s_character[0] = 9u;
    s_character[1] = 4u;
    s_characterConfirmed[0] = 0u;
    s_characterConfirmed[1] = 0u;
    s_characterRevision[0] =
        s_role == MDKR_LAN_ROLE_HOST ? 1u : 0u;
    s_characterRevision[1] =
        s_role == MDKR_LAN_ROLE_HOST ? 0u : 1u;
    s_frontendStage = MDKR_VITA_LAN_FRONTEND_CHARACTER_SELECT;
    s_frontendSelection = 0u;
    s_characterUnlockMask = 0u;
    if (s_role == MDKR_LAN_ROLE_HOST) {
        if (is_drumstick_unlocked()) {
            s_characterUnlockMask |= MDKR_VITA_LAN_UNLOCK_DRUMSTICK;
        }
        if (is_tt_unlocked()) {
            s_characterUnlockMask |= MDKR_VITA_LAN_UNLOCK_TT;
        }
        s_characterUnlocksReady = 1;
    } else {
        s_characterUnlocksReady = 0;
    }
    s_lastCharacterSendTime = 0u;
}

static int send_character_selection(void) {
    unsigned char payload[9];
    unsigned int slot = local_character_slot();
    memset(payload, 0, sizeof(payload));
    payload[0] = (unsigned char)slot;
    payload[1] = s_character[slot];
    payload[2] = s_characterConfirmed[slot];
    payload[3] = (unsigned char)(
        (s_frontendStage << 6) | (s_frontendSelection & 0x3Fu));
    store_u32_be(payload + 4, s_characterRevision[slot]);
    payload[8] = s_characterUnlockMask;
    return send_payload(&s_peerAddress, MDKR_LAN_PACKET_CHARACTER,
                        ++s_sequence, s_roomId, payload, sizeof(payload));
}

static int finalize_character_selection(void) {
    unsigned int slot;
    if (s_role != MDKR_LAN_ROLE_HOST ||
        s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        !s_characterConfirmed[0] || !s_characterConfirmed[1] ||
        s_character[0] == s_character[1]) {
        return 0;
    }
    for (slot = 0; slot < 2u; slot++) {
        s_launch.selections[slot].selection_revision =
            s_characterRevision[slot];
        s_launch.selections[slot].character_id = s_character[slot];
    }
    if (!mdkr_match_launch_descriptor_validate(&s_launch)) {
        return 0;
    }
    s_descriptorDigest = mdkr_match_launch_descriptor_digest(&s_launch);
    if (s_descriptorDigest == 0u) {
        return 0;
    }
    s_armReceived = 0;
    s_finalSessionReady = 0;
    s_startAcknowledged = 0;
    s_startScheduled = 0;
    s_lastSendTime = 0;
    s_state = MDKR_VITA_LAN_STARTING;
    set_carrier_status("LAN HOST: STARTING", "ONLINE: STARTING");
    (void)send_session_packet();
    return 1;
}

static int send_input_bundle(unsigned int newestTick) {
    MdkrMatchInputBundle bundle;
    unsigned char encoded[MDKR_MATCH_INPUT_BUNDLE_BYTES];
    unsigned int frame;
    memset(&bundle, 0, sizeof(bundle));
    bundle.match_epoch = s_launch.manifest.match_epoch;
    bundle.newest_tick = newestTick;
    bundle.slot_mask = s_localSlotMask;
    for (frame = 0;
         frame < MDKR_MATCH_INPUT_BUNDLE_FRAMES &&
         newestTick >= s_firstRaceTick + frame;
         frame++) {
        MdkrInputSet inputs;
        unsigned int slot;
        unsigned int tick = newestTick - frame;
        if (!mdkr_match_transport_inputs_for_tick(
                &s_matchTransport, bundle.match_epoch, tick, &inputs)) {
            break;
        }

        for (slot = 0; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
            if ((s_localSlotMask & (1u << slot)) != 0) {
                bundle.frames[frame][slot] = inputs.slots[slot];
            }
        }
        bundle.frame_count++;
    }
    if (!mdkr_match_input_bundle_encode(&bundle, encoded, sizeof(encoded))) {
        return -1;
    }
    return send_payload(&s_peerAddress, MDKR_LAN_PACKET_INPUT,
                        ++s_sequence, s_roomId, encoded, sizeof(encoded));
}

static int send_input_repair(const MdkrMatchInputRepair *message) {
    unsigned char encoded[MDKR_MATCH_INPUT_REPAIR_BYTES];
    if (!mdkr_match_input_repair_encode(message, encoded, sizeof(encoded))) {
        return -1;
    }
    return send_payload(&s_peerAddress, MDKR_LAN_PACKET_INPUT_REPAIR,
                        ++s_sequence, s_roomId, encoded, sizeof(encoded));
}

static void request_input_repairs(void) {
    unsigned long long now = sceKernelGetProcessTimeWide();
    unsigned int slot;
    unsigned int currentTick = s_matchTransport.history.current_tick;
#if defined(MDKR_ENABLE_VITA_RELAY)
    /* A relay connection is one reliable ordered byte stream. Its application
     * queues apply backpressure rather than dropping input, so a tail gap means
     * the peer has not authored that tick yet; UDP-style repair requests cannot
     * fill it and only compete with the ordered input already in flight. */
    if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
        return;
    }
#endif
    for (slot = 0; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        MdkrMatchInputRepair request;
        unsigned int first;
        unsigned int count;
        if (!mdkr_match_transport_input_gap(
                &s_matchTransport, slot, &first, &count)) {
            s_repairRequested[slot] = 0u;
            s_repairRequestedAtTime[slot] = 0;
            continue;
        }
        if (currentTick - first <= MDKR_MATCH_INPUT_BUNDLE_FRAMES) {
            continue;
        }
        if (s_repairRequested[slot] &&
            s_repairRequestedTick[slot] == first &&
            currentTick - s_repairRequestedAtTick[slot] <
                MDKR_LAN_REPAIR_REASK_TICKS &&
            now - s_repairRequestedAtTime[slot] <
                MDKR_LAN_REPAIR_REASK_US) {
            continue;
        }
        memset(&request, 0, sizeof(request));
        request.kind = MDKR_MATCH_INPUT_REPAIR_REQUEST;
        request.match_epoch = s_launch.manifest.match_epoch;
        request.first_tick = first;
        request.count = (unsigned char)count;
        if (send_input_repair(&request) >= 0) {
            char line[128];
            s_repairRequested[slot] = 1u;
            s_repairRequestedTick[slot] = first;
            s_repairRequestedAtTick[slot] = currentTick;
            s_repairRequestedAtTime[slot] = now;
            if (s_repairLogCount < 12u) {
                snprintf(line, sizeof(line),
                         "lan: repair-request slot=%u first=%u count=%u "
                         "observed=%u",
                         slot, first, count, currentTick);
                mdkr_vita_boot_log(line);
                s_repairLogCount++;
            }
        }
    }
}

static void answer_input_repair(const MdkrMatchInputRepair *request) {
    unsigned int slot;
    for (slot = 0; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
        unsigned int sent;
        if ((s_localSlotMask & (1u << slot)) == 0u) {
            continue;
        }
        for (sent = 0; sent < request->count;
             sent += MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS) {
            MdkrMatchInputRepair answer;
            unsigned int remaining = request->count - sent;
            unsigned int span =
                remaining < MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS
                    ? remaining
                    : MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS;
            unsigned int filled = 0;
            memset(&answer, 0, sizeof(answer));
            answer.kind = MDKR_MATCH_INPUT_REPAIR_ANSWER;
            answer.match_epoch = request->match_epoch;
            answer.first_tick = request->first_tick + sent;
            answer.slot = (unsigned char)slot;
            while (filled < span) {
                MdkrInputSet inputs;
                if (!mdkr_match_transport_inputs_for_tick(
                        &s_matchTransport, request->match_epoch,
                        answer.first_tick + filled, &inputs)) {
                    break;
                }
                answer.samples[filled] = inputs.slots[slot];
                filled++;
            }
            if (filled == 0u ||
                !mdkr_match_input_repair_budget_charge(
                    &s_repairAnswerBudget,
                    s_matchTransport.history.current_tick)) {
                return;
            }
            answer.count = (unsigned char)filled;
            (void)send_input_repair(&answer);
            if (filled < span) {
                return;
            }
        }
    }
}

static void apply_input_repair(const MdkrMatchInputRepair *answer) {
    unsigned int index;
    char line[128];
    if (answer->slot >= MDKR_SESSION_MAX_PLAYERS ||
        (s_remoteSlotMask & (1u << answer->slot)) == 0u) {
        return;
    }
    for (index = 0; index < answer->count; index++) {
        (void)mdkr_match_transport_receive(
            &s_matchTransport, answer->match_epoch, s_remoteSlotMask,
            answer->slot, answer->first_tick + index,
            &answer->samples[index]);
    }
    if (s_repairLogCount < 12u) {
        snprintf(line, sizeof(line),
                 "lan: repair-answer slot=%u first=%u count=%u",
                 (unsigned int)answer->slot, answer->first_tick,
                 (unsigned int)answer->count);
        mdkr_vita_boot_log(line);
        s_repairLogCount++;
    }
}

static unsigned char control_wire_type(MdkrVitaLanControl control) {
    switch (control) {
        case MDKR_VITA_LAN_CONTROL_RESTART:
            return MDKR_LAN_CONTROL_WIRE_RESTART;
        case MDKR_VITA_LAN_CONTROL_SELECT_TRACK:
            return MDKR_LAN_CONTROL_WIRE_SELECT_TRACK;
        case MDKR_VITA_LAN_CONTROL_QUIT:
            return MDKR_LAN_CONTROL_WIRE_QUIT;
        default:
            return 0;
    }
}

static int send_control(unsigned char wireType, unsigned int trackId,
                        unsigned int vehicleMask, unsigned int matchEpoch,
                        unsigned int controlId, unsigned int executeTick) {
    unsigned char payload[MDKR_LAN_CONTROL_PAYLOAD_BYTES];
    memset(payload, 0, sizeof(payload));
    payload[0] = wireType;
    payload[1] = (unsigned char)trackId;
    payload[2] = (unsigned char)vehicleMask;
    store_u32_be(payload + 4, matchEpoch);
    store_u32_be(payload + 8, controlId);
    store_u32_be(payload + 12, executeTick);
    return send_payload(&s_peerAddress, MDKR_LAN_PACKET_CONTROL,
                        ++s_sequence, s_roomId, payload, sizeof(payload));
}

static bool match_input_drain(
    void *context, uint32_t matchEpoch, uint32_t tick,
    const MdkrPadSample physicalInputs[MDKR_SESSION_MAX_PLAYERS],
    unsigned int physicalInputCount, MdkrInputSet *output) {
    MdkrPadSample local;
    const MdkrMatchTransportStats *stats;
    char line[128];
    (void)context;
    if (s_state != MDKR_VITA_LAN_RACING || physicalInputCount == 0 ||
        output == NULL || matchEpoch != s_launch.manifest.match_epoch) {
        snprintf(line, sizeof(line),
                 "lan: input-drain precondition state=%u count=%u "
                 "epoch=%u expected=%u",
                 (unsigned int)s_state, physicalInputCount, matchEpoch,
                 s_launch.manifest.match_epoch);
        mdkr_vita_boot_log(line);
        return false;
    }
    local = physicalInputs[0];
    mdkr_online_race_pause_filter_local_input(&local);
    if (!mdkr_match_transport_drain_tick(
            &s_matchTransport, matchEpoch, tick, &local, 1)) {
        stats = mdkr_match_transport_stats(&s_matchTransport);
        snprintf(line, sizeof(line),
                 "lan: input-drain rejected tick=%u rejected=%u drained=%u",
                 tick, stats != NULL ? stats->drain_rejected : 0u,
                 stats != NULL ? stats->drained : 0u);
        mdkr_vita_boot_log(line);
        return false;
    }
    {
        int sendResult = send_input_bundle(tick);
        if (s_inputSendLogCount < 8u) {
            snprintf(line, sizeof(line),
                     "lan: input-sent tick=%u result=%d", tick,
                     sendResult);
            mdkr_vita_boot_log(line);
            s_inputSendLogCount++;
        }
    }
    request_input_repairs();
    if (!mdkr_match_transport_inputs_for_tick(
            &s_matchTransport, matchEpoch, tick, output)) {
        snprintf(line, sizeof(line),
                 "lan: input-lookup rejected tick=%u", tick);
        mdkr_vita_boot_log(line);
        return false;
    }
    return true;
}

static bool match_inputs_for_tick(void *context, uint32_t matchEpoch,
                                  uint32_t tick, MdkrInputSet *output) {
    (void)context;
    return mdkr_match_transport_inputs_for_tick(
        &s_matchTransport, matchEpoch, tick, output);
}

static bool match_take_dirty(void *context, uint32_t matchEpoch,
                             uint32_t *tick) {
    (void)context;
    return matchEpoch == s_launch.manifest.match_epoch &&
           mdkr_match_transport_take_dirty(&s_matchTransport, tick);
}

static bool match_ai_mask(void *context, uint32_t matchEpoch,
                          uint32_t tick, unsigned char *slotMask) {
    (void)context;
    return mdkr_match_transport_ai_takeover_mask_for_tick(
        &s_matchTransport, matchEpoch, tick, slotMask);
}

static int install_match_runtime(void) {
    MdkrNetRoster roster;
    MdkrSessionLaunchV3 launch;
    MdkrMatchInputSource source;
    unsigned char localSlot =
        s_role == MDKR_LAN_ROLE_HOST ? 0 : 1;
    if (!s_haveLaunch || !launch_identity_matches_local(&s_launch) ||
        !mdkr_net_roster_init(&roster, &s_launch.manifest) ||
        !mdkr_net_roster_configure_local(&roster, &localSlot, 1) ||
        !mdkr_net_roster_set_viewports(&roster, &localSlot, 1)) {
        return 0;
    }
    mdkr_match_input_runtime_clear();
    mdkr_net_roster_runtime_clear();
    if (!mdkr_net_roster_runtime_install_launch(&s_launch, &roster)) {
        return 0;
    }
    memset(&launch, 0, sizeof(launch));
    launch.version = MDKR_SESSION_LAUNCH_V3_VERSION;
    launch.size = sizeof(launch);
    launch.match = s_launch;
    launch.local_slot_mask = (unsigned char)(1u << localSlot);
    launch.viewport_slot_mask = launch.local_slot_mask;
    mdkr_session_bridge_init(&s_matchBridge);
    if (!mdkr_session_bridge_apply_launch_v3(&s_matchBridge, &launch) ||
        !mdkr_session_bridge_set_engine_phase(
            &s_matchBridge, MDKR_ENGINE_BOOTING) ||
        !mdkr_session_bridge_set_engine_phase(
            &s_matchBridge, MDKR_ENGINE_READY) ||
        !mdkr_match_transport_init(
            &s_matchTransport, &s_matchBridge, s_firstRaceTick)) {
        mdkr_net_roster_runtime_clear();
        return 0;
    }
    memset(&source, 0, sizeof(source));
    source.version = MDKR_MATCH_INPUT_SOURCE_VERSION;
    source.match_epoch = s_launch.manifest.match_epoch;
    source.context = &s_matchTransport;
    source.drain = match_input_drain;
    source.inputs_for_tick = match_inputs_for_tick;
    source.take_dirty = match_take_dirty;
    source.ai_mask_for_tick = match_ai_mask;
    if (!mdkr_match_input_runtime_install(&source)) {
        mdkr_net_roster_runtime_clear();
        return 0;
    }
    s_localSlotMask = launch.local_slot_mask;
    s_remoteSlotMask = (unsigned char)(0x03u & ~s_localSlotMask);
    reset_input_repair();
    reset_finish_barrier();
    s_matchRuntimeOwned = 1;
    return 1;
}

static int open_socket(unsigned short port) {
    SceNetSockaddrIn address;
    int enabled = 1;
    int state;
    int result;

    result = mdkr_vita_network_acquire();
    log_result("network-acquire", result);
    if (result < 0) {
        return result;
    }
    s_networkOwned = 1;

    result = sceNetCtlInetGetState(&state);
    if (result < 0) {
        return result;
    }
    if (state != SCE_NETCTL_STATE_CONNECTED) {
        return -SCE_NET_ENETDOWN;
    }

    s_socket = sceNetSocket("mdkr-lan", SCE_NET_AF_INET,
                            SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (s_socket < 0) {
        return socket_error();
    }
    result = sceNetSetsockopt(s_socket, SCE_NET_SOL_SOCKET,
                              SCE_NET_SO_BROADCAST, &enabled,
                              sizeof(enabled));
    if (result < 0) {
        return socket_error();
    }
    result = sceNetSetsockopt(s_socket, SCE_NET_SOL_SOCKET,
                              SCE_NET_SO_REUSEADDR, &enabled,
                              sizeof(enabled));
    if (result < 0) {
        return socket_error();
    }

    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(port);
    address.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    result = sceNetBind(s_socket, (const SceNetSockaddr *)&address,
                        sizeof(address));
    return result < 0 ? socket_error() : 0;
}

static void set_error(const char *stage, int result) {
    MdkrVitaLanFailure failure;
    char detail[80];
    mdkr_vita_lan_stop();
    snprintf(s_status, sizeof(s_status), "LAN ERROR %s %08X", stage,
             (unsigned int)result);
    s_state = MDKR_VITA_LAN_ERROR;
    failure = strcmp(stage, "VERSION") == 0
                  ? MDKR_VITA_LAN_FAILURE_VERSION_MISMATCH
                  : (strcmp(stage, "MATCH") == 0
                         ? MDKR_VITA_LAN_FAILURE_STARTUP
                         : MDKR_VITA_LAN_FAILURE_GENERIC);
    snprintf(detail, sizeof(detail), "%s result=%08X", stage,
             (unsigned int)result);
    mdkr_vita_lan_report_failure_detail(failure, detail);
    log_result(stage, result);
}

int mdkr_vita_lan_character_selection(
    unsigned int *localSlot, unsigned char characters[2],
    unsigned char confirmed[2]) {
    if (s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        localSlot == NULL || characters == NULL || confirmed == NULL) {
        return 0;
    }
    *localSlot = local_character_slot();
    memcpy(characters, s_character, sizeof(s_character));
    memcpy(confirmed, s_characterConfirmed,
           sizeof(s_characterConfirmed));
    return 1;
}

int mdkr_vita_lan_set_character_selection(unsigned int character,
                                          int confirmed) {
    unsigned int slot;
    unsigned int remote;
    if (s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        character >= MDKR_MATCH_CHARACTER_COUNT) {
        return 0;
    }
    slot = local_character_slot();
    remote = 1u - slot;
    if (confirmed && s_characterConfirmed[remote] &&
        s_character[remote] == character) {
        return 0;
    }
    if (s_character[slot] == character &&
        s_characterConfirmed[slot] == (confirmed != 0)) {
        return 1;
    }
    s_character[slot] = (unsigned char)character;
    s_characterConfirmed[slot] = confirmed ? 1u : 0u;
    s_characterRevision[slot]++;
    if (s_characterRevision[slot] == 0u) {
        s_characterRevision[slot] = 1u;
    }
    s_lastCharacterSendTime = sceKernelGetProcessTimeWide();
    (void)send_character_selection();
    return 1;
}

int mdkr_vita_lan_frontend_state(unsigned int *stage,
                                 unsigned int *selection) {
    if (s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        stage == NULL || selection == NULL) {
        return 0;
    }
    *stage = s_frontendStage;
    *selection = s_frontendSelection;
    return 1;
}

int mdkr_vita_lan_set_frontend_state(unsigned int stage,
                                     unsigned int selection) {
    unsigned int slot;
    if (s_role != MDKR_LAN_ROLE_HOST ||
        s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        stage > MDKR_VITA_LAN_FRONTEND_TRACK_SELECT ||
        selection > 0x3Fu) {
        return 0;
    }
    if (s_frontendStage == stage && s_frontendSelection == selection) {
        return 1;
    }
    s_frontendStage = (unsigned char)stage;
    s_frontendSelection = (unsigned char)selection;
    slot = local_character_slot();
    s_characterRevision[slot]++;
    if (s_characterRevision[slot] == 0u) {
        s_characterRevision[slot] = 1u;
    }
    s_lastCharacterSendTime = sceKernelGetProcessTimeWide();
    (void)send_character_selection();
    return 1;
}

int mdkr_vita_lan_character_unlocks(unsigned int *unlockMask) {
    if (unlockMask == NULL || !s_characterUnlocksReady ||
        s_state != MDKR_VITA_LAN_CHARACTER_SELECT) {
        return 0;
    }
    *unlockMask = s_characterUnlockMask;
    return 1;
}

int mdkr_vita_lan_start_selected_race(unsigned int trackId,
                                      unsigned int vehicleMask) {
    unsigned int vehicle;
    if (s_role != MDKR_LAN_ROLE_HOST ||
        s_state != MDKR_VITA_LAN_CHARACTER_SELECT ||
        s_frontendStage != MDKR_VITA_LAN_FRONTEND_TRACK_SELECT ||
        trackId > UINT16_MAX || vehicleMask == 0u ||
        (vehicleMask & ~0x07u) != 0u) {
        return 0;
    }
    vehicle = (unsigned int)leveltable_vehicle_default((s32)trackId);
    if (vehicle > VEHICLE_PLANE ||
        (vehicleMask & (1u << vehicle)) == 0u) {
        vehicle = (vehicleMask & (1u << VEHICLE_CAR)) != 0u
                      ? VEHICLE_CAR
                      : ((vehicleMask &
                          (1u << VEHICLE_HOVERCRAFT)) != 0u
                             ? VEHICLE_HOVERCRAFT
                             : VEHICLE_PLANE);
    }
    s_launch.manifest.track_id = (unsigned short)trackId;
    s_launch.manifest.vehicle_mask = (unsigned char)vehicleMask;
    s_launch.selections[0].vehicle_id = (unsigned char)vehicle;
    s_launch.selections[1].vehicle_id = (unsigned char)vehicle;
    return finalize_character_selection();
}

void mdkr_vita_lan_report_failure(MdkrVitaLanFailure failure) {
    mdkr_vita_lan_report_failure_detail(failure, NULL);
}

void mdkr_vita_lan_report_failure_detail(
    MdkrVitaLanFailure failure, const char *detail) {
    if (failure == MDKR_VITA_LAN_FAILURE_NONE || s_failurePending) {
        return;
    }
    s_failure = failure;
    s_failurePending = 1;
    s_raceLaunchPending = 0;
    s_state = MDKR_VITA_LAN_ERROR;
    snprintf(s_status, sizeof(s_status), "%s", failure_message(failure));
    if (detail != NULL && detail[0] != '\0') {
        snprintf(s_failureMessage, sizeof(s_failureMessage), "%s\n%.96s",
                 failure_message(failure), detail);
    } else {
        snprintf(s_failureMessage, sizeof(s_failureMessage), "%s",
                 failure_message(failure));
    }
    persist_failure_detail(failure, failure_message(failure), detail);
    log_result("race-failure", -(int)failure);
}

static void send_discover(void) {
    SceNetSockaddrIn broadcast;
    memset(&broadcast, 0, sizeof(broadcast));
    broadcast.sin_len = sizeof(broadcast);
    broadcast.sin_family = SCE_NET_AF_INET;
    broadcast.sin_port = sceNetHtons(MDKR_LAN_PORT);
    broadcast.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_BROADCAST);
    send_packet(&broadcast, MDKR_LAN_PACKET_DISCOVER, ++s_sequence, 0);
}

static void record_room(const SceNetSockaddrIn *source,
                        const unsigned char *packet, unsigned int roomId,
                        unsigned long long now) {
    unsigned int index;
    unsigned int nameIndex;
    char discoveredName[MDKR_LAN_ROOM_NAME_BYTES];

    for (index = 0; index < s_roomCount; index++) {
        if (s_rooms[index].roomId == roomId &&
            address_equal(&s_rooms[index].address, source)) {
            s_rooms[index].lastSeenTime = now;
            return;
        }
    }
    if (s_roomCount >= MDKR_LAN_MAX_ROOMS) {
        return;
    }
    index = s_roomCount++;
    s_rooms[index].address = *source;
    s_rooms[index].roomId = roomId;
    s_rooms[index].lastSeenTime = now;
    memcpy(discoveredName, packet + 16, MDKR_LAN_ROOM_NAME_BYTES);
    discoveredName[MDKR_LAN_ROOM_NAME_BYTES - 1] = '\0';
    for (nameIndex = 0;
         nameIndex < MDKR_LAN_ROOM_NAME_BYTES - 1 &&
         discoveredName[nameIndex] != '\0'; nameIndex++) {
        if (discoveredName[nameIndex] < 0x20 ||
            discoveredName[nameIndex] > 0x7E) {
            discoveredName[nameIndex] = '?';
        }
    }
    snprintf(s_rooms[index].label, sizeof(s_rooms[index].label), "%s",
             discoveredName[0] != '\0' ? discoveredName : "ROOM");
    log_result("room-found",
               (int)(sceNetNtohl(source->sin_addr.s_addr) & 0xFFu));
}

static void expire_rooms(unsigned long long now) {
    unsigned int index = 0;
    while (index < s_roomCount) {
        if (now - s_rooms[index].lastSeenTime <= MDKR_LAN_ROOM_TIMEOUT_US) {
            index++;
            continue;
        }
        if (index + 1 < s_roomCount) {
            memmove(&s_rooms[index], &s_rooms[index + 1],
                    (s_roomCount - index - 1) * sizeof(s_rooms[0]));
        }
        s_roomCount--;
    }
}

#if defined(MDKR_ENABLE_VITA_RELAY)
static void relay_slot_address(unsigned int slot,
                               SceNetSockaddrIn *address) {
    memset(address, 0, sizeof(*address));
    address->sin_len = sizeof(*address);
    address->sin_family = SCE_NET_AF_INET;
    address->sin_port = sceNetHtons(MDKR_LAN_PORT);
    address->sin_addr.s_addr =
        sceNetHtonl(0x7F000001u + (slot & 0x03u));
}
#endif

static int carrier_receive(unsigned char *packet, unsigned int capacity,
                           SceNetSockaddrIn *source,
                           unsigned int *relayChannel) {
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
        MdkrVitaRelayChannel channel;
        unsigned int senderSlot;
        unsigned int length;
        int result = mdkr_vita_relay_receive(
            &senderSlot, &channel, packet, capacity, &length);
        if (result <= 0) {
            return result;
        }
        relay_slot_address(senderSlot, source);
        *relayChannel = (unsigned int)channel;
        return (int)length;
    }
#endif
    {
        unsigned int sourceLength = sizeof(*source);
        int result = sceNetRecvfrom(
            s_socket, packet, capacity, SCE_NET_MSG_DONTWAIT,
            (SceNetSockaddr *)source, &sourceLength);
        if (result < 0 && (*sceNetErrnoLoc() == SCE_NET_EAGAIN ||
                           *sceNetErrnoLoc() == SCE_NET_EWOULDBLOCK)) {
            return 0;
        }
        *relayChannel = 0u;
        return result;
    }
}

static void reset_deferred_input_bundles(void) {
    s_deferredInputBundleCount = 0;
    s_deferredInputLogCount = 0;
}

static int defer_input_bundle(const MdkrMatchInputBundle *bundle,
                              unsigned int sequence) {
    MdkrLanDeferredInputBundle *deferred;
    if (bundle == NULL ||
        s_deferredInputBundleCount >= MDKR_LAN_DEFERRED_INPUT_BUNDLES) {
        return 0;
    }
    deferred = &s_deferredInputBundles[s_deferredInputBundleCount++];
    deferred->bundle = *bundle;
    deferred->sequence = sequence;
    if (s_deferredInputLogCount < 4u) {
        char line[128];
        snprintf(line, sizeof(line),
                 "lan: deferred-input epoch=%u tick=%u count=%u",
                 bundle->match_epoch, bundle->newest_tick,
                 s_deferredInputBundleCount);
        mdkr_vita_boot_log(line);
        s_deferredInputLogCount++;
    }
    return 1;
}

static int apply_deferred_input_bundles(unsigned int matchEpoch) {
    unsigned int index;
    for (index = 0; index < s_deferredInputBundleCount; index++) {
        const MdkrLanDeferredInputBundle *deferred =
            &s_deferredInputBundles[index];
        const MdkrMatchInputBundle *bundle = &deferred->bundle;
        unsigned int frame;
        unsigned int slot;
        if (bundle->match_epoch != matchEpoch) {
            continue;
        }
        for (frame = 0; frame < bundle->frame_count; frame++) {
            unsigned int tick = bundle->newest_tick - frame;
            for (slot = 0; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
                MdkrMatchTransportIngressResult ingress;
                if ((bundle->slot_mask & (1u << slot)) == 0u) {
                    continue;
                }
                ingress = mdkr_match_transport_receive(
                    &s_matchTransport, bundle->match_epoch,
                    bundle->slot_mask, slot, tick,
                    &bundle->frames[frame][slot]);
                if (ingress >= MDKR_MATCH_INGRESS_INVALID) {
                    char detail[112];
                    snprintf(detail, sizeof(detail),
                             "DEFERRED INPUT reason=%u tick=%u slot=%u "
                             "seq=%u",
                             (unsigned int)ingress, tick, slot,
                             deferred->sequence);
                    reset_deferred_input_bundles();
                    mdkr_vita_lan_report_failure_detail(
                        MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, detail);
                    return 0;
                }
            }
        }
    }
    if (s_deferredInputBundleCount != 0u) {
        char line[96];
        snprintf(line, sizeof(line),
                 "lan: deferred-input-applied epoch=%u bundles=%u",
                 matchEpoch, s_deferredInputBundleCount);
        mdkr_vita_boot_log(line);
    }
    reset_deferred_input_bundles();
    return 1;
}

static int is_previous_match_epoch(unsigned int candidate,
                                   unsigned int current) {
    unsigned int following = candidate + 1u;
    if (following == 0u) {
        following = 1u;
    }
    return following == current;
}

static void receive_packets(unsigned long long now) {
    int receiveCount;
    int receiveLimit = MDKR_LAN_MAX_RECEIVES_PER_UPDATE;
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
        receiveLimit = MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH;
    }
#endif
    for (receiveCount = 0;
         receiveCount < receiveLimit;
         receiveCount++) {
        unsigned char packet[MDKR_LAN_MAX_PACKET_SIZE];
        SceNetSockaddrIn source;
        unsigned int relayChannel = 0u;
        unsigned char type;
        unsigned int sequence;
        unsigned int roomId;
        int result;

        memset(&source, 0, sizeof(source));
        result = carrier_receive(packet, sizeof(packet), &source,
                                 &relayChannel);
        if (result == 0) {
            break;
        }
        if (result < 0 || source.sin_family != SCE_NET_AF_INET ||
            !packet_decode(packet, result, &type, &sequence, &roomId)) {
#if defined(MDKR_ENABLE_VITA_RELAY)
            if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
                char detail[96];
                snprintf(detail, sizeof(detail),
                         "INVALID LAN PACKET len=%d channel=%u", result,
                         relayChannel);
                mdkr_vita_lan_report_failure_detail(
                    MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, detail);
                return;
            }
#endif
            continue;
        }
#if defined(MDKR_ENABLE_VITA_RELAY)
        if (s_carrier == MDKR_LAN_CARRIER_RELAY &&
            relayChannel !=
                (unsigned int)
                    mdkr_vita_relay_channel_for_lan_packet(type)) {
            char detail[112];
            snprintf(detail, sizeof(detail),
                     "CHANNEL MISMATCH type=%u got=%u expected=%u seq=%u",
                     type, relayChannel,
                     (unsigned int)
                         mdkr_vita_relay_channel_for_lan_packet(type),
                     sequence);
            mdkr_vita_lan_report_failure_detail(
                MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, detail);
            return;
        }
#endif

        if (s_role == MDKR_LAN_ROLE_HOST) {
            if (s_state == MDKR_VITA_LAN_HOSTING &&
                type == MDKR_LAN_PACKET_DISCOVER) {
                send_packet(&source, MDKR_LAN_PACKET_OFFER, sequence,
                            s_roomId);
            } else if (s_state == MDKR_VITA_LAN_HOSTING &&
                       type == MDKR_LAN_PACKET_JOIN &&
                       roomId == s_roomId) {
                s_peerAddress = source;
                s_lastReceiveTime = now;
                if (!build_fixed_launch()) {
                    set_error("SESSION", -1);
                    return;
                }
                s_state = MDKR_VITA_LAN_HANDSHAKING;
                set_carrier_status("LAN HOST: HANDSHAKE",
                                   "ONLINE: SYNCHRONIZING");
                send_packet(&source, MDKR_LAN_PACKET_ACCEPT, sequence,
                            s_roomId);
                (void)send_session_packet();
                log_result("host-handshake", 0);
            }
        } else if (s_role == MDKR_LAN_ROLE_BROWSER) {
            if (s_state == MDKR_VITA_LAN_BROWSING &&
                type == MDKR_LAN_PACKET_OFFER && roomId != 0) {
                record_room(&source, packet, roomId, now);
            } else if (s_state == MDKR_VITA_LAN_JOINING &&
                       type == MDKR_LAN_PACKET_ACCEPT &&
                       roomId == s_roomId &&
                       address_equal(&source, &s_roomAddress)) {
                s_peerAddress = source;
                s_lastReceiveTime = now;
                s_state = MDKR_VITA_LAN_HANDSHAKING;
                set_carrier_status("LAN JOIN: HANDSHAKE",
                                   "ONLINE: SYNCHRONIZING");
                log_result("join-handshake", 0);
            }
        }

        if (!address_equal(&source, &s_peerAddress) ||
            roomId != s_roomId) {
            continue;
        }
        if (s_state == MDKR_VITA_LAN_HANDSHAKING ||
            s_state == MDKR_VITA_LAN_CHARACTER_SELECT ||
            s_state == MDKR_VITA_LAN_STARTING ||
            s_state == MDKR_VITA_LAN_RACING) {
            s_lastReceiveTime = now;
        }
        if (type == MDKR_LAN_PACKET_PING) {
            send_packet(&source, MDKR_LAN_PACKET_ACK, sequence, s_roomId);
            continue;
        }
        if (s_state == MDKR_VITA_LAN_CHARACTER_SELECT &&
            type == MDKR_LAN_PACKET_CHARACTER) {
            unsigned int slot =
                packet[MDKR_LAN_PACKET_HEADER_SIZE];
            unsigned int character =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 1];
            unsigned int confirmed =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 2];
            unsigned int frontend =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 3];
            unsigned int frontendStage = frontend >> 6;
            unsigned int frontendSelection = frontend & 0x3Fu;
            unsigned int revision =
                load_u32_be(packet + MDKR_LAN_PACKET_HEADER_SIZE + 4);
            unsigned int unlockMask =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 8];
            unsigned int expectedSlot =
                s_role == MDKR_LAN_ROLE_HOST ? 1u : 0u;
            if (slot != expectedSlot || character >=
                    MDKR_MATCH_CHARACTER_COUNT || confirmed > 1u ||
                frontendStage > MDKR_VITA_LAN_FRONTEND_TRACK_SELECT ||
                (unlockMask & ~(MDKR_VITA_LAN_UNLOCK_DRUMSTICK |
                                MDKR_VITA_LAN_UNLOCK_TT)) != 0u ||
                revision == 0u || revision <= s_characterRevision[slot]) {
                continue;
            }
            s_character[slot] = (unsigned char)character;
            s_characterConfirmed[slot] = (unsigned char)confirmed;
            s_characterRevision[slot] = revision;
            if (s_role == MDKR_LAN_ROLE_BROWSER && slot == 0u) {
                s_frontendStage = (unsigned char)frontendStage;
                s_frontendSelection = (unsigned char)frontendSelection;
                s_characterUnlockMask = (unsigned char)unlockMask;
                s_characterUnlocksReady = 1;
            }
            if (s_characterConfirmed[0] && s_characterConfirmed[1] &&
                s_character[0] == s_character[1]) {
                s_characterConfirmed[1] = 0u;
                if (local_character_slot() == 1u) {
                    s_characterRevision[1]++;
                    (void)send_character_selection();
                }
            }
            continue;
        }
        if (s_state == MDKR_VITA_LAN_RACING &&
            type == MDKR_LAN_PACKET_STATE_DIGEST) {
            if (s_carrier == MDKR_LAN_CARRIER_UDP && s_matchRuntimeOwned) {
                digest_receive(packet + MDKR_LAN_PACKET_HEADER_SIZE);
            }
            continue;
        }
        if (s_state == MDKR_VITA_LAN_RACING &&
            type == MDKR_LAN_PACKET_INPUT_REPAIR &&
            s_matchRuntimeOwned) {
            MdkrMatchInputRepair repair;
            if (!mdkr_match_input_repair_decode(
                    packet + MDKR_LAN_PACKET_HEADER_SIZE,
                    MDKR_MATCH_INPUT_REPAIR_BYTES, &repair) ||
                repair.match_epoch != s_launch.manifest.match_epoch) {
                continue;
            }
            if (repair.kind == MDKR_MATCH_INPUT_REPAIR_REQUEST) {
                answer_input_repair(&repair);
            } else {
                apply_input_repair(&repair);
            }
            continue;
        }
        if (s_state == MDKR_VITA_LAN_RACING &&
            type == MDKR_LAN_PACKET_CONTROL) {
            unsigned char wireType =
                packet[MDKR_LAN_PACKET_HEADER_SIZE];
            unsigned int trackId =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 1];
            unsigned int vehicleMask =
                packet[MDKR_LAN_PACKET_HEADER_SIZE + 2];
            unsigned int matchEpoch =
                load_u32_be(packet + MDKR_LAN_PACKET_HEADER_SIZE + 4);
            unsigned int controlId =
                load_u32_be(packet + MDKR_LAN_PACKET_HEADER_SIZE + 8);
            if (wireType == MDKR_LAN_CONTROL_WIRE_CURSOR &&
                s_role == MDKR_LAN_ROLE_BROWSER) {
                s_trackPickerActive = 1;
                s_trackPickerTrackId = trackId;
                continue;
            }
            if (wireType == MDKR_LAN_CONTROL_WIRE_CURSOR_CLOSE &&
                s_role == MDKR_LAN_ROLE_BROWSER) {
                s_trackPickerActive = 0;
                continue;
            }
            if (wireType == MDKR_LAN_CONTROL_WIRE_FINISH_READY) {
                unsigned int finishTick =
                    load_u32_be(packet + MDKR_LAN_PACKET_HEADER_SIZE + 12);
                if (matchEpoch == s_launch.manifest.match_epoch &&
                    finishTick != 0u && !s_finishComplete) {
                    if (s_finishRemoteTick == 0u &&
                        (s_role == MDKR_LAN_ROLE_BROWSER ||
                         (s_finishLocalTick != 0u &&
                          finishTick == s_finishLocalTick))) {
                        s_finishRemoteTick = finishTick;
                        s_finishRemoteReadyTime = now;
                        log_result("finish-ready-received", (int)finishTick);
                    }
                }
                continue;
            }
            if (s_controlPending &&
                s_role == MDKR_LAN_ROLE_HOST &&
                (s_control == MDKR_VITA_LAN_CONTROL_RESTART ||
                 s_control == MDKR_VITA_LAN_CONTROL_SELECT_TRACK)) {
                continue;
            }
            if (controlId == 0 ||
                controlId <= s_lastReceivedControlId) {
                continue;
            }
            if (wireType == MDKR_LAN_CONTROL_WIRE_RESTART ||
                wireType == MDKR_LAN_CONTROL_WIRE_SELECT_TRACK) {
                if (s_role != MDKR_LAN_ROLE_BROWSER ||
                    matchEpoch != s_launch.manifest.match_epoch + 1u ||
                    matchEpoch == 0 || vehicleMask == 0 ||
                    (vehicleMask & ~0x07u) != 0) {
                    continue;
                }
                s_control =
                    wireType == MDKR_LAN_CONTROL_WIRE_RESTART
                        ? MDKR_VITA_LAN_CONTROL_RESTART
                        : MDKR_VITA_LAN_CONTROL_SELECT_TRACK;
            } else if (wireType == MDKR_LAN_CONTROL_WIRE_QUIT) {
                s_control = MDKR_VITA_LAN_CONTROL_QUIT;
            } else {
                continue;
            }
            reset_deferred_input_bundles();
            s_lastReceivedControlId = controlId;
            s_controlId = controlId;
            s_controlTrackId = trackId;
            s_controlVehicleMask = vehicleMask;
            s_controlMatchEpoch = matchEpoch;
            s_controlExecuteTime = now + MDKR_LAN_CONTROL_DELAY_US;
            s_controlPending = 1;
            s_trackPickerActive = 0;
            log_control("control-received", wireType);
            continue;
        }
        if (s_role == MDKR_LAN_ROLE_BROWSER &&
            (s_state == MDKR_VITA_LAN_HANDSHAKING ||
             s_state == MDKR_VITA_LAN_CHARACTER_SELECT) &&
            type == MDKR_LAN_PACKET_SESSION) {
            MdkrMatchLaunchDescriptorV1 launch;
            int finalSelection =
                s_state == MDKR_VITA_LAN_CHARACTER_SELECT;
            if (!mdkr_match_launch_descriptor_decode(
                    packet + MDKR_LAN_PACKET_HEADER_SIZE,
                    MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES, &launch) ||
                !launch_identity_matches_local(&launch) ||
                launch.manifest.match_epoch != roomId) {
                set_error("VERSION", -1);
                return;
            }
            if (finalSelection) {
                unsigned int localSlot = local_character_slot();
                if (!s_characterConfirmed[localSlot] ||
                    launch.selections[localSlot].character_id !=
                        s_character[localSlot] ||
                    launch.selections[0].character_id ==
                        launch.selections[1].character_id) {
                    continue;
                }
                s_character[0] = launch.selections[0].character_id;
                s_character[1] = launch.selections[1].character_id;
                s_characterConfirmed[0] = 1u;
                s_characterConfirmed[1] = 1u;
            }
            s_launch = launch;
            s_descriptorDigest =
                mdkr_match_launch_descriptor_digest(&s_launch);
            s_haveLaunch = s_descriptorDigest != 0;
            if (!finalSelection) {
                reset_character_selection();
                s_state = MDKR_VITA_LAN_CHARACTER_SELECT;
                set_carrier_status("LAN JOIN: SELECT RACER",
                                   "ONLINE: SELECT RACER");
            } else {
                s_state = MDKR_VITA_LAN_STARTING;
                set_carrier_status("LAN JOIN: READY", "ONLINE: READY");
            }
            (void)send_digest_packet(MDKR_LAN_PACKET_READY);
            log_result(finalSelection ? "join-ready" : "join-select", 0);
        } else if (s_role == MDKR_LAN_ROLE_HOST &&
                   s_state == MDKR_VITA_LAN_HANDSHAKING &&
                   type == MDKR_LAN_PACKET_READY &&
                   load_u64_be(packet + MDKR_LAN_PACKET_HEADER_SIZE) ==
                       s_descriptorDigest) {
            s_state = MDKR_VITA_LAN_CHARACTER_SELECT;
            set_carrier_status("LAN HOST: SELECT RACER",
                               "ONLINE: SELECT RACER");
            (void)send_character_selection();
        } else if (s_role == MDKR_LAN_ROLE_HOST &&
                   s_state == MDKR_VITA_LAN_STARTING &&
                   type == MDKR_LAN_PACKET_READY &&
                   load_u64_be(packet + MDKR_LAN_PACKET_HEADER_SIZE) ==
                       s_descriptorDigest) {
            s_finalSessionReady = 1;
            (void)send_digest_packet(MDKR_LAN_PACKET_ARM);
        } else if (s_role == MDKR_LAN_ROLE_BROWSER &&
                   s_state == MDKR_VITA_LAN_STARTING &&
                   type == MDKR_LAN_PACKET_ARM &&
                   load_u64_be(packet + MDKR_LAN_PACKET_HEADER_SIZE) ==
                       s_descriptorDigest) {
            s_armReceived = 1;
            (void)send_digest_packet(MDKR_LAN_PACKET_ARM_ACK);
        } else if (s_role == MDKR_LAN_ROLE_HOST &&
                   s_state == MDKR_VITA_LAN_STARTING &&
                   type == MDKR_LAN_PACKET_ARM_ACK &&
                   load_u64_be(packet + MDKR_LAN_PACKET_HEADER_SIZE) ==
                       s_descriptorDigest) {
            s_startAcknowledged = 1;
            if (!s_startScheduled) {
                s_startTime = now + MDKR_LAN_START_DELAY_US;
                s_startScheduled = 1;
                set_carrier_status("LAN HOST: STARTING",
                                   "ONLINE: STARTING");
            }
            (void)send_digest_packet(MDKR_LAN_PACKET_GO);
        } else if (s_role == MDKR_LAN_ROLE_BROWSER &&
                   s_state == MDKR_VITA_LAN_STARTING &&
                   type == MDKR_LAN_PACKET_GO &&
                   load_u64_be(packet + MDKR_LAN_PACKET_HEADER_SIZE) ==
                       s_descriptorDigest) {
            if (!s_startScheduled) {
                s_startTime = now + MDKR_LAN_START_DELAY_US;
                s_startScheduled = 1;
                set_carrier_status("LAN JOIN: STARTING",
                                   "ONLINE: STARTING");
            }
        } else if (s_state == MDKR_VITA_LAN_RACING &&
                   type == MDKR_LAN_PACKET_INPUT &&
                   s_matchRuntimeOwned) {
            MdkrMatchInputBundle bundle;
            int bundleValid;
            unsigned int frame;
            unsigned int slot;
            bundleValid = mdkr_match_input_bundle_decode(
                packet + MDKR_LAN_PACKET_HEADER_SIZE,
                MDKR_MATCH_INPUT_BUNDLE_BYTES, &bundle);
            if (bundleValid &&
                bundle.slot_mask == s_remoteSlotMask &&
                bundle.match_epoch != s_launch.manifest.match_epoch) {
#if defined(MDKR_ENABLE_VITA_RELAY)
                if (s_carrier == MDKR_LAN_CARRIER_RELAY &&
                    s_controlPending &&
                    (s_control == MDKR_VITA_LAN_CONTROL_RESTART ||
                     s_control == MDKR_VITA_LAN_CONTROL_SELECT_TRACK) &&
                    bundle.match_epoch == s_controlMatchEpoch) {
                    if (!defer_input_bundle(&bundle, sequence)) {
                        char detail[112];
                        snprintf(detail, sizeof(detail),
                                 "DEFERRED INPUT OVERFLOW epoch=%u tick=%u "
                                 "count=%u",
                                 bundle.match_epoch, bundle.newest_tick,
                                 s_deferredInputBundleCount);
                        mdkr_vita_lan_report_failure_detail(
                            MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION,
                            detail);
                        return;
                    }
                    continue;
                }
                if (s_carrier == MDKR_LAN_CARRIER_RELAY &&
                    is_previous_match_epoch(
                        bundle.match_epoch,
                        s_launch.manifest.match_epoch)) {
                    continue;
                }
#endif
            }
            if (!bundleValid ||
                bundle.match_epoch != s_launch.manifest.match_epoch ||
                bundle.slot_mask != s_remoteSlotMask) {
#if defined(MDKR_ENABLE_VITA_RELAY)
                if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
                    char detail[112];
                    snprintf(detail, sizeof(detail),
                             "INVALID INPUT seq=%u decode=%u epoch=%u/%u "
                             "mask=%02X/%02X",
                             sequence, bundleValid ? 1u : 0u,
                             bundleValid ? bundle.match_epoch : 0u,
                             s_launch.manifest.match_epoch,
                             bundleValid
                                 ? (unsigned int)bundle.slot_mask
                                 : 0u,
                             (unsigned int)s_remoteSlotMask);
                    mdkr_vita_lan_report_failure_detail(
                        MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, detail);
                    return;
                }
#endif
                continue;
            }
            if (s_inputReceiveLogCount < 8u) {
                char line[128];
                snprintf(line, sizeof(line),
                         "lan: input-received tick=%u frames=%u mask=%u",
                         bundle.newest_tick,
                         (unsigned int)bundle.frame_count,
                         (unsigned int)bundle.slot_mask);
                mdkr_vita_boot_log(line);
                s_inputReceiveLogCount++;
            }
            for (frame = 0; frame < bundle.frame_count; frame++) {
                unsigned int tick = bundle.newest_tick - frame;
                for (slot = 0; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
                    if ((bundle.slot_mask & (1u << slot)) != 0) {
                        MdkrMatchTransportIngressResult ingress =
                            mdkr_match_transport_receive(
                            &s_matchTransport, bundle.match_epoch,
                            bundle.slot_mask, slot, tick,
                            &bundle.frames[frame][slot]);
                        if (ingress >= MDKR_MATCH_INGRESS_INVALID &&
                            s_inputRejectLogCount < 8u) {
                            char line[128];
                            snprintf(
                                line, sizeof(line),
                                "lan: input-rejected tick=%u current=%u "
                                "slot=%u reason=%u",
                                tick, s_matchTransport.history.current_tick,
                                slot, (unsigned int)ingress);
                            mdkr_vita_boot_log(line);
                            s_inputRejectLogCount++;
                        }
#if defined(MDKR_ENABLE_VITA_RELAY)
                        if (s_carrier == MDKR_LAN_CARRIER_RELAY &&
                            ingress >= MDKR_MATCH_INGRESS_INVALID) {
                            char detail[112];
                            snprintf(
                                detail, sizeof(detail),
                                "INPUT REJECT reason=%u tick=%u now=%u "
                                "slot=%u seq=%u",
                                (unsigned int)ingress, tick,
                                s_matchTransport.history.current_tick,
                                slot, sequence);
                            mdkr_vita_lan_report_failure_detail(
                                MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION,
                                detail);
                            return;
                        }
#endif
                    }
                }
            }
        }
    }
}

int mdkr_vita_lan_start_browser(void) {
    int result;
    mdkr_vita_lan_stop();
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (mdkr_vita_relay_state() != MDKR_VITA_RELAY_IDLE) {
        mdkr_vita_relay_stop();
    }
#endif
    result = open_socket(0);
    if (result < 0) {
        set_error("BROWSE", result);
        return result;
    }
    s_role = MDKR_LAN_ROLE_BROWSER;
    s_carrier = MDKR_LAN_CARRIER_UDP;
    s_state = MDKR_VITA_LAN_BROWSING;
    s_roomCount = 0;
    s_sequence = 0;
    s_lastSendTime = 0;
    snprintf(s_status, sizeof(s_status), "LAN: BROWSING");
    snprintf(s_roomLabel, sizeof(s_roomLabel), "SEARCHING...");
    log_result("browser-start", 0);
    return 0;
}

int mdkr_vita_lan_start_host(void) {
    int result;
    result = begin_room_name_dialog();
    log_result("room-name-begin", result);
    if (result < 0) {
        set_error("ROOM NAME", result);
    }
    return result;
}

static int start_host_socket(void) {
    int result;
    mdkr_vita_lan_stop();
    result = open_socket(MDKR_LAN_PORT);
    if (result < 0) {
        set_error("HOST", result);
        return result;
    }
    s_role = MDKR_LAN_ROLE_HOST;
    s_carrier = MDKR_LAN_CARRIER_UDP;
    s_state = MDKR_VITA_LAN_HOSTING;
    s_roomId = (unsigned int)sceKernelGetProcessTimeWide() ^ 0x47424C4Eu;
    if (s_roomId == 0) {
        s_roomId = 1;
    }
    s_sequence = 0;
    s_lastSendTime = 0;
    snprintf(s_status, sizeof(s_status), "LAN HOST: WAITING");
    log_result("host-start", 0);
    return 0;
}

static void finish_room_name_dialog(void) {
    SceImeDialogResult result;
    unsigned int index;
    unsigned int outputLength = 0;
    int hostResult;

    if (!s_imeTerminated) {
        if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            return;
        }
        memset(&result, 0, sizeof(result));
        s_imeApiResult = sceImeDialogGetResult(&result);
        s_imeDialogResult = result.result;
        s_imeButton = result.button;
        sceImeDialogTerm();
        s_imeTerminated = 1;
        return;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE) {
        return;
    }
    if (s_imeModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
        s_imeModuleOwned = 0;
    }
    if (s_imeApiResult < 0) {
        set_error("ROOM NAME", s_imeApiResult);
        return;
    }
    if (s_imeDialogResult != SCE_COMMON_DIALOG_RESULT_OK ||
        s_imeButton != SCE_IME_DIALOG_BUTTON_ENTER) {
        s_state = MDKR_VITA_LAN_BROWSING;
        snprintf(s_status, sizeof(s_status), "LAN: BROWSING");
        return;
    }

    for (index = 0; index < MDKR_LAN_ROOM_NAME_BYTES - 1 &&
                    s_imeOutput[index] != 0; index++) {
        SceWChar16 value = s_imeOutput[index];
        if (value >= 0x20 && value <= 0x7E) {
            s_roomName[outputLength++] = (char)value;
        }
    }
    if (outputLength == 0) {
        memcpy(s_roomName, "GOLDEN ROOM", sizeof("GOLDEN ROOM"));
    } else {
        s_roomName[outputLength] = '\0';
    }
    s_state = MDKR_VITA_LAN_BROWSING;
    hostResult = start_host_socket();
    log_result("room-name-finish", hostResult);
}

int mdkr_vita_lan_join_room(unsigned int index) {
    if (s_state != MDKR_VITA_LAN_BROWSING || index >= s_roomCount) {
        return -1;
    }
    s_roomAddress = s_rooms[index].address;
    s_roomId = s_rooms[index].roomId;
    s_state = MDKR_VITA_LAN_JOINING;
    s_lastSendTime = 0;
    snprintf(s_status, sizeof(s_status), "LAN JOIN: CONNECTING");
    log_result("join-start", 0);
    return 0;
}

#if defined(MDKR_ENABLE_VITA_RELAY)
int mdkr_vita_lan_start_relay(void) {
    const char *roomCode;
    unsigned int roomHash = 2166136261u;
    unsigned int index;
    int localSlot;
    if (s_state != MDKR_VITA_LAN_IDLE ||
        mdkr_vita_relay_state() != MDKR_VITA_RELAY_LOBBY ||
        mdkr_vita_relay_member_count() != 2u ||
        mdkr_vita_relay_member_mask() != 0x03u) {
        return -1;
    }
    localSlot = mdkr_vita_relay_local_slot();
    roomCode = mdkr_vita_relay_room_code();
    if (localSlot < 0 || localSlot > 1 || roomCode == NULL ||
        roomCode[0] == '\0') {
        return -1;
    }
    mdkr_vita_lan_stop();
    for (index = 0; roomCode[index] != '\0'; index++) {
        roomHash ^= (unsigned char)roomCode[index];
        roomHash *= 16777619u;
    }
    if (roomHash == 0u) {
        roomHash = 1u;
    }
    s_carrier = MDKR_LAN_CARRIER_RELAY;
    s_role = localSlot == 0 ? MDKR_LAN_ROLE_HOST
                            : MDKR_LAN_ROLE_BROWSER;
    s_roomId = roomHash;
    s_sequence = 0;
    s_lastSendTime = 0;
    s_lastReceiveTime = sceKernelGetProcessTimeWide();
    relay_slot_address((unsigned int)localSlot ^ 1u, &s_peerAddress);
    s_roomAddress = s_peerAddress;
    if (s_role == MDKR_LAN_ROLE_HOST) {
        s_state = MDKR_VITA_LAN_HOSTING;
    } else {
        s_state = MDKR_VITA_LAN_JOINING;
    }
    snprintf(s_status, sizeof(s_status), "ONLINE: CONNECTING GAMEPLAY");
    log_result("relay-start", localSlot);
    return 0;
}

int mdkr_vita_lan_is_relay(void) {
    return s_carrier == MDKR_LAN_CARRIER_RELAY;
}
#endif

void mdkr_vita_lan_update(void) {
    unsigned long long now;

    if (s_state == MDKR_VITA_LAN_NAMING) {
        finish_room_name_dialog();
        return;
    }
    if (s_state != MDKR_VITA_LAN_BROWSING &&
        s_state != MDKR_VITA_LAN_HOSTING &&
        s_state != MDKR_VITA_LAN_JOINING &&
        s_state != MDKR_VITA_LAN_CONNECTED &&
        s_state != MDKR_VITA_LAN_HANDSHAKING &&
        s_state != MDKR_VITA_LAN_CHARACTER_SELECT &&
        s_state != MDKR_VITA_LAN_STARTING &&
        s_state != MDKR_VITA_LAN_RACING) {
        return;
    }
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
        unsigned int expectedRemote =
            1u << ((unsigned int)mdkr_vita_relay_local_slot() ^ 1u);
        mdkr_vita_relay_update();
        if (mdkr_vita_relay_state() != MDKR_VITA_RELAY_LOBBY ||
            (mdkr_vita_relay_member_mask() & expectedRemote) == 0u) {
            const MdkrVitaLanFailure failure =
                mdkr_vita_relay_synchronization_lost()
                    ? MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION
                    : (s_role == MDKR_LAN_ROLE_HOST
                           ? MDKR_VITA_LAN_FAILURE_PEER_DISCONNECTED
                           : MDKR_VITA_LAN_FAILURE_HOST_LEFT);
            char detail[112];
            char relayDiagnostic[64];
            mdkr_vita_relay_format_diagnostics(
                relayDiagnostic, sizeof(relayDiagnostic));
            if (failure == MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION) {
                snprintf(detail, sizeof(detail), "RELAY %.104s",
                         mdkr_vita_relay_status());
            } else {
                snprintf(detail, sizeof(detail),
                         "RELAY %.32s mask=%02X need=%02X %.32s",
                         mdkr_vita_relay_status(),
                         mdkr_vita_relay_member_mask(), expectedRemote,
                         relayDiagnostic);
            }
            mdkr_vita_lan_report_failure_detail(failure, detail);
            return;
        }
    }
#endif
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    now = sceKernelGetProcessTimeWide();
    receive_packets(now);

    if (s_state == MDKR_VITA_LAN_BROWSING) {
        expire_rooms(now);
    }
    if ((s_state == MDKR_VITA_LAN_CONNECTED ||
         s_state == MDKR_VITA_LAN_HANDSHAKING ||
         s_state == MDKR_VITA_LAN_CHARACTER_SELECT ||
         s_state == MDKR_VITA_LAN_STARTING) &&
        now - s_lastReceiveTime > MDKR_LAN_PEER_TIMEOUT_US) {
#if defined(MDKR_ENABLE_VITA_RELAY)
        if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
            char detail[112];
            char relayDiagnostic[64];
            mdkr_vita_relay_format_diagnostics(
                relayDiagnostic, sizeof(relayDiagnostic));
            snprintf(detail, sizeof(detail),
                     "FRONTEND RX TIMEOUT age=%llums %s",
                     (now - s_lastReceiveTime) / 1000ULL,
                     relayDiagnostic);
            mdkr_vita_lan_report_failure_detail(
                MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, detail);
            return;
        }
#endif
        if (s_role == MDKR_LAN_ROLE_HOST) {
            s_state = MDKR_VITA_LAN_HOSTING;
            snprintf(s_status, sizeof(s_status), "LAN HOST: WAITING");
        } else {
            s_state = MDKR_VITA_LAN_BROWSING;
            s_roomCount = 0;
            snprintf(s_status, sizeof(s_status), "LAN: BROWSING");
            snprintf(s_roomLabel, sizeof(s_roomLabel), "SEARCHING...");
        }
        log_result("peer-timeout", 0);
    }
    if (s_state == MDKR_VITA_LAN_RACING &&
        now - s_lastReceiveTime > MDKR_LAN_PEER_TIMEOUT_US) {
        char detail[112];
        const MdkrVitaLanFailure failure =
            s_role == MDKR_LAN_ROLE_HOST
                ? MDKR_VITA_LAN_FAILURE_PEER_DISCONNECTED
                : MDKR_VITA_LAN_FAILURE_HOST_LEFT;
#if defined(MDKR_ENABLE_VITA_RELAY)
        if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
            char relayDiagnostic[64];
            mdkr_vita_relay_format_diagnostics(
                relayDiagnostic, sizeof(relayDiagnostic));
            snprintf(detail, sizeof(detail),
                     "RACE RX TIMEOUT age=%llums %s",
                     (now - s_lastReceiveTime) / 1000ULL,
                     relayDiagnostic);
        } else
#endif
        {
            snprintf(detail, sizeof(detail),
                     "RACE RX TIMEOUT age=%llums carrier=%u",
                     (now - s_lastReceiveTime) / 1000ULL,
                     (unsigned int)s_carrier);
        }
        mdkr_vita_lan_report_failure_detail(failure, detail);
        return;
    }
    if (s_state == MDKR_VITA_LAN_RACING && s_matchRuntimeOwned) {
        MdkrMatchRecovery recovery;
        request_input_repairs();
        digest_service();
        if (mdkr_match_transport_recovery(&s_matchTransport, &recovery)) {
            char line[128];
            snprintf(line, sizeof(line),
                     "lan: transport-recovery reason=%u first=%u "
                     "observed=%u slot=%u",
                     (unsigned int)recovery.reason,
                     recovery.first_unrecoverable_tick,
                     recovery.observed_at_tick,
                     (unsigned int)recovery.canonical_slot);
            mdkr_vita_boot_log(line);
            snprintf(line, sizeof(line),
                     "%s slot=%u first=%u now=%u age=%u",
                     recovery_reason_name(recovery.reason),
                     (unsigned int)recovery.canonical_slot,
                     recovery.first_unrecoverable_tick,
                     recovery.observed_at_tick,
                     recovery.observed_at_tick -
                         recovery.first_unrecoverable_tick);
            mdkr_vita_lan_report_failure_detail(
                MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION, line);
            return;
        }
    }

    if (s_state == MDKR_VITA_LAN_RACING && s_controlPending &&
        now - s_lastControlSendTime >= MDKR_LAN_CONTROL_INTERVAL_US) {
        unsigned char wireType = control_wire_type(s_control);
        s_lastControlSendTime = now;
        if (wireType != 0) {
            (void)send_control(
                wireType, s_controlTrackId, s_controlVehicleMask,
                s_controlMatchEpoch, s_controlId, 0);
        }
    } else if (s_state == MDKR_VITA_LAN_RACING &&
               s_finishLocalTick != 0u && !s_finishComplete &&
               now - s_lastFinishSendTime >=
                   MDKR_LAN_CONTROL_INTERVAL_US) {
        s_lastFinishSendTime = now;
        (void)send_control(
            MDKR_LAN_CONTROL_WIRE_FINISH_READY, 0, 0,
            s_launch.manifest.match_epoch, 0, s_finishLocalTick);
    } else if (s_state == MDKR_VITA_LAN_RACING &&
               s_role == MDKR_LAN_ROLE_HOST && s_trackPickerActive &&
               now - s_lastControlSendTime >=
                   MDKR_LAN_CURSOR_INTERVAL_US) {
        s_lastControlSendTime = now;
        (void)send_control(
            MDKR_LAN_CONTROL_WIRE_CURSOR, s_trackPickerTrackId, 0, 0, 0, 0);
    }
    if (s_state == MDKR_VITA_LAN_STARTING && s_startScheduled &&
        now >= s_startTime) {
        if (!install_match_runtime()) {
            set_error("MATCH", -1);
            return;
        }
        s_state = MDKR_VITA_LAN_RACING;
        s_raceLaunchPending = 1;
        digest_reset();
        {
            char line[96];
            snprintf(line, sizeof(line),
                     "lan: race-ready sim-fields=%d digest=%s epoch=%u",
                     platform_sim_tick_fields(),
                     s_carrier == MDKR_LAN_CARRIER_UDP ? "udp" : "off",
                     s_launch.manifest.match_epoch);
            mdkr_vita_boot_log(line);
        }
        set_carrier_status("LAN: RACE READY", "ONLINE: RACE READY");
        log_result("race-ready", 0);
        return;
    }
    if (s_state == MDKR_VITA_LAN_CHARACTER_SELECT &&
        now - s_lastCharacterSendTime >= MDKR_LAN_CHARACTER_INTERVAL_US) {
        s_lastCharacterSendTime = now;
        (void)send_character_selection();
    }

    if (s_state == MDKR_VITA_LAN_BROWSING &&
        now - s_lastSendTime >= MDKR_LAN_DISCOVERY_INTERVAL_US) {
        s_lastSendTime = now;
        send_discover();
    } else if (s_state == MDKR_VITA_LAN_JOINING &&
               now - s_lastSendTime >= MDKR_LAN_SEND_INTERVAL_US) {
        s_lastSendTime = now;
        send_packet(&s_roomAddress, MDKR_LAN_PACKET_JOIN, ++s_sequence,
                    s_roomId);
    } else if (s_state == MDKR_VITA_LAN_HANDSHAKING &&
               now - s_lastSendTime >= MDKR_LAN_SEND_INTERVAL_US) {
        s_lastSendTime = now;
        if (s_role == MDKR_LAN_ROLE_HOST) {
            send_packet(&s_peerAddress, MDKR_LAN_PACKET_ACCEPT,
                        ++s_sequence, s_roomId);
            (void)send_session_packet();
        }
    } else if (s_state == MDKR_VITA_LAN_STARTING &&
               now - s_lastSendTime >=
                   (s_startScheduled ? MDKR_LAN_START_INTERVAL_US
                                     : MDKR_LAN_SEND_INTERVAL_US)) {
        s_lastSendTime = now;
        if (s_role == MDKR_LAN_ROLE_HOST) {
            if (!s_finalSessionReady) {
                (void)send_session_packet();
            } else {
                (void)send_digest_packet(
                    s_startAcknowledged ? MDKR_LAN_PACKET_GO
                                        : MDKR_LAN_PACKET_ARM);
            }
        } else if (!s_startScheduled) {
            (void)send_digest_packet(
                s_armReceived ? MDKR_LAN_PACKET_ARM_ACK
                              : MDKR_LAN_PACKET_READY);
        }
    } else if ((s_state == MDKR_VITA_LAN_CONNECTED ||
                s_state == MDKR_VITA_LAN_CHARACTER_SELECT ||
                s_state == MDKR_VITA_LAN_RACING) &&
               now - s_lastSendTime >= MDKR_LAN_SEND_INTERVAL_US) {
        s_lastSendTime = now;
        send_packet(&s_peerAddress, MDKR_LAN_PACKET_PING, ++s_sequence,
                    s_roomId);
    }
}

int mdkr_vita_lan_should_stall_race(void) {
    unsigned int slot;
    unsigned int largestGap = 0;
    unsigned int largestGapFirst = 0;
    unsigned int largestGapSlot = 0xffu;
    unsigned int predictionLimit = MDKR_LAN_PREDICTION_STALL_TICKS;
    int transportBackpressured = 0;
    int stalled = 0;
    if (s_state == MDKR_VITA_LAN_RACING && s_matchRuntimeOwned) {
#if defined(MDKR_ENABLE_VITA_RELAY)
        if (s_carrier == MDKR_LAN_CARRIER_RELAY) {
            predictionLimit =
                MDKR_LAN_RELAY_PREDICTION_STALL_TICKS;
            if (!mdkr_vita_relay_send_ready(
                    MDKR_LAN_PACKET_HEADER_SIZE +
                    MDKR_MATCH_INPUT_BUNDLE_BYTES)) {
                transportBackpressured = 1;
                stalled = 1;
            }
        }
#endif
        for (slot = 0; slot < MDKR_NET_INPUT_SLOTS; slot++) {
            unsigned int first;
            unsigned int count;
            if (mdkr_match_transport_input_gap(
                    &s_matchTransport, slot, &first, &count) &&
                count >= predictionLimit) {
                stalled = 1;
                if (count > largestGap) {
                    largestGap = count;
                    largestGapFirst = first;
                    largestGapSlot = slot;
                }
            }
        }
    }
    if (stalled != s_raceInputStalled &&
        s_raceStallLogCount < 16u) {
        char line[128];
        snprintf(line, sizeof(line),
                 "lan: race-stall active=%d role=%u carrier=%u tick=%u "
                 "slot=%u first=%u gap=%u tx=%d",
                 stalled, (unsigned int)s_role, (unsigned int)s_carrier,
                 s_matchTransport.history.current_tick, largestGapSlot,
                 largestGapFirst, largestGap, transportBackpressured);
        mdkr_vita_boot_log(line);
        s_raceStallLogCount++;
    }
    s_raceInputStalled = stalled;
    return stalled;
}

void mdkr_vita_lan_stop(void) {
    enum MdkrLanCarrier carrier = s_carrier;
    if (s_state == MDKR_VITA_LAN_NAMING) {
        SceCommonDialogStatus dialogStatus = sceImeDialogGetStatus();
        if (dialogStatus == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            sceImeDialogAbort();
        }
        if (dialogStatus != SCE_COMMON_DIALOG_STATUS_NONE) {
            sceImeDialogTerm();
        }
        if (s_imeModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
            s_imeModuleOwned = 0;
        }
    }
    if (s_socket >= 0) {
        sceNetSocketClose(s_socket);
        s_socket = -1;
    }
    if (s_matchRuntimeOwned) {
        mdkr_match_input_runtime_clear();
        mdkr_net_roster_runtime_clear();
        s_matchRuntimeOwned = 0;
    }
    if (s_networkOwned) {
        mdkr_vita_network_release();
        s_networkOwned = 0;
    }
    s_state = MDKR_VITA_LAN_IDLE;
    s_role = MDKR_LAN_ROLE_NONE;
    s_carrier = MDKR_LAN_CARRIER_NONE;
    s_roomCount = 0;
    memset(s_rooms, 0, sizeof(s_rooms));
    memset(&s_roomAddress, 0, sizeof(s_roomAddress));
    memset(&s_peerAddress, 0, sizeof(s_peerAddress));
    memset(&s_launch, 0, sizeof(s_launch));
    memset(&s_matchBridge, 0, sizeof(s_matchBridge));
    memset(&s_matchTransport, 0, sizeof(s_matchTransport));
    s_descriptorDigest = 0;
    s_localSlotMask = 0;
    s_remoteSlotMask = 0;
    s_haveLaunch = 0;
    s_armReceived = 0;
    s_finalSessionReady = 0;
    s_startAcknowledged = 0;
    s_startScheduled = 0;
    s_startTime = 0;
    s_raceLaunchPending = 0;
    s_failure = MDKR_VITA_LAN_FAILURE_NONE;
    s_failurePending = 0;
    s_failureMessage[0] = '\0';
    s_control = MDKR_VITA_LAN_CONTROL_NONE;
    s_controlTrackId = 0;
    s_controlVehicleMask = 0;
    s_controlMatchEpoch = 0;
    s_controlIdCounter = 0;
    s_controlId = 0;
    s_lastReceivedControlId = 0;
    s_controlExecuteTime = 0;
    s_lastControlSendTime = 0;
    s_controlPending = 0;
    reset_finish_barrier();
    s_trackPickerActive = 0;
    s_trackPickerTrackId = 0;
    s_inputSendLogCount = 0;
    s_inputReceiveLogCount = 0;
    s_inputRejectLogCount = 0;
    reset_deferred_input_bundles();
    reset_input_repair();
    memset(s_character, 0, sizeof(s_character));
    memset(s_characterConfirmed, 0, sizeof(s_characterConfirmed));
    memset(s_characterRevision, 0, sizeof(s_characterRevision));
    s_frontendStage = MDKR_VITA_LAN_FRONTEND_CHARACTER_SELECT;
    s_frontendSelection = 0;
    s_characterUnlockMask = 0;
    s_characterUnlocksReady = 0;
    s_lastCharacterSendTime = 0;
#if defined(MDKR_ENABLE_VITA_RELAY)
    if (carrier == MDKR_LAN_CARRIER_RELAY) {
        mdkr_vita_relay_stop();
    }
#else
    (void)carrier;
#endif
}

MdkrVitaLanState mdkr_vita_lan_state(void) {
    return s_state;
}

int mdkr_vita_lan_is_host(void) {
    return s_state == MDKR_VITA_LAN_NAMING ||
           s_role == MDKR_LAN_ROLE_HOST;
}

unsigned int mdkr_vita_lan_room_count(void) {
    return s_roomCount;
}

const char *mdkr_vita_lan_status(void) {
    return s_status;
}

const char *mdkr_vita_lan_room_label(unsigned int index) {
    return index < s_roomCount ? s_rooms[index].label : s_roomLabel;
}

int mdkr_vita_lan_take_race_launch(MdkrMatchLaunchDescriptorV1 *launch) {
    if (!s_raceLaunchPending || launch == NULL ||
        s_state != MDKR_VITA_LAN_RACING) {
        return 0;
    }
    *launch = s_launch;
    s_raceLaunchPending = 0;
    return 1;
}

int mdkr_vita_lan_take_failure(MdkrVitaLanFailure *failure,
                               const char **message) {
    if (!s_failurePending || failure == NULL || message == NULL) {
        return 0;
    }
    *failure = s_failure;
    *message = s_failureMessage[0] != '\0'
                   ? s_failureMessage
                   : failure_message(s_failure);
    s_failurePending = 0;
    return 1;
}

int mdkr_vita_lan_request_control(MdkrVitaLanControl control,
                                  unsigned int trackId,
                                  unsigned int vehicleMask) {
    unsigned char wireType;
    unsigned int matchEpoch;
    unsigned int roleBits;
    if (s_state != MDKR_VITA_LAN_RACING ||
        control == MDKR_VITA_LAN_CONTROL_NONE ||
        s_controlPending ||
        ((control == MDKR_VITA_LAN_CONTROL_RESTART ||
          control == MDKR_VITA_LAN_CONTROL_SELECT_TRACK) &&
         s_role != MDKR_LAN_ROLE_HOST)) {
        return 0;
    }
    wireType = control_wire_type(control);
    if (wireType == 0) {
        return 0;
    }
    if (control == MDKR_VITA_LAN_CONTROL_RESTART) {
        trackId = s_launch.manifest.track_id;
        vehicleMask = s_launch.manifest.vehicle_mask;
    }
    if (control != MDKR_VITA_LAN_CONTROL_QUIT &&
        (trackId > 255u || vehicleMask == 0 ||
         (vehicleMask & ~0x07u) != 0)) {
        return 0;
    }
    matchEpoch = control == MDKR_VITA_LAN_CONTROL_QUIT
                     ? 0
                     : s_launch.manifest.match_epoch + 1u;
    if (matchEpoch == 0) {
        matchEpoch = 1;
    }
    roleBits =
        s_role == MDKR_LAN_ROLE_HOST ? 0x80000000u : 0x40000000u;
    s_controlIdCounter++;
    s_controlId = roleBits | (s_controlIdCounter & 0x3FFFFFFFu);
    reset_deferred_input_bundles();
    s_control = control;
    s_controlTrackId = trackId;
    s_controlVehicleMask = vehicleMask;
    s_controlMatchEpoch = matchEpoch;
    s_controlExecuteTime =
        sceKernelGetProcessTimeWide() + MDKR_LAN_CONTROL_DELAY_US;
    s_lastControlSendTime = 0;
    s_controlPending = 1;
    s_trackPickerActive = 0;
    (void)send_control(
        wireType, trackId, vehicleMask, matchEpoch, s_controlId,
        0);
    log_control("control-requested", wireType);
    return 1;
}

int mdkr_vita_lan_take_control(MdkrVitaLanControl *control,
                               unsigned int *trackId,
                               unsigned int *vehicleMask,
                               unsigned int *matchEpoch) {
    if (!s_controlPending || control == NULL || trackId == NULL ||
        vehicleMask == NULL || matchEpoch == NULL ||
        sceKernelGetProcessTimeWide() < s_controlExecuteTime) {
        return 0;
    }
    *control = s_control;
    *trackId = s_controlTrackId;
    *vehicleMask = s_controlVehicleMask;
    *matchEpoch = s_controlMatchEpoch;
    s_controlPending = 0;
    s_control = MDKR_VITA_LAN_CONTROL_NONE;
    return 1;
}

int mdkr_vita_lan_restart_race(unsigned int trackId,
                               unsigned int vehicleMask,
                               unsigned int matchEpoch,
                               MdkrMatchLaunchDescriptorV1 *launch) {
    unsigned int vehicle;
    if (s_state != MDKR_VITA_LAN_RACING || launch == NULL ||
        trackId > UINT16_MAX || matchEpoch == 0 || vehicleMask == 0 ||
        (vehicleMask & ~0x07u) != 0) {
        return 0;
    }
    if (s_matchRuntimeOwned) {
        mdkr_match_input_runtime_clear();
        mdkr_net_roster_runtime_clear();
        s_matchRuntimeOwned = 0;
    }
    s_launch.manifest.match_epoch = matchEpoch;
    s_launch.manifest.track_id = (unsigned short)trackId;
    s_launch.manifest.vehicle_mask = (unsigned char)vehicleMask;
    vehicle = (unsigned int)leveltable_vehicle_default((s32)trackId);
    if (vehicle > VEHICLE_PLANE ||
        (vehicleMask & (1u << vehicle)) == 0u) {
        vehicle = (vehicleMask & (1u << VEHICLE_CAR)) != 0u
                      ? VEHICLE_CAR
                      : ((vehicleMask &
                          (1u << VEHICLE_HOVERCRAFT)) != 0u
                             ? VEHICLE_HOVERCRAFT
                             : VEHICLE_PLANE);
    }
    s_launch.selections[0].vehicle_id = (unsigned char)vehicle;
    s_launch.selections[1].vehicle_id = (unsigned char)vehicle;
    s_launch.manifest.slot_owner[0] =
        ((unsigned long long)matchEpoch << 32) | 1u;
    s_launch.manifest.slot_owner[1] =
        ((unsigned long long)matchEpoch << 32) | 2u;
    s_launch.manifest.rng_seed =
        ((unsigned long long)matchEpoch << 32) ^
        0x444B525649544131ULL;
    s_descriptorDigest = mdkr_match_launch_descriptor_digest(&s_launch);
    s_haveLaunch =
        s_descriptorDigest != 0 &&
        mdkr_match_launch_descriptor_validate(&s_launch);
    if (!s_haveLaunch || !install_match_runtime()) {
        mdkr_vita_lan_report_failure(MDKR_VITA_LAN_FAILURE_STARTUP);
        return 0;
    }
    if (!apply_deferred_input_bundles(matchEpoch)) {
        return 0;
    }
    *launch = s_launch;
    s_firstRaceTick = 1;
    digest_reset();
    s_lastReceiveTime = sceKernelGetProcessTimeWide();
    snprintf(s_status, sizeof(s_status), "LAN: RACE RESTART");
    log_result("race-restart", 0);
    return 1;
}

int mdkr_vita_lan_finish_barrier_ready(unsigned int confirmedTick) {
    unsigned long long now;
    unsigned long long bothReadyTime;

    if (s_state != MDKR_VITA_LAN_RACING || !s_matchRuntimeOwned ||
        confirmedTick == 0u || s_finishComplete) {
        return s_finishComplete;
    }
    now = sceKernelGetProcessTimeWide();
    if (s_role == MDKR_LAN_ROLE_BROWSER) {
        if (s_finishRemoteTick == 0u) {
            return 0;
        }
        confirmedTick = s_finishRemoteTick;
    }
    if (s_finishLocalTick == 0u) {
        s_finishLocalTick = confirmedTick;
        s_finishLocalReadyTime = now;
        s_lastFinishSendTime = now;
        (void)send_control(
            MDKR_LAN_CONTROL_WIRE_FINISH_READY, 0, 0,
            s_launch.manifest.match_epoch, 0, s_finishLocalTick);
        log_result("finish-ready-local", (int)confirmedTick);
    }
    if (s_finishRemoteTick == 0u) {
        return 0;
    }
    bothReadyTime = s_finishLocalReadyTime > s_finishRemoteReadyTime
                        ? s_finishLocalReadyTime
                        : s_finishRemoteReadyTime;
    if (now - bothReadyTime < MDKR_LAN_FINISH_SETTLE_US) {
        return 0;
    }
    s_finishComplete = 1;
    log_result("finish-barrier-complete", (int)s_finishRemoteTick);
    return 1;
}

unsigned int mdkr_vita_lan_finish_target_tick(void) {
    if (s_state != MDKR_VITA_LAN_RACING) {
        return 0u;
    }
    return s_role == MDKR_LAN_ROLE_HOST
               ? s_finishLocalTick
               : s_finishRemoteTick;
}

void mdkr_vita_lan_set_track_picker(int active, unsigned int trackId) {
    if (s_state != MDKR_VITA_LAN_RACING ||
        s_role != MDKR_LAN_ROLE_HOST) {
        return;
    }
    s_trackPickerActive = active != 0;
    s_trackPickerTrackId = trackId;
    s_lastControlSendTime = 0;
    (void)send_control(
        s_trackPickerActive ? MDKR_LAN_CONTROL_WIRE_CURSOR
                            : MDKR_LAN_CONTROL_WIRE_CURSOR_CLOSE,
        trackId, 0, 0, 0, 0);
}

int mdkr_vita_lan_track_picker(unsigned int *trackId) {
    if (trackId == NULL || !s_trackPickerActive ||
        s_state != MDKR_VITA_LAN_RACING) {
        return 0;
    }
    *trackId = s_trackPickerTrackId;
    return 1;
}

#endif
