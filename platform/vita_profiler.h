#ifndef MDKR_VITA_PROFILER_H
#define MDKR_VITA_PROFILER_H

#include <stdint.h>

typedef enum MdkrVitaProfileZone {
    MDKR_VP_ZONE_GAME_FRAME = 0,
    MDKR_VP_ZONE_RENDER_WALK,
    MDKR_VP_ZONE_REPLAY_WALK,
    MDKR_VP_ZONE_TEXTURE_UPLOAD,
    MDKR_VP_ZONE_MOD_FILE_READ,
    MDKR_VP_ZONE_MOD_PNG_DECODE,
    MDKR_VP_ZONE_MOD_CACHE_EVICT,
    MDKR_VP_ZONE_MOD_OVERRIDE_UPLOAD,
    MDKR_VP_ZONE_BUFFER_SWAP,
    MDKR_VP_ZONE_PRESENT_PACE,
    MDKR_VP_ZONE_COUNT
} MdkrVitaProfileZone;

typedef enum MdkrVitaProfileMetric {
    MDKR_VP_METRIC_DRAW_STATE_US = 0,
    MDKR_VP_METRIC_SHADER_US,
    MDKR_VP_METRIC_TEXTURE_BIND_US,
    MDKR_VP_METRIC_VERTEX_TRANSFORM_US,
    MDKR_VP_METRIC_TRIANGLE_EMIT_US,
    MDKR_VP_METRIC_DRAW_SUBMIT_US,
    MDKR_VP_METRIC_MOD_LOOKUP_US,
    MDKR_VP_METRIC_MOD_FILE_READ_US,
    MDKR_VP_METRIC_MOD_PNG_DECODE_US,
    MDKR_VP_METRIC_MOD_CACHE_EVICT_US,
    MDKR_VP_METRIC_MOD_OVERRIDE_UPLOAD_US,
    MDKR_VP_METRIC_COUNT
} MdkrVitaProfileMetric;

typedef enum MdkrVitaProfileCounter {
    MDKR_VP_COUNTER_MOD_RESIDENT_HITS = 0,
    MDKR_VP_COUNTER_MOD_RESOLVES,
    MDKR_VP_COUNTER_MOD_EVICTIONS,
    MDKR_VP_COUNTER_MOD_DECODED_BYTES,
    MDKR_VP_COUNTER_MOD_UPLOADS,
    MDKR_VP_COUNTER_MOD_UPLOADED_BYTES,
    MDKR_VP_COUNTER_COUNT
} MdkrVitaProfileCounter;

typedef struct MdkrVitaProfileScope { uint64_t storage[4]; } MdkrVitaProfileScope;

#if defined(__vita__) && defined(MDKR_VITA_PROFILER)
void mdkr_vita_profiler_init(void);
void mdkr_vita_profiler_shutdown(void);
void mdkr_vita_profiler_frame_begin(void);
void mdkr_vita_profiler_frame_end(void);
void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone zone, MdkrVitaProfileScope *scope);
void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *scope);
void mdkr_vita_profiler_count_draw(void);
uint64_t mdkr_vita_profiler_metric_begin(void);
void mdkr_vita_profiler_metric_end(MdkrVitaProfileMetric metric,
                                   uint64_t started_us);
void mdkr_vita_profiler_counter_add(MdkrVitaProfileCounter counter,
                                    uint64_t value);
#else
static inline void mdkr_vita_profiler_init(void) {}
static inline void mdkr_vita_profiler_shutdown(void) {}
static inline void mdkr_vita_profiler_frame_begin(void) {}
static inline void mdkr_vita_profiler_frame_end(void) {}
static inline void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone z, MdkrVitaProfileScope *s) {(void)z;(void)s;}
static inline void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *s) {(void)s;}
static inline void mdkr_vita_profiler_count_draw(void) {}
static inline uint64_t mdkr_vita_profiler_metric_begin(void) { return 0u; }
static inline void mdkr_vita_profiler_metric_end(MdkrVitaProfileMetric m,
                                                 uint64_t t) {(void)m;(void)t;}
static inline void mdkr_vita_profiler_counter_add(MdkrVitaProfileCounter c,
                                                  uint64_t v) {(void)c;(void)v;}
#endif

#endif
