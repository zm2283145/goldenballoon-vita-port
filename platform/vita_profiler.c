#include "vita_profiler.h"

#if defined(__vita__) && defined(MDKR_VITA_PROFILER)

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vitaprofiler.h>
#include <vitaprofiler_stream.h>
#include <vitaprofiler_tcp_vita.h>

extern void mdkr_vita_boot_log(const char *msg);
extern void mdkr_vita_boot_log_flush(void);

#ifndef MDKR_VITA_PROFILER_HOST_A
#define MDKR_VITA_PROFILER_HOST_A 127
#endif
#ifndef MDKR_VITA_PROFILER_HOST_B
#define MDKR_VITA_PROFILER_HOST_B 0
#endif
#ifndef MDKR_VITA_PROFILER_HOST_C
#define MDKR_VITA_PROFILER_HOST_C 0
#endif
#ifndef MDKR_VITA_PROFILER_HOST_D
#define MDKR_VITA_PROFILER_HOST_D 1
#endif
#ifndef MDKR_VITA_PROFILER_PORT
#define MDKR_VITA_PROFILER_PORT 18195
#endif
#ifndef MDKR_VITA_PROFILER_CAPTURE_FRAMES
#define MDKR_VITA_PROFILER_CAPTURE_FRAMES 300
#endif

#if MDKR_VITA_PROFILER_HOST_A < 0 || MDKR_VITA_PROFILER_HOST_A > 255 || \
    MDKR_VITA_PROFILER_HOST_B < 0 || MDKR_VITA_PROFILER_HOST_B > 255 || \
    MDKR_VITA_PROFILER_HOST_C < 0 || MDKR_VITA_PROFILER_HOST_C > 255 || \
    MDKR_VITA_PROFILER_HOST_D < 0 || MDKR_VITA_PROFILER_HOST_D > 255
#error "VitaProfiler host octets must each be in [0, 255]"
#endif
#if MDKR_VITA_PROFILER_PORT < 1 || MDKR_VITA_PROFILER_PORT > 65535
#error "VitaProfiler port must be in [1, 65535]"
#endif
#if MDKR_VITA_PROFILER_CAPTURE_FRAMES < 0
#error "VitaProfiler capture frame limit must be zero or positive"
#endif

#define MDKR_VP_RING_CAPACITY 4096u
#define MDKR_VP_NAME_CAPACITY \
    (MDKR_VP_ZONE_COUNT + MDKR_VP_METRIC_COUNT + MDKR_VP_COUNTER_COUNT + 2u)
#define MDKR_VP_NAME_TEXT_BYTES 1536u
#define MDKR_VP_DICTIONARY_WIRE_BYTES 4096u
#define MDKR_VP_NET_MEMORY_BYTES (1024u * 1024u)
#define MDKR_VP_DRAIN_BATCH 128u
#define MDKR_VP_CLOSE_RETRIES 4u
#define MDKR_VP_NETWORK_WAIT_STEPS 200u
#define MDKR_VP_NETWORK_WAIT_US 100000u
#define MDKR_VP_IDLE_DELAY_US 2000u
#define MDKR_VP_THREAD_PRIORITY 0x10000100
#define MDKR_VP_THREAD_STACK_BYTES (64u * 1024u)
#define MDKR_VP_ACTIVE_SCOPE_CAPACITY 64u
#define MDKR_VP_STATUS_PATH "ux0:data/goldenballoon/profiler_status.txt"

enum mdkr_vita_profiler_result {
    MDKR_VP_RESULT_IDLE = 0,
    MDKR_VP_RESULT_RUNNING = 1,
    MDKR_VP_RESULT_COMPLETE = 2,
    MDKR_VP_RESULT_STOPPED = 3,
    MDKR_VP_RESULT_FAILED = -1,
};

static uint8_t s_net_memory[MDKR_VP_NET_MEMORY_BYTES]
    __attribute__((aligned(64)));
