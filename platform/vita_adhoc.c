#include "vita_adhoc.h"

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_ADHOC)

#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/power.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>

#include "vita_network_runtime.h"

extern void mdkr_vita_boot_log(const char *message);
extern void mdkr_vita_boot_log_flush(void);

#define MDKR_ADHOC_PORT 31415
#define MDKR_ADHOC_PDP_BUFFER_SIZE (32 * 1024)
#define MDKR_ADHOC_PACKET_SIZE 20
#define MDKR_ADHOC_PROTOCOL_VERSION 2
#define MDKR_ADHOC_SEND_INTERVAL_US 500000ULL
#define MDKR_ADHOC_PEER_TIMEOUT_US 5000000ULL
#define MDKR_ADHOC_MAX_RECEIVES_PER_UPDATE 8

#ifndef MDKR_VERSION_STRING
#define MDKR_VERSION_STRING "dev"
#endif

enum MdkrAdhocPacketType {
    MDKR_ADHOC_PACKET_HELLO = 1,
    MDKR_ADHOC_PACKET_PING = 2,
    MDKR_ADHOC_PACKET_ACK = 3
};

static const unsigned char s_packetMagic[4] = { 'D', 'K', 'R', 'A' };
static const char s_productId[SCE_NET_ADHOCCTL_ADHOCID_LEN] = {
    'D', 'K', 'R', 'V', 'I', 'T', 'A', '0', '1'
};

static MdkrVitaAdhocState s_state;
static char s_status[48] = "AD-HOC";
static int s_pdpSocket = -1;
static int s_networkOwned;
static int s_adhocModuleOwned;
static int s_adhocInitialized;
static int s_adhocCtlInitialized;
static SceNetEtherAddr s_localMac;
static SceNetEtherAddr s_peerMac;
static unsigned int s_sequence;
static unsigned long long s_lastSendTime;
static unsigned long long s_lastReceiveTime;
static int s_dialogTerminated;
static int s_dialogResult;

