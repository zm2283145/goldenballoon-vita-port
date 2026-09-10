#include "vita_trophy.h"

#include "structs.h"

#ifdef __vita__

#include <psp2/common_dialog.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* VitaSDK has the SceNpTrophy import library but presently does not publish
 * its public C header. Keep the small ABI surface used by this port local. */
extern int sceNpTrophyInit(void *options);
extern int sceNpTrophyCreateContext(int *context, const void *commId,
                                    const void *commSignature, uint64_t options);
extern int sceNpTrophyCreateHandle(int *handle);
extern int sceNpTrophySetupDialogInit(void *param);
extern SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
extern int sceNpTrophySetupDialogTerm(void);
extern int sceNpTrophyUnlockTrophy(int context, int handle, int trophyId,
                                   int *platinumId);

/* VitaSDK does not currently expose SceNpTrophy's dialog definitions. This
 * layout is the one used by established vitaGL homebrew trophy integrations. */
typedef struct SceNpTrophySetupDialogParam {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    uint8_t reserved[128];
} SceNpTrophySetupDialogParam;

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
static int sLoggedSettings;
static int sLoggedPump;
static int sSetupComplete;

/* Use the port's existing, file-gated boot log. An empty
 * ux0:data/goldenballoon/debug file enables these diagnostics, and all output
 * joins the normal mdkr_boot.log rather than creating a second log. */
extern void mdkr_vita_boot_log(const char *msg);
extern void mdkr_vita_boot_log_flush(void);
extern int mdkr_vita_debug_enabled(void);

static void trophy_log(const char *format, ...) {
    char line[256];
    va_list args;
    if (!mdkr_vita_debug_enabled()) return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    mdkr_vita_boot_log(line);
    /* Unlike the renderer diagnostics, this bridge may be the only debug
     * producer during a short test run. Persist each line immediately so an
     * app exit or a crash cannot hide the actual trophy-service return code. */
    mdkr_vita_boot_log_flush();
}

static int trophy_ready(void) {
    /* sceNpTrophyCreateContext takes the nine-character title ID. The
     * manifest's <npcommid> retains its distinct 12-character _00/_01
     * suffix, but passing that suffix to this API causes setup to reject the
     * otherwise valid archive. */
    /* sceNpTrophy's undocumented ABI reads the fixed 12-byte communication
     * identifier buffer. Keep the trailing three bytes zeroed, as in the
     * established FF4A and Ghostship integrations, rather than passing a
     * shorter C string with unspecified bytes beyond its terminator. */
    static const char communicationId[12] = "GBLN00001";
    /* NoTrpDrm bypasses the per-title signature verification, but the trophy
     * service still expects the normal signature header to be present. */
    static const unsigned char signature[160] = { 0xb9, 0xdd, 0xe1, 0x3b, 0x01, 0x00 };

    int result;
    if (sUnavailable) return 0;
    if (sTrophyContext >= 0 && sTrophyHandle >= 0) return 1;
    /* sceNpTrophy is not automatically resident in a Vita homebrew. Calling
     * an unresolved service was the cause of the first test build's crash. */
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    trophy_log("module np_trophy=0x%08X", result);
    trophy_log("communication id=%s", communicationId);
    result = sceNpTrophyInit(NULL);
    trophy_log("init=0x%08X", result);
    if (result < 0) goto unavailable;
    result = sceNpTrophyCreateContext(&sTrophyContext, communicationId, signature, 0);
    trophy_log("create context=0x%08X context=%d", result, sTrophyContext);
    if (result < 0) goto unavailable;
    if (!sSetupComplete) {
        SceNpTrophySetupDialogParam setupParam;
        SceCommonDialogStatus setupStatus;
        memset(&setupParam, 0, sizeof(setupParam));
        _sceCommonDialogSetMagicNumber(&setupParam.commonParam);
        setupParam.sdkVersion = PSP2_SDK_VERSION;
        setupParam.context = sTrophyContext;
        result = sceNpTrophySetupDialogInit(&setupParam);
        trophy_log("setup dialog init=0x%08X", result);
        if (result < 0) goto unavailable;
        do {
            setupStatus = sceNpTrophySetupDialogGetStatus();
            if (setupStatus == SCE_COMMON_DIALOG_STATUS_RUNNING) {
                vglSwapBuffers(GL_TRUE);
            }
        } while (setupStatus == SCE_COMMON_DIALOG_STATUS_RUNNING);
        trophy_log("setup dialog status=%d", (int) setupStatus);
        result = sceNpTrophySetupDialogTerm();
        trophy_log("setup dialog term=0x%08X", result);
        if (result < 0 || setupStatus != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            goto unavailable;
        }
        sSetupComplete = 1;
    }
    result = sceNpTrophyCreateHandle(&sTrophyHandle);
    trophy_log("create handle=0x%08X handle=%d", result, sTrophyHandle);
    if (result < 0) {
unavailable:
        trophy_log("trophy service unavailable");
        sUnavailable = 1;
        return 0;
    }
    return 1;
}

static void unlock(unsigned trophyId) {
    int platinumId = -1;
    int result;
    uint32_t bit = 1u << trophyId;
    if ((sSubmitted & bit) != 0) return;
    /* An already-unlocked result is intentionally treated as submitted: the
     * system owns persistence, while this guard prevents an every-frame retry. */
    result = sceNpTrophyUnlockTrophy(sTrophyContext, sTrophyHandle, (int)trophyId,
                                     &platinumId);
    trophy_log("unlock id=%u result=0x%08X platinum=%d", trophyId, result,
               platinumId);
    sSubmitted |= bit;
}

void mdkr_vita_trophy_register(void) {
    /* Trophy setup owns the system-side title entry. Do it as the player
     * leaves Press Start, rather than making a first unlock create it as a
     * side effect. trophy_ready() is idempotent after setup succeeds. */
    (void) trophy_ready();
}

void mdkr_vita_trophy_pump(const struct Settings *settings) {
    unsigned trophyState;
    if (!sLoggedPump) {
        trophy_log("trophy pump settings=%p balloons=%p newGame=%d", (void *) settings,
                   settings != NULL ? (void *) settings->balloonsPtr : NULL,
                   settings != NULL ? settings->newGame : -1);
        sLoggedPump = 1;
    }
    if (settings == NULL || settings->balloonsPtr == NULL || settings->newGame ||
        !trophy_ready()) return;

    if (!sLoggedSettings) {
        trophy_log("settings balloons=%d keys=0x%04X bosses=0x%04X cups=0x%04X amulet=%d",
                   settings->balloonsPtr[0], settings->keys, settings->bosses,
                   settings->trophies, settings->wizpigAmulet);
        sLoggedSettings = 1;
    }

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

void mdkr_vita_trophy_register(void) {}

void mdkr_vita_trophy_pump(const struct Settings *settings) {
    (void)settings;
}

#endif