static struct vp_slot s_slots[MDKR_VP_RING_CAPACITY];
static struct vp_context s_context;
static struct vp_name_entry s_name_entries[MDKR_VP_NAME_CAPACITY];
static char s_name_text[MDKR_VP_NAME_TEXT_BYTES];
static struct vp_name_dictionary s_names;
static uint32_t s_zone_ids[MDKR_VP_ZONE_COUNT];
static uint32_t s_metric_ids[MDKR_VP_METRIC_COUNT];
static uint32_t s_counter_ids[MDKR_VP_COUNTER_COUNT];
static uint32_t s_frame_id;
static uint32_t s_draw_id;
static uint8_t s_dictionary_wire[MDKR_VP_DICTIONARY_WIRE_BYTES];
static struct vp_stream_writer s_writer;
static struct vp_vita_tcp_sce_net_backend s_backend =
    VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
static struct vp_vita_tcp_sink s_sink;

static SceUID s_consumer_thread = -1;
static volatile uint32_t s_stop_requested;
static volatile uint32_t s_producers_enabled;
static volatile uint32_t s_capture_complete;
static volatile uint32_t s_completed_frames;
static volatile uint32_t s_draw_calls;
static volatile uint64_t s_metric_us[MDKR_VP_METRIC_COUNT];
static volatile uint64_t s_counter_values[MDKR_VP_COUNTER_COUNT];
static volatile int32_t s_result = MDKR_VP_RESULT_IDLE;
static volatile int32_t s_first_error;

/* The game, renderer, and swap hooks are cooperative and run on the same
 * thread. These fields therefore need no lock; shutdown also runs there. */
static uint32_t s_frame_active;
static uint32_t s_context_initialized;
static uint32_t s_names_initialized;
static struct vp_zone_scope *s_active_scopes[MDKR_VP_ACTIVE_SCOPE_CAPACITY];
static uint32_t s_active_scope_count;

static const char *const s_zone_names[MDKR_VP_ZONE_COUNT] = {
    "game.frame.cpu",
    "vitagl.render_walk.cpu",
    "vitagl.replay_walk.cpu",
    "vitagl.texture_upload.cpu",
    "mods.file_read.cpu",
    "mods.png_decode.cpu",
    "mods.cache_evict.cpu",
    "mods.override_upload.cpu",
    "vitagl.swap_buffers.cpu",
    "presentation.pace.cpu",
};

static const char *const s_metric_names[MDKR_VP_METRIC_COUNT] = {
    "render.draw_state.inclusive_us",
    "render.shader.inclusive_us",
    "render.texture_bind.inclusive_us",
    "render.vertex_transform.inclusive_us",
    "render.triangle_emit.inclusive_us",
    "render.draw_submit.inclusive_us",
    "mods.lookup.inclusive_us",
    "mods.file_read.inclusive_us",
    "mods.png_decode.inclusive_us",
    "mods.cache_evict.inclusive_us",
    "mods.override_upload.inclusive_us",
};

static const char *const s_counter_names[MDKR_VP_COUNTER_COUNT] = {
    "mods.resident_hits",
    "mods.resolves",
    "mods.evictions",
    "mods.decoded_bytes",
    "mods.uploads",
    "mods.uploaded_bytes",
};

typedef char mdkr_vp_scope_size_check[
    sizeof(struct vp_zone_scope) <= sizeof(MdkrVitaProfileScope) ? 1 : -1];