static void log_result(const char *stage, int result) {
    char line[96];
    snprintf(line, sizeof(line), "adhoc: %s result=0x%08X", stage,
             (unsigned int)result);
    mdkr_vita_boot_log(line);
    mdkr_vita_boot_log_flush();
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

static unsigned int load_u16_be(const unsigned char *input) {
    return ((unsigned int)input[0] << 8) | input[1];
}

static unsigned int load_u32_be(const unsigned char *input) {
    return ((unsigned int)input[0] << 24) |
           ((unsigned int)input[1] << 16) |
           ((unsigned int)input[2] << 8) | input[3];
}

static void set_error(const char *stage, int result) {
    snprintf(s_status, sizeof(s_status), "AD-HOC ERROR %s %08X", stage,
             (unsigned int)result);
    s_state = MDKR_VITA_ADHOC_ERROR;
}

static int mac_equal(const SceNetEtherAddr *left,
                     const SceNetEtherAddr *right) {
    return memcmp(left->data, right->data, sizeof(left->data)) == 0;
}

static int mac_valid(const SceNetEtherAddr *address) {
    static const unsigned char zero[6] = { 0 };
    return memcmp(address->data, zero, sizeof(zero)) != 0;
}

static int packet_encode(unsigned char type, unsigned int sequence,
                         unsigned char output[MDKR_ADHOC_PACKET_SIZE]) {
    char version[8] = { 0 };
    memcpy(output, s_packetMagic, sizeof(s_packetMagic));
    output[4] = MDKR_ADHOC_PROTOCOL_VERSION;
    output[5] = type;
    store_u16_be(output + 6, MDKR_ADHOC_PACKET_SIZE);
    store_u32_be(output + 8, sequence);
    snprintf(version, sizeof(version), "%s", MDKR_VERSION_STRING);
    memcpy(output + 12, version, sizeof(version));
    return MDKR_ADHOC_PACKET_SIZE;
}

static int packet_decode(const unsigned char *packet, int length,
                         unsigned char *type, unsigned int *sequence) {
    char version[8] = { 0 };
    snprintf(version, sizeof(version), "%s", MDKR_VERSION_STRING);
    if (length != MDKR_ADHOC_PACKET_SIZE ||
        memcmp(packet, s_packetMagic, sizeof(s_packetMagic)) != 0 ||
        packet[4] != MDKR_ADHOC_PROTOCOL_VERSION ||
        load_u16_be(packet + 6) != MDKR_ADHOC_PACKET_SIZE ||
        packet[5] < MDKR_ADHOC_PACKET_HELLO ||
        packet[5] > MDKR_ADHOC_PACKET_ACK ||
        memcmp(packet + 12, version, sizeof(version)) != 0) {
        return 0;
    }
    *type = packet[5];
    *sequence = load_u32_be(packet + 8);
    return 1;
}

static int send_packet(const SceNetEtherAddr *peer, unsigned char type,
                       unsigned int sequence) {
    unsigned char packet[MDKR_ADHOC_PACKET_SIZE];
    int length = packet_encode(type, sequence, packet);
    int result = sceNetAdhocPdpSend(s_pdpSocket, peer, MDKR_ADHOC_PORT,
                                    packet, length, 0,
                                    SCE_NET_ADHOC_F_NONBLOCK);
    if (result == (int)SCE_ERROR_NET_ADHOC_WOULD_BLOCK) {
        return 0;
    }
    return result;
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

static int begin_connection_dialog(void) {
    SceCommonDialogConfigParam common;
    SceNetCheckDialogParam parameter;
    SceNetAdhocctlGroupName groupName;
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

    sceNetCheckDialogParamInit(&parameter);
    memset(&groupName, 0, sizeof(groupName));
    parameter.groupName = &groupName;
    memcpy(parameter.npCommunicationId.data, s_productId,
           sizeof(parameter.npCommunicationId.data));
    parameter.npCommunicationId.term = '\0';
    parameter.npCommunicationId.num = 0;
    parameter.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    parameter.timeoutUs = 0;
    status = sceNetCheckDialogInit(&parameter);
    if (status < 0) {
        return status;
    }
    s_dialogTerminated = 0;
    s_dialogResult = SCE_COMMON_DIALOG_RESULT_ABORTED;
    return 0;
}

static void send_discovery_packets(void) {
    SceNetAdhocctlPeerInfo peers[8];
    SceNetAdhocctlPeerInfo *peer;
    int length = sizeof(peers);
    int count = 0;

    memset(peers, 0, sizeof(peers));
    if (sceNetAdhocctlGetPeerList(&length, peers) < 0) {
        return;
    }
    for (peer = peers; peer != NULL && count < 8; peer = peer->next, count++) {
        if (mac_valid(&peer->macAddr) &&
            !mac_equal(&peer->macAddr, &s_localMac)) {
            send_packet(&peer->macAddr, MDKR_ADHOC_PACKET_HELLO,
                        ++s_sequence);
        }
    }
}

static void receive_packets(unsigned long long now) {
    int receiveCount;
    for (receiveCount = 0;
         receiveCount < MDKR_ADHOC_MAX_RECEIVES_PER_UPDATE;
         receiveCount++) {
        unsigned char packet[MDKR_ADHOC_PACKET_SIZE];
        SceNetEtherAddr source;
        SceUShort16 sourcePort = 0;
        unsigned char type;
        unsigned int sequence;
        int length = sizeof(packet);
        int result = sceNetAdhocPdpRecv(s_pdpSocket, &source, &sourcePort,
                                        packet, &length, 0,
                                        SCE_NET_ADHOC_F_NONBLOCK);
        if (result == (int)SCE_ERROR_NET_ADHOC_WOULD_BLOCK) {
            break;
        }
        if (result < 0 || sourcePort != MDKR_ADHOC_PORT ||
            !mac_valid(&source) || mac_equal(&source, &s_localMac) ||
            !packet_decode(packet, length, &type, &sequence)) {
            continue;
        }

        if (s_state == MDKR_VITA_ADHOC_CONNECTED &&
            !mac_equal(&source, &s_peerMac)) {
            continue;
        }
        s_peerMac = source;
        s_lastReceiveTime = now;
        if (type == MDKR_ADHOC_PACKET_HELLO ||
            type == MDKR_ADHOC_PACKET_PING) {
            send_packet(&source, MDKR_ADHOC_PACKET_ACK, sequence);
        }
        if (s_state != MDKR_VITA_ADHOC_CONNECTED) {
            s_state = MDKR_VITA_ADHOC_CONNECTED;
            snprintf(s_status, sizeof(s_status), "AD-HOC: CONNECTED");
            send_packet(&source, MDKR_ADHOC_PACKET_ACK, sequence);
        }
    }
}

int mdkr_vita_adhoc_start(void) {
    SceNetAdhocctlAdhocId product;
    int result;

    if (s_state == MDKR_VITA_ADHOC_SEARCHING ||
        s_state == MDKR_VITA_ADHOC_CONNECTED) {
        return 0;
    }
    log_result("start", 0);
    mdkr_vita_adhoc_stop();

    result = mdkr_vita_network_acquire();
    log_result("network-acquire", result);
    if (result < 0) {
        set_error("NET", result);
        return result;
    }
    s_networkOwned = 1;

    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_PSPNET_ADHOC) !=
        SCE_SYSMODULE_LOADED) {
        result = sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC);
        log_result("module-load", result);
        if (result < 0) {
            mdkr_vita_adhoc_stop();
            set_error("MODULE", result);
            return result;
        }
        s_adhocModuleOwned = 1;
    }

    result = sceNetAdhocInit();
    log_result("adhoc-init", result);
    if (result < 0) {
        mdkr_vita_adhoc_stop();
        set_error("INIT", result);
        return result;
    }
    s_adhocInitialized = 1;

    memset(&product, 0, sizeof(product));
    product.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
    memcpy(product.data, s_productId, sizeof(product.data));
    result = sceNetAdhocctlInit(&product);
    log_result("adhocctl-init", result);
    if (result < 0) {
        mdkr_vita_adhoc_stop();
        set_error("CTL", result);
        return result;
    }
    s_adhocCtlInitialized = 1;

    result = begin_connection_dialog();
    log_result("connection-dialog-begin", result);
    if (result < 0) {
        mdkr_vita_adhoc_stop();
        set_error("DIALOG", result);
        return result;
    }
    s_state = MDKR_VITA_ADHOC_CONNECTING;
    snprintf(s_status, sizeof(s_status), "AD-HOC: CONNECTING");
    return 0;
}

