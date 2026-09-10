#include "vita_trophy.h"

#include "structs.h"
#include "asset_enums.h"
#include "menu.h"
#include "taj_mod.h"

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
extern int sceNpTrophyGetTrophyUnlockState(int context, int handle, void *state,
                                           uint32_t *count);

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
/* The completed set contains 98 trophies. Keep the local retry guard wide
 * enough for every ID; the Vita service remains the persistence authority. */
static uint32_t sSubmitted[(98 + 31) / 32];
static uint32_t sUnlocked[(98 + 31) / 32];
static int sUnavailable;
static int sLoggedSettings;
static int sLoggedPump;
static int sSetupComplete;
static int sTrophyServiceReady;
static unsigned char sAdventureBalloonCount[10];
static unsigned char sBonusAdventureBalloonCount[MOD_RACER_IDENTITY_COUNT];
static unsigned char sEditorCharacterBalloonCount[13];
static const unsigned char sEditorCharacterTrophyIds[13] = {
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 95, 96, 97
};

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

static int trophy_service_ready(void) {
    int result;
    if (sTrophyServiceReady) return 1;
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    trophy_log("module np_trophy=0x%08X", result);
    result = sceNpTrophyInit(NULL);
    trophy_log("init=0x%08X", result);
    if (result < 0) return 0;
    sTrophyServiceReady = 1;
    return 1;
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
    trophy_log("communication id=%s", communicationId);
    if (!trophy_service_ready()) goto unavailable;
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
    {
        uint32_t count = 0;
        result = sceNpTrophyGetTrophyUnlockState(sTrophyContext, sTrophyHandle,
                                                  sUnlocked, &count);
        trophy_log("unlock state=0x%08X count=%u", result, count);
        if (result < 0) memset(sUnlocked, 0, sizeof(sUnlocked));
    }
    return 1;
}

static void unlock(unsigned trophyId) {
    int platinumId = -1;
    int result;
    unsigned word;
    uint32_t bit;
    if (trophyId >= 98) return;
    word = trophyId / 32;
    bit = 1u << (trophyId % 32);
    if ((sSubmitted[word] & bit) != 0) return;
    /* An already-unlocked result is intentionally treated as submitted: the
     * system owns persistence, while this guard prevents an every-frame retry. */
    result = sceNpTrophyUnlockTrophy(sTrophyContext, sTrophyHandle, (int)trophyId,
                                     &platinumId);
    trophy_log("unlock id=%u result=0x%08X platinum=%d", trophyId, result,
               platinumId);
    if (result >= 0) sUnlocked[word] |= bit;
    sSubmitted[word] |= bit;
}

int mdkr_vita_trophy_is_unlocked(unsigned trophyId) {
    if (trophyId >= 98 || !trophy_ready()) return 0;
    return (sUnlocked[trophyId / 32] & (UINT32_C(1) << (trophyId % 32))) != 0;
}

int mdkr_vita_trophy_character_balloon_progress(unsigned characterIndex) {
    return characterIndex < ARRAY_COUNT(sEditorCharacterBalloonCount)
               ? sEditorCharacterBalloonCount[characterIndex] : 0;
}

void mdkr_vita_trophy_set_character_balloon_progress(unsigned characterIndex,
                                                      unsigned balloonCount) {
    if (characterIndex >= ARRAY_COUNT(sEditorCharacterBalloonCount)) return;
    if (balloonCount > 5) balloonCount = 5;
    sEditorCharacterBalloonCount[characterIndex] = (unsigned char)balloonCount;
    /* This mirrors the gameplay counter: reaching five is the condition, and
     * unlock() remains the sole service boundary. */
    if (balloonCount >= 5 && trophy_ready()) {
        unlock(sEditorCharacterTrophyIds[characterIndex]);
    }
}

/* RetroAchievements' display and the credits both use this exact canonical
 * order. The game gives us level IDs, so centralising the map keeps the
 * trophy IDs stable even though the level-header table is not ordered that
 * way. */