static uint32_t mdkr_vp_load_u32(volatile uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void mdkr_vp_store_u32(volatile uint32_t *value, uint32_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static int32_t mdkr_vp_load_i32(volatile int32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void mdkr_vp_store_i32(volatile int32_t *value, int32_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static void mdkr_vp_remember_error(int result) {
    int32_t expected = 0;
    if (result == VP_RESULT_OK) {
        return;
    }
    (void)__atomic_compare_exchange_n(
        &s_first_error, &expected, (int32_t)result, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void mdkr_vp_track_scope(struct vp_zone_scope *scope) {
    if (scope == NULL || scope->active == 0u) {
        return;
    }
    if (s_active_scope_count >= MDKR_VP_ACTIVE_SCOPE_CAPACITY) {
        mdkr_vp_remember_error(VP_ERROR_CAPACITY);
        return;
    }
    s_active_scopes[s_active_scope_count++] = scope;
}

static void mdkr_vp_forget_scope(struct vp_zone_scope *scope) {
    uint32_t index;
    if (scope == NULL) {
        return;
    }
    for (index = s_active_scope_count; index > 0u; --index) {
        if (s_active_scopes[index - 1u] == scope) {
            const uint32_t tail = s_active_scope_count - index;
            if (tail != 0u) {
                memmove(&s_active_scopes[index - 1u],
                        &s_active_scopes[index],
                        tail * sizeof(s_active_scopes[0]));
            }
            --s_active_scope_count;
            s_active_scopes[s_active_scope_count] = NULL;
            return;
        }
    }
}

static int mdkr_vp_end_scope(struct vp_zone_scope *scope) {
    int result;
    if (scope == NULL || scope->active == 0u) {
        return VP_ERROR_INVALID_ARGUMENT;
    }
    result = vp_zone_end(&s_context, scope);
    mdkr_vp_forget_scope(scope);
    return result;
}

static void mdkr_vp_unwind_active_scopes(void) {
    while (s_active_scope_count != 0u) {
        struct vp_zone_scope *scope =
            s_active_scopes[--s_active_scope_count];
        int result;
        s_active_scopes[s_active_scope_count] = NULL;
        if (scope == NULL || scope->active == 0u) {
            continue;
        }
        result = vp_zone_end(&s_context, scope);
        if (result != VP_RESULT_OK) {
            mdkr_vp_remember_error(result);
        }
    }
}

static void mdkr_vp_write_status(const char *stage) {
    struct vp_stats ring;
    struct vp_stream_writer_stats writer;
    struct vp_vita_tcp_sink_stats sink;
    char text[1024];
    int length;
    SceUID fd;

    memset(&ring, 0, sizeof(ring));
    memset(&writer, 0, sizeof(writer));
    memset(&sink, 0, sizeof(sink));
    if (s_context_initialized != 0u) {
        (void)vp_get_stats(&s_context, &ring);
    }
    (void)vp_stream_writer_get_stats(&s_writer, &writer);
    (void)vp_vita_tcp_sink_get_stats(&s_sink, &sink);

    length = snprintf(
        text, sizeof(text),
        "goldenballoon-vita-profiler=1\n"
        "stage=%s\nresult=%d\nfirst_error=0x%08X\n"
        "endpoint=%u.%u.%u.%u:%u\nframes=%u\n"
        "ring_accepted=%u\nring_dropped=%u\nring_pending=%u\n"
        "writer_events=%llu\nwriter_lost=%u\nwriter_state=%u\n"
        "tcp_bytes=%llu\ntcp_failures=%u\ntcp_state=%u\n"
        "pmu_enabled=0\n",
        stage != NULL ? stage : "unknown",
        (int)mdkr_vp_load_i32(&s_result),
        (unsigned int)mdkr_vp_load_i32(&s_first_error),
        (unsigned int)MDKR_VITA_PROFILER_HOST_A,
        (unsigned int)MDKR_VITA_PROFILER_HOST_B,
        (unsigned int)MDKR_VITA_PROFILER_HOST_C,
        (unsigned int)MDKR_VITA_PROFILER_HOST_D,
        (unsigned int)MDKR_VITA_PROFILER_PORT,
        (unsigned int)mdkr_vp_load_u32(&s_completed_frames),
        ring.accepted, ring.dropped, ring.pending,
        (unsigned long long)writer.events_written,
        writer.events_lost_to_sink, writer.state,
        (unsigned long long)sink.bytes_sent, sink.failures, sink.state);
    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(text)) {
        length = (int)sizeof(text) - 1;
    }
    fd = sceIoOpen(MDKR_VP_STATUS_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        (void)sceIoWrite(fd, text, (SceSize)length);
        (void)sceIoClose(fd);
    }
}

static int mdkr_vp_wait_for_network(void) {
    uint32_t attempt;
    for (attempt = 0u; attempt < MDKR_VP_NETWORK_WAIT_STEPS; ++attempt) {
        int state = SCE_NETCTL_STATE_DISCONNECTED;
        int result;
        if (mdkr_vp_load_u32(&s_stop_requested) != 0u) {
            return VP_RESULT_OK;
        }
        result = sceNetCtlInetGetState(&state);
        if (result < 0) {
            return result;
        }
        if (state == SCE_NETCTL_STATE_CONNECTED) {
            return VP_RESULT_OK;
        }
        sceKernelDelayThread(MDKR_VP_NETWORK_WAIT_US);
    }
    return VP_ERROR_IO;
}

static int mdkr_vp_close_sink(void) {
    uint32_t attempt;
    int result = VP_RESULT_OK;
    for (attempt = 0u; attempt < MDKR_VP_CLOSE_RETRIES; ++attempt) {
        result = vp_vita_tcp_sink_close(&s_sink);
        if (result == VP_RESULT_OK) {
            return result;
        }
        mdkr_vp_remember_error(result);
        sceKernelDelayThread(100000u);
    }
    return result;
}

static int mdkr_vp_drain_and_close_writer(void) {
    struct vp_stream_writer_stats stats;
    int result;

    result = vp_stream_writer_get_stats(&s_writer, &stats);
    if (result != VP_RESULT_OK) {
        return result;
    }
    if (stats.state == VP_STREAM_WRITER_CLOSED) {
        return VP_RESULT_OK;
    }
    if (stats.state != VP_STREAM_WRITER_STREAMING) {
        return VP_ERROR_STATE;
    }
    for (;;) {
        size_t drained = 0u;
        result = vp_stream_writer_drain(&s_writer, MDKR_VP_DRAIN_BATCH,
                                        &drained);
        if (result != VP_RESULT_OK) {
            return result;
        }
        if (drained == 0u) {
            break;
        }
    }
    return vp_stream_writer_close(&s_writer);
}

static int mdkr_vp_consumer(SceSize args, void *argp) {
    struct vp_vita_tcp_sink_config sink_config;
    struct vp_stream_writer_config writer_config;
    SceNetInitParam net_init;
    SceInt64 stream_start;
    int net_module_owned = 0;
    int net_initialized = 0;
    int netctl_initialized = 0;
    int sink_initialized = 0;
    int sink_released = 1;
    int writer_streaming = 0;
    int result = VP_RESULT_OK;

    (void)args;
    (void)argp;
    mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_RUNNING);
    mdkr_vp_write_status("starting");

    /* Profiler builds have one network owner. A debugger/logger that already
     * owns SceNet is rejected instead of creating two competing lifetimes. */
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) == 0) {
        result = VP_ERROR_BUSY;
        goto cleanup;
    }
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0) {
        goto cleanup;
    }
    net_module_owned = 1;

    memset(&net_init, 0, sizeof(net_init));
    net_init.memory = s_net_memory;
    net_init.size = (int)sizeof(s_net_memory);
    result = sceNetInit(&net_init);
    if (result < 0) {
        goto cleanup;
    }
    net_initialized = 1;

    result = sceNetCtlInit();
    if (result < 0) {
        goto cleanup;
    }
    netctl_initialized = 1;
    mdkr_vp_write_status("waiting-network");
    result = mdkr_vp_wait_for_network();
    if (result != VP_RESULT_OK ||
        mdkr_vp_load_u32(&s_stop_requested) != 0u) {
        goto cleanup;
    }

    vp_vita_tcp_sink_config_init(&sink_config);
    sink_config.endpoint.ipv4[0] = (uint8_t)MDKR_VITA_PROFILER_HOST_A;
    sink_config.endpoint.ipv4[1] = (uint8_t)MDKR_VITA_PROFILER_HOST_B;
    sink_config.endpoint.ipv4[2] = (uint8_t)MDKR_VITA_PROFILER_HOST_C;
    sink_config.endpoint.ipv4[3] = (uint8_t)MDKR_VITA_PROFILER_HOST_D;
    sink_config.endpoint.port = (uint16_t)MDKR_VITA_PROFILER_PORT;
    result = vp_vita_tcp_sce_net_ops_init(&s_backend, &sink_config.ops);
    if (result != VP_RESULT_OK) {
        goto cleanup;
    }
    sink_config.ops_user = &s_backend;
    result = vp_vita_tcp_sink_init(&s_sink, &sink_config);
    if (result != VP_RESULT_OK) {
        goto cleanup;
    }
    sink_initialized = 1;
    sink_released = 0;
    mdkr_vp_write_status("connecting");
    result = vp_vita_tcp_sink_connect(&s_sink);
    if (result != VP_RESULT_OK) {
        goto cleanup;
    }

    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &s_context;
    writer_config.names = &s_names;
    writer_config.write = vp_vita_tcp_sink_write;
    writer_config.write_user = &s_sink;
    writer_config.dictionary_buffer = s_dictionary_wire;
    writer_config.dictionary_buffer_capacity = sizeof(s_dictionary_wire);
    result = vp_stream_writer_init(&s_writer, &writer_config);
    if (result != VP_RESULT_OK) {
        goto cleanup;
    }
    stream_start = sceKernelGetSystemTimeWide();
    if (stream_start < 0) {
        result = VP_ERROR_PLATFORM;
        goto cleanup;
    }
    result = vp_stream_writer_begin(&s_writer, (uint64_t)stream_start);
    if (result != VP_RESULT_OK) {
        goto cleanup;
    }
    writer_streaming = 1;

    /* Only the consumer performs network I/O. Game/render hooks remain
     * nonblocking producers and are published after the stream is ready. */
    mdkr_vp_store_u32(&s_producers_enabled, 1u);
    mdkr_vp_write_status("capturing");
    while (mdkr_vp_load_u32(&s_stop_requested) == 0u &&
           mdkr_vp_load_u32(&s_capture_complete) == 0u) {
        size_t drained = 0u;
        result = vp_stream_writer_drain(&s_writer, MDKR_VP_DRAIN_BATCH,
                                        &drained);
        if (result != VP_RESULT_OK) {
            mdkr_vp_store_u32(&s_producers_enabled, 0u);
            break;
        }
        if (drained == 0u) {
            sceKernelDelayThread(MDKR_VP_IDLE_DELAY_US);
        }
    }

    mdkr_vp_store_u32(&s_producers_enabled, 0u);
    if (result == VP_RESULT_OK) {
        result = mdkr_vp_drain_and_close_writer();
        if (result == VP_RESULT_OK) {
            writer_streaming = 0;
        }
    }

cleanup:
    mdkr_vp_store_u32(&s_producers_enabled, 0u);
    if (result != VP_RESULT_OK && writer_streaming) {
        (void)mdkr_vp_drain_and_close_writer();
    }
    if (sink_initialized) {
        const int close_result = mdkr_vp_close_sink();
        if (close_result == VP_RESULT_OK) {
            sink_released = 1;
        }
        if (result == VP_RESULT_OK && close_result != VP_RESULT_OK) {
            result = close_result;
        }
    }

    /* A retained socket is more important than eager teardown. Never call
     * sceNetTerm underneath an adapter that still owns a descriptor. */
    if (sink_released) {
        if (netctl_initialized) {
            sceNetCtlTerm();
            netctl_initialized = 0;
        }
        if (net_initialized) {
            const int net_result = sceNetTerm();
            if (net_result < 0) {
                mdkr_vp_remember_error(net_result);
                if (result == VP_RESULT_OK) {
                    result = net_result;
                }
            } else {
                net_initialized = 0;
            }
        }
        if (net_module_owned && !net_initialized) {
            const int module_result =
                sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
            if (module_result < 0) {
                mdkr_vp_remember_error(module_result);
                if (result == VP_RESULT_OK) {
                    result = module_result;
                }
            }
        }
    }

    if (result != VP_RESULT_OK) {
        mdkr_vp_remember_error(result);
        mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_FAILED);
        mdkr_vp_write_status("failed");
    } else if (mdkr_vp_load_u32(&s_capture_complete) != 0u) {
        mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_COMPLETE);
        mdkr_vp_write_status("complete");
    } else {
        mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_STOPPED);
        mdkr_vp_write_status("stopped");
    }
    return 0;
}

