#include "video_mode_table.h"

void mdkr_video_apply_pal_height_raise(VideoModeResolution *modes,
                                       int lastIndex, int palDelta,
                                       int *raised) {
    int i;

    if (modes == NULL) {
        return;
    }
    /* Latching once-flag: skip if this process already raised the table. */
    if (raised != NULL && *raised != 0) {
        return;
    }
    for (i = 0; i <= lastIndex; i++) {
        modes[i].height += palDelta;
    }
    if (raised != NULL) {
        *raised = 1;
    }
}
