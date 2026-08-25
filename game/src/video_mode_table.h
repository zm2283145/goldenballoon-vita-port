#ifndef MDKR_VIDEO_MODE_TABLE_H
#define MDKR_VIDEO_MODE_TABLE_H

#include "video.h" /* VideoModeResolution */

/**
 * Raise every video mode's height by the PAL taller-field delta, at most once
 * per process.
 *
 * On console video_init() runs a single time per power cycle, so the retail
 * `gVideoModeResolutions[i].height += PAL_HEIGHT_DIFFERENCE` loop applies once
 * and the raised table simply persists.  The native persistent launcher breaks
 * that assumption: "Return to Launcher" returns home in-process and a second
 * "Play" runs another engine epoch WITHOUT reloading game .data/.bss, so
 * video_init() runs again against the already-raised global table.  Applying
 * the raise unconditionally therefore compounds the height on a PAL ROM
 * (240 -> 264 -> 288 -> ...), which inflates fb_size()/the logical surface and
 * the menu ortho viewport and lifts every PAL SAFE_2D layout (e.g. the
 * GAME SELECT "ADVENTURE" labels) up the screen on the second and later epochs.
 *
 * `*raised` is a caller-owned once-flag (a file-scope static in video.c): the
 * raise runs only while it is zero, then latches it.  NTSC never calls this
 * (its height is unchanged) and the first PAL epoch is byte-identical to the
 * retail loop; only the compounding on later epochs is prevented.
 *
 * @param modes      the mode table (gVideoModeResolutions)
 * @param lastIndex  the last valid index (NUM_RESOLUTION_MODES); indices
 *                   0..lastIndex inclusive are raised, matching the retail loop
 * @param palDelta   PAL_HEIGHT_DIFFERENCE
 * @param raised     caller-owned once-flag; NULL disables the guard
 */
void mdkr_video_apply_pal_height_raise(VideoModeResolution *modes,
                                       int lastIndex, int palDelta,
                                       int *raised);

#endif /* MDKR_VIDEO_MODE_TABLE_H */