void mdkr_vita_profiler_init(void) {
    struct vp_name_dictionary_config name_config;
    char log_line[160];
    uint32_t index;
    int result;

    if (s_consumer_thread >= 0 || s_context_initialized != 0u ||
        s_names_initialized != 0u) {
        return;
    }

    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = s_name_entries;
    name_config.entry_capacity = MDKR_VP_NAME_CAPACITY;
    name_config.text = s_name_text;
    name_config.text_capacity = sizeof(s_name_text);
    result = vp_name_dictionary_init(&s_names, &name_config);
    if (result != VP_RESULT_OK) {
        goto fail;
    }
    s_names_initialized = 1u;
    for (index = 0u; index < (uint32_t)MDKR_VP_ZONE_COUNT; ++index) {
        result = vp_name_dictionary_register(
            &s_names, s_zone_names[index], &s_zone_ids[index]);
        if (result != VP_RESULT_OK) {
            goto fail;
        }
    }
    for (index = 0u; index < (uint32_t)MDKR_VP_METRIC_COUNT; ++index) {
        result = vp_name_dictionary_register(
            &s_names, s_metric_names[index], &s_metric_ids[index]);
        if (result != VP_RESULT_OK) {
            goto fail;
        }
    }
    for (index = 0u; index < (uint32_t)MDKR_VP_COUNTER_COUNT; ++index) {
        result = vp_name_dictionary_register(
            &s_names, s_counter_names[index], &s_counter_ids[index]);
        if (result != VP_RESULT_OK) {
            goto fail;
        }
    }
    result = vp_name_dictionary_register(&s_names, "game.frame", &s_frame_id);
    if (result == VP_RESULT_OK) {
        result = vp_name_dictionary_register(
            &s_names, "vitagl.draw_calls", &s_draw_id);
    }
    if (result == VP_RESULT_OK) {
        result = vp_name_dictionary_seal(&s_names);
    }
    if (result != VP_RESULT_OK) {
        goto fail;
    }

    result = vp_vita_init(&s_context, s_slots, MDKR_VP_RING_CAPACITY);
    if (result != VP_RESULT_OK) {
        goto fail;
    }
    s_context_initialized = 1u;

    mdkr_vp_store_u32(&s_stop_requested, 0u);
    mdkr_vp_store_u32(&s_producers_enabled, 0u);
    mdkr_vp_store_u32(&s_capture_complete, 0u);
    mdkr_vp_store_u32(&s_completed_frames, 0u);
    mdkr_vp_store_u32(&s_draw_calls, 0u);
    memset((void *)s_metric_us, 0, sizeof(s_metric_us));
    memset((void *)s_counter_values, 0, sizeof(s_counter_values));
    mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_IDLE);
    mdkr_vp_store_i32(&s_first_error, 0);
    s_frame_active = 0u;
    s_active_scope_count = 0u;
    memset(s_active_scopes, 0, sizeof(s_active_scopes));
    memset(&s_backend, 0, sizeof(s_backend));
    memset(&s_sink, 0, sizeof(s_sink));
    memset(&s_writer, 0, sizeof(s_writer));

    s_consumer_thread = sceKernelCreateThread(
        "GoldenBalloonProfiler", mdkr_vp_consumer, MDKR_VP_THREAD_PRIORITY,
        MDKR_VP_THREAD_STACK_BYTES, 0, 0, NULL);
    if (s_consumer_thread < 0) {
        result = s_consumer_thread;
        s_consumer_thread = -1;
        goto fail;
    }
    result = sceKernelStartThread(s_consumer_thread, 0, NULL);
    if (result < 0) {
        (void)sceKernelDeleteThread(s_consumer_thread);
        s_consumer_thread = -1;
        goto fail;
    }

    snprintf(log_line, sizeof(log_line),
             "profiler: TCP consumer started for %u.%u.%u.%u:%u",
             (unsigned int)MDKR_VITA_PROFILER_HOST_A,
             (unsigned int)MDKR_VITA_PROFILER_HOST_B,
             (unsigned int)MDKR_VITA_PROFILER_HOST_C,
             (unsigned int)MDKR_VITA_PROFILER_HOST_D,
             (unsigned int)MDKR_VITA_PROFILER_PORT);
    mdkr_vita_boot_log(log_line);
    mdkr_vita_boot_log_flush();
    return;

