/** enhancement_registry.c — see enhancement_registry.h. */
#include "enhancement_registry.h"

#include <stdio.h>

static const MdkrEnhancement s_enhancements[] = {
    {
        MDKR_ENH_SPEEDOMETER,
        "Speedometer",
        "Shows your current speed in the corner of the screen while you race.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "1",   /* mph readout on */
        MDKR_ENH_PROOF_SOLO_RACE,
    },
    {
        MDKR_ENH_DRAW_DISTANCE,
        "Draw distance",
        "Draws scenery and pickups (bananas, coins, balloons) further ahead "
        "so they stop popping in near the horizon. Maximum removes distance "
        "pop-in entirely -- recommended for split-screen coin challenges.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "400", /* the old top of the range; still valid now that it's 100..1600 */
        MDKR_ENH_PROOF_SOLO_RACE,
    },
    {
        MDKR_ENH_LOD_BIAS,
        "Model detail",
        "Keeps higher-detail models on screen further into the distance.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "2",   /* hold high detail furthest out */
        MDKR_ENH_PROOF_SOLO_RACE,
    },
    {
        MDKR_ENH_AI_DIFFICULTY,
        "Opponent skill",
        "Makes the other racers push harder, for a tougher replay once "
        "you've beaten the game.",
        MDKR_ENH_GAMEPLAY,
        MDKR_ENH_CAT_DIFFICULTY,
        "brutal",
        MDKR_ENH_PROOF_SOLO_RACE,
    },
    {
        MDKR_ENH_ADVENTURE_PARTY,
        "Adventure Party",
        "Lets two to four local players explore and race together in "
        "Adventure. With it on, this setting owns Adventure admission for "
        "those players; the JOINTVENTURE magic code does not add lead swapping "
        "on top.",
        MDKR_ENH_GAMEPLAY,
        MDKR_ENH_CAT_MULTIPLAYER,
        "1",   /* on: two-to-four-player Adventure admission */
        MDKR_ENH_PROOF_ADVENTURE_PARTY_3P,
    },
};

#define MDKR_ENHANCEMENT_COUNT \
    ((int)(sizeof(s_enhancements) / sizeof(s_enhancements[0])))

int mdkr_enhancement_count(void) { return MDKR_ENHANCEMENT_COUNT; }

const MdkrEnhancement *mdkr_enhancement_at(int index) {
    if (index < 0 || index >= MDKR_ENHANCEMENT_COUNT)
        return NULL;
    return &s_enhancements[index];
}

const MdkrEnhancement *mdkr_enhancement_for_key(MdkrVideoKey key) {
    for (int i = 0; i < MDKR_ENHANCEMENT_COUNT; i++) {
        if (s_enhancements[i].key == key)
            return &s_enhancements[i];
    }
    return NULL;
}

static const char *authority_name(MdkrEnhAuthority authority) {
    return authority == MDKR_ENH_GAMEPLAY ? "gameplay" : "presentation";
}

static const char *category_name(MdkrEnhCategory category) {
    switch (category) {
    case MDKR_ENH_CAT_DISPLAY:     return "display";
    case MDKR_ENH_CAT_DIFFICULTY:  return "difficulty";
    case MDKR_ENH_CAT_COSMETIC:    return "cosmetic";
    case MDKR_ENH_CAT_MULTIPLAYER: return "multiplayer";
    default:                       return "unknown";
    }
}

static const char *proof_profile_name(MdkrEnhProofProfile profile) {
    switch (profile) {
    case MDKR_ENH_PROOF_SOLO_RACE:          return "solo_race";
    case MDKR_ENH_PROOF_ADVENTURE_PARTY_3P: return "adventure_party_3p";
    default:                                return "unknown";
    }
}

void mdkr_enhancement_dump_table(void) {
    for (int i = 0; i < MDKR_ENHANCEMENT_COUNT; i++) {
        const MdkrEnhancement *e = &s_enhancements[i];
        const MdkrVideoSchema *schema = mdkr_video_schema(e->key);
        /* The schema name, not the label: the gate has to set this key through
         * the same string the ini and --video-set accept, and a player-facing
         * label is not that string. The proof profile rides along so the gate
         * dispatches on the row's own declaration, not a copy in the test. */
        printf("[ENHTABLE] key=%s authority=%s category=%s probe=%s profile=%s\n",
               schema != NULL ? schema->name : "?",
               authority_name(e->authority),
               category_name(e->category),
               e->probe_value != NULL ? e->probe_value : "",
               proof_profile_name(e->proof_profile));
    }
    fflush(stdout);
}