static int trophy_track_index(int levelId) {
    switch (levelId) {
        case ASSET_LEVEL_ANCIENTLAKE: return 0;
        case ASSET_LEVEL_FOSSILCANYON: return 1;
        case ASSET_LEVEL_JUNGLEFALLS: return 2;
        case ASSET_LEVEL_HOTTOPVOLCANO: return 3;
        case ASSET_LEVEL_EVERFROSTPEAK: return 4;
        case ASSET_LEVEL_WALRUSCOVE: return 5;
        case ASSET_LEVEL_SNOWBALLVALLEY: return 6;
        case ASSET_LEVEL_FROSTYVILLAGE: return 7;
        case ASSET_LEVEL_WHALEBAY: return 8;
        case ASSET_LEVEL_CRESCENTISLAND: return 9;
        case ASSET_LEVEL_PIRATELAGOON: return 10;
        case ASSET_LEVEL_TREASURECAVES: return 11;
        case ASSET_LEVEL_WINDMILLPLAINS: return 12;
        case ASSET_LEVEL_GREENWOODVILLAGE: return 13;
        case ASSET_LEVEL_BOULDERCANYON: return 14;
        case ASSET_LEVEL_HAUNTEDWOODS: return 15;
        case ASSET_LEVEL_SPACEDUSTALLEY: return 16;
        case ASSET_LEVEL_DARKMOONCAVERNS: return 17;
        case ASSET_LEVEL_SPACEPORTALPHA: return 18;
        case ASSET_LEVEL_STARCITY: return 19;
        default: return -1;
    }
}

void mdkr_vita_trophy_silver_coin_race(int levelId) {
    int track = trophy_track_index(levelId);
    if (track >= 0 && trophy_ready()) unlock(18 + (unsigned)track);
}

void mdkr_vita_trophy_tt_ghost_beaten(int levelId) {
    int track = trophy_track_index(levelId);
    if (track >= 0 && trophy_ready()) unlock(40 + (unsigned)track);
}

void mdkr_vita_trophy_developer_time(int levelId, int courseTime) {
    /* Credits' developer records in hundredths, in the same canonical order
     * as trophy_track_index(). Compare without rounding between the game's
     * 60 Hz race clock and the printed centisecond values. */
    static const unsigned short developerCentiseconds[20] = {
        5343, 8155, 5413, 8248, 6411, 7351, 8501, 5505,
        9763, 11660, 5781, 8801, 11115, 9180, 12316, 5825,
        12038, 12568, 11296, 11500
    };
    int track = trophy_track_index(levelId);
    if (track < 0 || courseTime < 0) return;
    if ((unsigned long)courseTime * 100UL <
        (unsigned long)developerCentiseconds[track] * 60UL && trophy_ready()) {
        unlock(60 + (unsigned)track);
    }
}

void mdkr_vita_trophy_banana_collected(int bananaCount) {
    if (bananaCount >= 10 && trophy_ready()) unlock(16);
}

void mdkr_vita_trophy_max_powerup(int balloonType, int balloonLevel) {
    /* Trophy IDs follow the displayed order Rocket, Boost, Mine, Shield,
     * Magnet, while the engine's BalloonType starts with Boost. */
    static const unsigned char trophyForBalloon[5] = { 91, 90, 92, 93, 94 };
    if ((unsigned)balloonType < 5 && balloonLevel >= 2 && trophy_ready()) {
        unlock(trophyForBalloon[balloonType]);
    }
}

void mdkr_vita_trophy_golden_balloon_collected(int characterId, int playerIndex) {
    static const unsigned char trophyForCharacter[10] = {
        80, 82, 85, 84, 87, 83, 88, 86, 89, 81
    };
    static const unsigned char trophyForBonusCharacter[MOD_RACER_IDENTITY_COUNT] = {
        0, 95, 96, 97
    };
    ModRacerIdentity identity = mod_racer_live_identity(playerIndex);

    /* Bonus racers use retail donor character IDs while racing. Consult the
     * live roster identity first so their balloon collection cannot unlock a
     * donor-character trophy instead of the Taj, Wizpig, or Terry trophy. */
    if (identity > MOD_RACER_RETAIL && identity < MOD_RACER_IDENTITY_COUNT) {
        if (sBonusAdventureBalloonCount[identity] < 5) {
            sBonusAdventureBalloonCount[identity]++;
        }
        if (sBonusAdventureBalloonCount[identity] >= 5 && trophy_ready()) {
            unlock(trophyForBonusCharacter[identity]);
        }
        return;
    }
    if ((unsigned)characterId >= 10) return;
    if (sAdventureBalloonCount[characterId] < 5) {
        sAdventureBalloonCount[characterId]++;
    }
    if (sAdventureBalloonCount[characterId] >= 5 && trophy_ready()) {
        unlock(trophyForCharacter[characterId]);
    }
}