static void finish_connection_dialog(void) {
    SceNetCheckDialogResult dialogResult;
    int result;

    if (!s_dialogTerminated) {
        if (sceNetCheckDialogGetStatus() !=
            SCE_COMMON_DIALOG_STATUS_FINISHED) {
            return;
        }
        memset(&dialogResult, 0, sizeof(dialogResult));
        result = sceNetCheckDialogGetResult(&dialogResult);
        log_result("connection-dialog-result", result);
        s_dialogResult = result < 0 ? result : dialogResult.result;
        sceNetCheckDialogTerm();
        s_dialogTerminated = 1;
        return;
    }
    if (sceNetCheckDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE) {
        return;
    }
    if (s_dialogResult != SCE_COMMON_DIALOG_RESULT_OK) {
        result = s_dialogResult;
        mdkr_vita_adhoc_stop();
        if (result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED) {
            snprintf(s_status, sizeof(s_status), "AD-HOC: CANCELED");
        } else {
            set_error("DIALOG", result);
        }
        return;
    }

    result = sceNetAdhocctlGetEtherAddr(&s_localMac);
    log_result("local-mac", result);
    if (result < 0) {
        mdkr_vita_adhoc_stop();
        set_error("MAC", result);
        return;
    }
    s_pdpSocket = sceNetAdhocPdpCreate(&s_localMac, MDKR_ADHOC_PORT,
                                       MDKR_ADHOC_PDP_BUFFER_SIZE, 0);
    log_result("pdp-create", s_pdpSocket);
    if (s_pdpSocket < 0) {
        result = s_pdpSocket;
        s_pdpSocket = -1;
        mdkr_vita_adhoc_stop();
        set_error("PDP", result);
        return;
    }

    s_sequence = 0;
    s_lastSendTime = 0;
    s_lastReceiveTime = 0;
    s_state = MDKR_VITA_ADHOC_SEARCHING;
    snprintf(s_status, sizeof(s_status), "AD-HOC: SEARCHING");
    log_result("searching", 0);
}

void mdkr_vita_adhoc_update(void) {
    unsigned long long now;

    if (s_state == MDKR_VITA_ADHOC_CONNECTING) {
        finish_connection_dialog();
        return;
    }
    if (s_state != MDKR_VITA_ADHOC_SEARCHING &&
        s_state != MDKR_VITA_ADHOC_CONNECTED) {
        return;
    }
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    now = sceKernelGetProcessTimeWide();
    receive_packets(now);

    if (s_state == MDKR_VITA_ADHOC_CONNECTED &&
        now - s_lastReceiveTime > MDKR_ADHOC_PEER_TIMEOUT_US) {
        s_state = MDKR_VITA_ADHOC_SEARCHING;
        snprintf(s_status, sizeof(s_status), "AD-HOC: SEARCHING");
    }
    if (now - s_lastSendTime < MDKR_ADHOC_SEND_INTERVAL_US) {
        return;
    }
    s_lastSendTime = now;
    if (s_state == MDKR_VITA_ADHOC_CONNECTED) {
        send_packet(&s_peerMac, MDKR_ADHOC_PACKET_PING, ++s_sequence);
    } else {
        send_discovery_packets();
    }
}

void mdkr_vita_adhoc_stop(void) {
    if (s_state == MDKR_VITA_ADHOC_CONNECTING) {
        SceCommonDialogStatus dialogStatus = sceNetCheckDialogGetStatus();
        if (dialogStatus == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            sceNetCheckDialogAbort();
        }
        if (dialogStatus != SCE_COMMON_DIALOG_STATUS_NONE) {
            sceNetCheckDialogTerm();
        }
    }
    if (s_pdpSocket >= 0) {
        sceNetAdhocPdpDelete(s_pdpSocket, 0);
        s_pdpSocket = -1;
    }
    if (s_adhocCtlInitialized) {
        sceNetCtlAdhocDisconnect();
        sceNetAdhocctlTerm();
        s_adhocCtlInitialized = 0;
    }
    if (s_adhocInitialized) {
        sceNetAdhocTerm();
        s_adhocInitialized = 0;
    }
    if (s_adhocModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
        s_adhocModuleOwned = 0;
    }
    if (s_networkOwned) {
        log_result("restore-infrastructure",
                   mdkr_vita_network_restore_infrastructure());
        mdkr_vita_network_release();
        s_networkOwned = 0;
    }
    memset(&s_localMac, 0, sizeof(s_localMac));
    memset(&s_peerMac, 0, sizeof(s_peerMac));
    s_state = MDKR_VITA_ADHOC_IDLE;
}

MdkrVitaAdhocState mdkr_vita_adhoc_state(void) {
    return s_state;
}

const char *mdkr_vita_adhoc_status(void) {
    return s_status;
}

#endif