fail:
    mdkr_vp_remember_error(result);
    mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_FAILED);
    mdkr_vp_write_status("start-failed");
    if (s_context_initialized != 0u) {
        vp_deinit(&s_context);
        s_context_initialized = 0u;
    }
    if (s_names_initialized != 0u) {
        vp_name_dictionary_deinit(&s_names);
        s_names_initialized = 0u;
    }
}

void mdkr_vita_profiler_shutdown(void) {
    int wait_result;

    if (s_context_initialized != 0u) {
        mdkr_vp_unwind_active_scopes();
    }
    (void)__atomic_exchange_n(&s_draw_calls, 0u, __ATOMIC_ACQ_REL);
    s_frame_active = 0u;
    mdkr_vp_store_u32(&s_producers_enabled, 0u);
    mdkr_vp_store_u32(&s_stop_requested, 1u);
    if (s_consumer_thread >= 0) {
        wait_result = sceKernelWaitThreadEnd(s_consumer_thread, NULL, NULL);
        if (wait_result < 0) {
            /* Retain shared state if the worker cannot be proven stopped. The
             * process will reclaim it without a teardown race. */
            mdkr_vp_remember_error(wait_result);
            mdkr_vp_store_i32(&s_result, MDKR_VP_RESULT_FAILED);
            mdkr_vp_write_status("join-failed");
            return;
        }
        (void)sceKernelDeleteThread(s_consumer_thread);
        s_consumer_thread = -1;
    }
    if (s_context_initialized != 0u) {
        vp_deinit(&s_context);
        s_context_initialized = 0u;
    }
    if (s_names_initialized != 0u) {
        vp_name_dictionary_deinit(&s_names);
        s_names_initialized = 0u;
    }
}

