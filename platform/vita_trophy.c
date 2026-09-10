#include "vita_trophy.h"

#include "structs.h"

#ifdef __vita__

#include <psp2/appmgr.h>
#include <psp2/sysmodule.h>
#include <stdint.h>
#include <string.h>

/* VitaSDK has the SceNpTrophy import library but presently does not publish
 * its public C header. Keep the small ABI surface used by this port local. */
extern int sceNpTrophyInit(void *options);
extern int sceNpTrophyCreateContext(int *context, const void *commId,
                                    const void *commSignature, uint64_t options);
extern int sceNpTrophyCreateHandle(int *handle);
extern int sceNpTrophyUnlockTrophy(int context, int handle, int trophyId,
                                   int *platinumId);

enum {
    TROPHY_PLATINUM = 0,
    TROPHY_FIRST_BALLOON = 1,
    TROPHY_WIZPIG_AMULET = 2,
    TROPHY_KEY_DINO = 3,
    TROPHY_KEY_SHERBET = 4,
    TROPHY_KEY_SNOWFLAKE = 5,
    TROPHY_KEY_DRAGON = 6,
    TROPHY_BALLOONS_39 = 7,
    TROPHY_BALLOONS_47 = 8,
    TROPHY_CUP_DINO = 9,
    TROPHY_CUP_SHERBET = 10,
    TROPHY_CUP_SNOWFLAKE = 11,
    TROPHY_CUP_DRAGON = 12,
    TROPHY_CUP_FUTURE = 13,
    TROPHY_WIZPIG_1 = 14,
    TROPHY_WIZPIG_2 = 15,
};

static int sTrophyContext = -1;
static int sTrophyHandle = -1;
static uint32_t sSubmitted;
static int sUnavailable;

static int trophy_ready(void) {
    char communicationId[16] = "GBLN00001_01";
    /* NoTrpDrm bypasses the per-title signature verification, but the trophy
     * service still expects the normal signature header to be present. */
    static const unsigned char signature[160] = { 0xb9, 0xdd, 0xe1, 0x3b, 0x01, 0x00 };

    if (sUnavailable) return 0;
    if (sTrophyContext >= 0 && sTrophyHandle >= 0) return 1;
    /* sceNpTrophy is not automatically resident in a Vita homebrew. Calling
     * an unresolved service was the cause of the first test build's crash. */
    (void)sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    {
        char appParamCommunicationId[16] = { 0 };
        if (sceAppMgrAppParamGetString(0, 12, appParamCommunicationId,
                                       sizeof(appParamCommunicationId)) >= 0 &&
            strlen(appParamCommunicationId) == 12 &&
            appParamCommunicationId[9] == '_') {
            memcpy(communicationId, appParamCommunicationId,
                   sizeof(communicationId));
        }
    }
    if (sceNpTrophyInit(NULL) < 0 ||
        sceNpTrophyCreateContext(&sTrophyContext, communicationId, signature, 0) < 0 ||
        sceNpTrophyCreateHandle(&sTrophyHandle) < 0) {
        sUnavailable = 1;
        return 0;
    }
    return 1;
}

static void unlock(unsigned trophyId) {
    int platinumId = -1;
    uint32_t bit = 1u << trophyId;
    if ((sSubmitted & bit) != 0) return;
    /* An already-unlocked result is intentionally treated as submitted: the
     * system owns persistence, while this guard prevents an every-frame retry. */
    (void)sceNpTrophyUnlockTrophy(sTrophyContext, sTrophyHandle, (int)trophyId,
                                  &platinumId);
    sSubmitted |= bit;
}

void mdkr_vita_trophy_pump(const struct Settings *settings) {
    unsigned trophyState;
    if (settings == NULL || settings->balloonsPtr == NULL || settings->newGame ||
        !trophy_ready()) return;

    if (settings->balloonsPtr[0] >= 1) unlock(TROPHY_FIRST_BALLOON);
    if (settings->wizpigAmulet >= 4) unlock(TROPHY_WIZPIG_AMULET);
    if (settings->keys & 0x02) unlock(TROPHY_KEY_DINO);
    if (settings->keys & 0x04) unlock(TROPHY_KEY_SHERBET);
    if (settings->keys & 0x08) unlock(TROPHY_KEY_SNOWFLAKE);
    if (settings->keys & 0x10) unlock(TROPHY_KEY_DRAGON);
    if (settings->balloonsPtr[0] >= 39) unlock(TROPHY_BALLOONS_39);
    if (settings->balloonsPtr[0] >= 47) unlock(TROPHY_BALLOONS_47);

    trophyState = settings->trophies;
    if (((trophyState >> 0) & 3u) == 3u) unlock(TROPHY_CUP_DINO);
    if (((trophyState >> 2) & 3u) == 3u) unlock(TROPHY_CUP_SHERBET);
    if (((trophyState >> 4) & 3u) == 3u) unlock(TROPHY_CUP_SNOWFLAKE);
    if (((trophyState >> 6) & 3u) == 3u) unlock(TROPHY_CUP_DRAGON);
    if (((trophyState >> 8) & 3u) == 3u) unlock(TROPHY_CUP_FUTURE);
    if (settings->bosses & 0x001) unlock(TROPHY_WIZPIG_1);
    if (settings->bosses & 0x020) unlock(TROPHY_WIZPIG_2);
}

#else

void mdkr_vita_trophy_pump(const struct Settings *settings) {
    (void)settings;
}

#endif
