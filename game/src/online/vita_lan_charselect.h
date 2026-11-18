#ifndef MDKR_VITA_LAN_CHARSELECT_H
#define MDKR_VITA_LAN_CHARSELECT_H

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)

#include "types.h"

/* Updates and draws the LAN character screen. Returns true while it owns the
 * title-screen input and presentation. */
s32 mdkr_vita_lan_charselect_frame(s32 updateRate);
void mdkr_vita_lan_charselect_close(void);

#endif

#endif
