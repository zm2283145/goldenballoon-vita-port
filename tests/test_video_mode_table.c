/*
 * Unit test for mdkr_video_apply_pal_height_raise (issue: PAL menu vertical
 * shift after Return-to-Launcher -> Play).
 *
 * The native persistent launcher calls video_init() once per in-process engine
 * epoch, not once per console power cycle. The retail PAL height raise mutates
 * the persistent global gVideoModeResolutions[] in place, so applying it every
 * epoch compounds the height (240 -> 264 -> 288 -> ...) and lifts PAL SAFE_2D
 * menus (GAME SELECT "ADVENTURE" labels) up the screen. The fix makes the raise
 * idempotent via a caller-owned once-flag; these tests pin that behaviour and
 * fail (compounded heights) against the pre-fix unguarded loop.
 */

#include "video_mode_table.h"

#include <stdio.h>

static int s_failures;

static void expect_eq(const char *name, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-40s actual=%d expected=%d\n",
                name, actual, expected);
        s_failures++;
    }
}

/* A representative PAL-ish table: distinct authored heights so an index/stride
 * mistake or a partial loop is caught, matching gVideoModeResolutions' shape
 * (several 240 modes plus a 480 hi-res mode). */
static void seed(VideoModeResolution *t) {
    t[0].width = 320; t[0].height = 240;
    t[1].width = 320; t[1].height = 240;
    t[2].width = 640; t[2].height = 240;
    t[3].width = 640; t[3].height = 480;
    t[4].width = 640; t[4].height = 480; /* sentinel: must NOT be raised */
}

#define DELTA 24
#define LAST_INDEX 3 /* raise indices 0..3 inclusive; index 4 is the sentinel */

int main(void) {
    VideoModeResolution table[5];

    /* 1. The bug: three epochs must raise the height EXACTLY once, not thrice.
     *    Against the unguarded retail loop this yields 240+72 and fails. */
    {
        int raised = 0;
        int epoch;
        seed(table);
        for (epoch = 0; epoch < 3; epoch++) {
            mdkr_video_apply_pal_height_raise(table, LAST_INDEX, DELTA, &raised);
        }
        expect_eq("idempotent/mode0", table[0].height, 240 + DELTA);
        expect_eq("idempotent/mode2", table[2].height, 240 + DELTA);
        expect_eq("idempotent/mode3_hires", table[3].height, 480 + DELTA);
        expect_eq("idempotent/flag_latched", raised, 1);
        /* sentinel past lastIndex is never touched */
        expect_eq("idempotent/sentinel_untouched", table[4].height, 480);
        expect_eq("idempotent/width_unchanged", table[0].width, 320);
    }

    /* 2. Non-vacuous: a single first-epoch raise really does apply the delta to
     *    every mode 0..lastIndex inclusive (matches the retail `<=` loop). */
    {
        int raised = 0;
        seed(table);
        mdkr_video_apply_pal_height_raise(table, LAST_INDEX, DELTA, &raised);
        expect_eq("first_raise/mode0", table[0].height, 240 + DELTA);
        expect_eq("first_raise/mode1", table[1].height, 240 + DELTA);
        expect_eq("first_raise/mode3_last", table[3].height, 480 + DELTA);
        expect_eq("first_raise/sentinel", table[4].height, 480);
    }

    /* 3. Latch respected: an already-raised flag makes the call a no-op. */
    {
        int raised = 1;
        seed(table);
        mdkr_video_apply_pal_height_raise(table, LAST_INDEX, DELTA, &raised);
        expect_eq("already_raised/no_change", table[0].height, 240);
    }

    /* 4. NULL guard always applies (documents that the caller owns the flag,
     *    as video.c does with its file-scope static). */
    {
        seed(table);
        mdkr_video_apply_pal_height_raise(table, LAST_INDEX, DELTA, NULL);
        mdkr_video_apply_pal_height_raise(table, LAST_INDEX, DELTA, NULL);
        expect_eq("null_guard/compounds", table[0].height, 240 + 2 * DELTA);
    }

    /* 5. NULL table is a safe no-op (defensive). */
    mdkr_video_apply_pal_height_raise(NULL, LAST_INDEX, DELTA, NULL);

    if (s_failures != 0) {
        fprintf(stderr, "test_video_mode_table: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_video_mode_table: PASS\n");
    return 0;
}