void mdkr_vita_profiler_frame_begin(void) {
    uint32_t index;
    s_frame_active = 0u;
    (void)__atomic_exchange_n(&s_draw_calls, 0u, __ATOMIC_ACQ_REL);
    for (index = 0u; index < (uint32_t)MDKR_VP_METRIC_COUNT; ++index) {
        (void)__atomic_exchange_n(&s_metric_us[index], 0u, __ATOMIC_ACQ_REL);
    }
    for (index = 0u; index < (uint32_t)MDKR_VP_COUNTER_COUNT; ++index) {
        (void)__atomic_exchange_n(&s_counter_values[index], 0u,
                                  __ATOMIC_ACQ_REL);
    }
    if (mdkr_vp_load_u32(&s_producers_enabled) != 0u) {
        s_frame_active = 1u;
    }
}

void mdkr_vita_profiler_frame_end(void) {
    uint32_t frames;
    uint32_t draws;
    uint32_t index;
    int result;

    if (s_frame_active == 0u) {
        return;
    }
    draws = __atomic_exchange_n(&s_draw_calls, 0u, __ATOMIC_ACQ_REL);
    result = vp_counter(&s_context, s_draw_id, (int64_t)draws);
    if (result < VP_RESULT_OK) {
        mdkr_vp_remember_error(result);
    }
    for (index = 0u; index < (uint32_t)MDKR_VP_METRIC_COUNT; ++index) {
        const uint64_t elapsed =
            __atomic_exchange_n(&s_metric_us[index], 0u, __ATOMIC_ACQ_REL);
        result = vp_counter(&s_context, s_metric_ids[index], (int64_t)elapsed);
        if (result < VP_RESULT_OK) {
            mdkr_vp_remember_error(result);
        }
    }
    for (index = 0u; index < (uint32_t)MDKR_VP_COUNTER_COUNT; ++index) {
        const uint64_t value = __atomic_exchange_n(
            &s_counter_values[index], 0u, __ATOMIC_ACQ_REL);
        result = vp_counter(&s_context, s_counter_ids[index], (int64_t)value);
        if (result < VP_RESULT_OK) {
            mdkr_vp_remember_error(result);
        }
    }
    result = vp_frame_mark(&s_context, s_frame_id);
    if (result < VP_RESULT_OK) {
        mdkr_vp_remember_error(result);
    }
    s_frame_active = 0u;
    frames = __atomic_add_fetch(&s_completed_frames, 1u, __ATOMIC_ACQ_REL);
#if MDKR_VITA_PROFILER_CAPTURE_FRAMES > 0
    if (frames >= (uint32_t)MDKR_VITA_PROFILER_CAPTURE_FRAMES) {
        mdkr_vp_store_u32(&s_producers_enabled, 0u);
        mdkr_vp_store_u32(&s_capture_complete, 1u);
    }
#else
    (void)frames;
#endif
}

