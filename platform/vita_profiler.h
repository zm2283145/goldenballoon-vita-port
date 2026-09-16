#ifndef MDKR_VITA_PROFILER_H
#define MDKR_VITA_PROFILER_H

#include <stdint.h>

typedef enum MdkrVitaProfileZone {
    MDKR_VP_ZONE_GAME_FRAME = 0,
    MDKR_VP_ZONE_RENDER_WALK,
    MDKR_VP_ZONE_REPLAY_WALK,
    MDKR_VP_ZONE_TEXTURE_UPLOAD,
    MDKR_VP_ZONE_BUFFER_SWAP,
    MDKR_VP_ZONE_COUNT
} MdkrVitaProfileZone;

typedef struct MdkrVitaProfileScope { uint64_t storage[4]; } MdkrVitaProfileScope;

#if defined(__vita__) && defined(MDKR_VITA_PROFILER)
void mdkr_vita_profiler_init(void);
void mdkr_vita_profiler_shutdown(void);
void mdkr_vita_profiler_frame_begin(void);
void mdkr_vita_profiler_frame_end(void);
void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone zone, MdkrVitaProfileScope *scope);
void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *scope);
void mdkr_vita_profiler_count_draw(void);
#else
static inline void mdkr_vita_profiler_init(void) {}
static inline void mdkr_vita_profiler_shutdown(void) {}
static inline void mdkr_vita_profiler_frame_begin(void) {}
static inline void mdkr_vita_profiler_frame_end(void) {}
static inline void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone z, MdkrVitaProfileScope *s) {(void)z;(void)s;}
static inline void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *s) {(void)s;}
static inline void mdkr_vita_profiler_count_draw(void) {}
#endif

#endif