void mdkr_vita_trophy_set_adventure_active(int active) {
    /* These achievements require five balloons in one uninterrupted Adventure
     * session. Reset on the game's own Tracks-mode boundary, not on loading a
     * hub or a race within Adventure. */
    if (!active) {
        memset(sAdventureBalloonCount, 0, sizeof(sAdventureBalloonCount));
        memset(sBonusAdventureBalloonCount, 0, sizeof(sBonusAdventureBalloonCount));
    }
}

void mdkr_vita_trophy_register(void) {
    /* Trophy setup owns the system-side title entry. Do it as the player
     * leaves Press Start, rather than making a first unlock create it as a
     * side effect. trophy_ready() is idempotent after setup succeeds. The
     * manifest's gid=001 bonus group is installed by this same dialog. */
    (void) trophy_ready();
}

void mdkr_vita_trophy_pump(const struct Settings *settings) {
    unsigned trophyState;
    int adventureTwo;
    if (!sLoggedPump) {
        trophy_log("trophy pump settings=%p balloons=%p newGame=%d", (void *) settings,
                   settings != NULL ? (void *) settings->balloonsPtr : NULL,
                   settings != NULL ? settings->newGame : -1);
        sLoggedPump = 1;
    }
    if (settings == NULL || settings->balloonsPtr == NULL || settings->newGame ||
        !trophy_ready()) return;

    adventureTwo = (settings->cutsceneFlags & CUTSCENE_ADVENTURE_TWO) != 0;

    if (!sLoggedSettings) {
        trophy_log("settings balloons=%d keys=0x%04X bosses=0x%04X cups=0x%04X amulet=%d",
                   settings->balloonsPtr[0], settings->keys, settings->bosses,
                   settings->trophies, settings->wizpigAmulet);
        sLoggedSettings = 1;
    }

    /* The native editor persists the four arena results in dedicated per-slot
     * flags: stock DKR omits Horseshoe Gulch from its serialized course list,
     * so map flags alone disappear after loading a save. */
    if (((u32) settings->cutsceneFlags & MDKR_VITA_ARENA_COMPLETE_MASK) ==
        MDKR_VITA_ARENA_COMPLETE_MASK) unlock(17);

    if (!adventureTwo) {
        if (settings->balloonsPtr[0] >= 1) unlock(TROPHY_FIRST_BALLOON);
        if (settings->wizpigAmulet >= 4) unlock(TROPHY_WIZPIG_AMULET);
        if (settings->keys & 0x02) unlock(TROPHY_KEY_DINO);
        if (settings->keys & 0x04) unlock(TROPHY_KEY_SHERBET);
        if (settings->keys & 0x08) unlock(TROPHY_KEY_SNOWFLAKE);
        if (settings->keys & 0x10) unlock(TROPHY_KEY_DRAGON);
        if (settings->balloonsPtr[0] >= 39) unlock(TROPHY_BALLOONS_39);
        if (settings->balloonsPtr[0] >= 47) unlock(TROPHY_BALLOONS_47);
    } else {
        if (settings->balloonsPtr[0] >= 47) unlock(38);
        if (settings->bosses & 0x020) unlock(39);
    }

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

void mdkr_vita_trophy_silver_coin_race(int levelId) { (void)levelId; }
void mdkr_vita_trophy_tt_ghost_beaten(int levelId) { (void)levelId; }
void mdkr_vita_trophy_developer_time(int levelId, int courseTime) {
    (void)levelId;
    (void)courseTime;
}
void mdkr_vita_trophy_banana_collected(int bananaCount) { (void)bananaCount; }
void mdkr_vita_trophy_max_powerup(int balloonType, int balloonLevel) {
    (void)balloonType;
    (void)balloonLevel;
}
void mdkr_vita_trophy_golden_balloon_collected(int characterId, int playerIndex) {
    (void)characterId;
    (void)playerIndex;
}
void mdkr_vita_trophy_set_adventure_active(int active) { (void)active; }
int mdkr_vita_trophy_is_unlocked(unsigned trophy_id) { (void)trophy_id; return 0; }
int mdkr_vita_trophy_character_balloon_progress(unsigned character_index) {
    (void)character_index; return 0;
}
void mdkr_vita_trophy_set_character_balloon_progress(unsigned character_index,
                                                      unsigned balloon_count) {
    (void)character_index; (void)balloon_count;
}

#endif