void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone zone,
                                   MdkrVitaProfileScope *scope) {
    struct vp_zone_scope *value;
    if (scope == NULL) {
        return;
    }
    memset(scope, 0, sizeof(*scope));
    if (s_frame_active == 0u ||
        mdkr_vp_load_u32(&s_producers_enabled) == 0u ||
        zone < 0 || zone >= MDKR_VP_ZONE_COUNT) {
        return;
    }
    value = (struct vp_zone_scope *)scope;
    if (vp_zone_begin(&s_context, s_zone_ids[(uint32_t)zone], value) ==
        VP_RESULT_OK) {
        mdkr_vp_track_scope(value);
    }
}

void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *scope) {
    struct vp_zone_scope *value;
    if (scope == NULL) {
        return;
    }
    value = (struct vp_zone_scope *)scope;
    if (value->active != 0u) {
        const int result = mdkr_vp_end_scope(value);
        if (result != VP_RESULT_OK) {
            mdkr_vp_remember_error(result);
        }
    }
}

void mdkr_vita_profiler_count_draw(void) {
    if (s_frame_active != 0u &&
        mdkr_vp_load_u32(&s_producers_enabled) != 0u) {
        (void)__atomic_add_fetch(&s_draw_calls, 1u, __ATOMIC_RELAXED);
    }
}

