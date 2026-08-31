#include "online/online_character_map.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Single-source table: same order, same values as online_portraits.h's
 * sOnlineToPortrait[] (both expand MDKR_ONLINE_CHARACTER_ENGINE_ORDER). */
static const short kOnlineToEngine[MDKR_ONLINE_CHARACTER_MAP_COUNT] = {
    MDKR_ONLINE_CHARACTER_ENGINE_ORDER,
};

int mdkr_online_character_to_engine(int online_id) {
    if (online_id < 0 || online_id >= MDKR_ONLINE_CHARACTER_MAP_COUNT) {
        return online_id;
    }
    return (int)kOnlineToEngine[online_id];
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
