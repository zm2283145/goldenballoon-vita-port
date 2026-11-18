#include "vita_network_runtime.h"

#if defined(__vita__)

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#define MDKR_VITA_NET_MEMORY_SIZE (1024 * 1024)

static unsigned s_networkReferences;
static void *s_networkMemory;
static int s_ownsNetModule;
static int s_netInitialized;
static int s_netCtlInitialized;

static void mdkr_vita_network_cleanup(void) {
    if (s_netCtlInitialized) {
        sceNetCtlTerm();
        s_netCtlInitialized = 0;
    }
    if (s_netInitialized) {
        sceNetTerm();
        s_netInitialized = 0;
    }
    free(s_networkMemory);
    s_networkMemory = NULL;
    if (s_ownsNetModule) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        s_ownsNetModule = 0;
    }
}

int mdkr_vita_network_acquire(void) {
    SceNetInitParam init;
    int result;

    if (s_netInitialized) {
        s_networkReferences++;
        return 0;
    }

    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (result < 0) {
            return result;
        }
        s_ownsNetModule = 1;
    }

    s_networkMemory = memalign(64, MDKR_VITA_NET_MEMORY_SIZE);
    if (s_networkMemory == NULL) {
        mdkr_vita_network_cleanup();
        return -1;
    }

    memset(&init, 0, sizeof(init));
    init.memory = s_networkMemory;
    init.size = MDKR_VITA_NET_MEMORY_SIZE;
    init.flags = 0;
    result = sceNetInit(&init);
    if (result < 0) {
        mdkr_vita_network_cleanup();
        return result;
    }
    s_netInitialized = 1;

    result = sceNetCtlInit();
    if (result < 0) {
        mdkr_vita_network_cleanup();
        return result;
    }
    s_netCtlInitialized = 1;
    s_networkReferences = 1;
    return 0;
}

void mdkr_vita_network_release(void) {
    if (s_networkReferences == 0) {
        return;
    }
    s_networkReferences--;
}

int mdkr_vita_network_restore_infrastructure(void) {
    int result;
    if (!s_netInitialized || !s_netCtlInitialized) {
        return 0;
    }
    sceNetCtlTerm();
    s_netCtlInitialized = 0;
    result = sceNetCtlInit();
    if (result >= 0) {
        s_netCtlInitialized = 1;
    }
    return result;
}

void mdkr_vita_network_shutdown(void) {
    s_networkReferences = 0;
    mdkr_vita_network_cleanup();
}

#endif