uint64_t mdkr_vita_profiler_metric_begin(void) {
    if (s_frame_active == 0u ||
        mdkr_vp_load_u32(&s_producers_enabled) == 0u) {
        return 0u;
    }
    return sceKernelGetProcessTimeWide();
}

void mdkr_vita_profiler_metric_end(MdkrVitaProfileMetric metric,
                                   uint64_t started_us) {
    uint64_t ended_us;
    if (started_us == 0u || metric < 0 || metric >= MDKR_VP_METRIC_COUNT ||
        s_frame_active == 0u ||
        mdkr_vp_load_u32(&s_producers_enabled) == 0u) {
        return;
    }
    ended_us = sceKernelGetProcessTimeWide();
    if (ended_us >= started_us) {
        (void)__atomic_add_fetch(
            &s_metric_us[(uint32_t)metric], ended_us - started_us,
            __ATOMIC_RELAXED);
    }
}

void mdkr_vita_profiler_counter_add(MdkrVitaProfileCounter counter,
                                    uint64_t value) {
    if (value == 0u || counter < 0 || counter >= MDKR_VP_COUNTER_COUNT ||
        s_frame_active == 0u ||
        mdkr_vp_load_u32(&s_producers_enabled) == 0u) {
        return;
    }
    (void)__atomic_add_fetch(&s_counter_values[(uint32_t)counter], value,
                             __ATOMIC_RELAXED);
}

#endif
