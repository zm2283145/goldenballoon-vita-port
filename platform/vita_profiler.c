#include "vita_profiler.h"

#if defined(__vita__) && defined(MDKR_VITA_PROFILER)
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#include <uvdb.h>
#include <vitaprofiler.h>
#include <vitadebug_pmu_profiler.h>

#define RING_CAPACITY 2048u
#define REPORT_FRAMES 60u
static unsigned char debugnet_memory[1024 * 1024] __attribute__((aligned(64)));
static uint32_t debugnet_started, debugnet_network_ready;
static uint32_t debugnet_retry_frames, net_owned, net_module_owned;

static struct vp_context ctx;
static struct vp_slot slots[RING_CAPACITY];
static struct vp_name_dictionary names;
static struct vp_name_entry name_entries[MDKR_VP_ZONE_COUNT + 2];
static char name_text[384];
static uint32_t zone_ids[MDKR_VP_ZONE_COUNT], frame_id, draw_id;
static uint64_t zone_total[MDKR_VP_ZONE_COUNT], draw_total;
static uint32_t zone_samples[MDKR_VP_ZONE_COUNT], frames, enabled;
static struct vd_kernel_pmu_profiler_info pmu_info;
static struct vd_kernel_pmu_profiler_handle pmu_handle;
static uint32_t pmu_active, pmu_event_index;
static const char *const zone_names[MDKR_VP_ZONE_COUNT] = {
    "game.frame.cpu", "vitagl.render_walk.cpu",
    "vitagl.replay_walk.cpu", "vitagl.texture_upload.cpu", "vitagl.swap_buffers.cpu"
};
static const uint32_t pmu_events[] = {
    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
    VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS,
    VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT
};
typedef char scope_size_check[sizeof(struct vp_zone_scope) <= sizeof(MdkrVitaProfileScope) ? 1 : -1];

static void debugnet_try_start(void) {
    struct uvdb_debugnet_config config;
    if (debugnet_started || !debugnet_network_ready) return;
    if (debugnet_retry_frames != 0u) {
        debugnet_retry_frames--;
        return;
    }
    memset(&config, 0, sizeof(config));
    config.server_ip = MDKR_VITA_DEBUGNET_HOST;
    config.port = MDKR_VITA_DEBUGNET_PORT;
    config.level = UVDB_LOG_INFO;
    if (uvdb_debugnet_start(&config) == 0) {
        debugnet_started = 1;
        (void)uvdb_debugnet_write(UVDB_LOG_INFO,
                                  "[VPROF] DebugNet connected after network startup");
    } else {
        /* The current VitaDebugger lifecycle contract requires one cleanup
         * pass after a failed start before retrying: setup may have retained a
         * socket, event UID, or worker handle whose release is retryable. */
        (void)uvdb_debugnet_stop();
        /* Network association can lag sceNetInit during application startup.
         * Retry at one-second intervals without blocking the render thread. */
        debugnet_retry_frames = REPORT_FRAMES;
    }
}

static int zone_from_id(uint32_t id) {
    int i; for (i = 0; i < MDKR_VP_ZONE_COUNT; ++i) if (zone_ids[i] == id) return i;
    return -1;
}

static void pmu_close_report(void) {
    struct vd_kernel_pmu_profiler_sample sample;
    char line[192]; int rd, close_result;
    if (!pmu_active) return;
    memset(&sample, 0, sizeof(sample));
    sample.struct_size = sizeof(sample); sample.abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    rd = vdKernelPmuProfilerRead(&pmu_handle, &sample);
    close_result = vdKernelPmuProfilerClose(&pmu_handle);
    snprintf(line, sizeof(line), "[VPROF] pmu event=0x%02x value=%" PRIu64 " read=%d close=%d core=%u lane=%u",
             (unsigned)pmu_handle.event_code, rd == 0 ? sample.value : UINT64_C(0), rd, close_result,
             (unsigned)sample.core_id, (unsigned)sample.physical_counter);
    if (debugnet_started) (void)uvdb_debugnet_write(UVDB_LOG_INFO, line);
    pmu_active = 0;
    if (close_result != 0) pmu_info.capabilities = 0; /* fail closed: never re-arm */
}

static void pmu_open(void) {
    struct vd_kernel_pmu_profiler_open_request req; char line[96]; int result;
    const uint32_t required = VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE |
        VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS | VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM;
    if (pmu_active || (pmu_info.capabilities & required) != required) return;
    memset(&req, 0, sizeof(req)); req.struct_size = sizeof(req);
    req.abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    req.event_code = pmu_events[pmu_event_index]; req.lease_ms = 5000;
    req.flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT;
    memset(&pmu_handle, 0, sizeof(pmu_handle));
    result = vdKernelPmuProfilerOpen(&req, &pmu_handle);
    if (result == 0) { pmu_active = 1; pmu_event_index = (pmu_event_index + 1) % 3; }
    else {
        snprintf(line, sizeof(line), "[VPROF] pmu open failed result=%d", result);
        if (debugnet_started) (void)uvdb_debugnet_write(UVDB_LOG_ERROR, line);
    }
}

static void drain_events(void) {
    struct vp_event ev[128]; size_t n;
    do { size_t i; n = vp_drain(&ctx, ev, 128);
        for (i = 0; i < n; ++i) if (ev[i].type == VP_EVENT_ZONE_END && ev[i].value >= 0) {
            int z = zone_from_id(ev[i].name_id);
            if (z >= 0) { zone_total[z] += (uint64_t)ev[i].value; zone_samples[z]++; }
        }
    } while (n != 0);
}

static void report(void) {
    struct vp_stats stats; char line[448]; size_t used = 0; int i;
    used += (size_t)snprintf(line + used, sizeof(line) - used, "[VPROF] frames=%u", (unsigned)frames);
    for (i = 0; i < MDKR_VP_ZONE_COUNT && used < sizeof(line); ++i) {
        uint64_t avg = zone_samples[i] ? zone_total[i] / zone_samples[i] : 0;
        used += (size_t)snprintf(line + used, sizeof(line) - used, " z%d=%" PRIu64 "us/%u", i, avg, (unsigned)zone_samples[i]);
    }
    if (frames && used < sizeof(line)) used += (size_t)snprintf(line + used, sizeof(line) - used, " draws=%" PRIu64, draw_total / frames);
    if (vp_get_stats(&ctx, &stats) == VP_RESULT_OK && used < sizeof(line))
        (void)snprintf(line + used, sizeof(line) - used, " dropped=%u", (unsigned)stats.dropped);
    if (debugnet_started) (void)uvdb_debugnet_write(UVDB_LOG_INFO, line);
    memset(zone_total, 0, sizeof(zone_total)); memset(zone_samples, 0, sizeof(zone_samples)); draw_total = 0; frames = 0;
}

void mdkr_vita_profiler_init(void) {
    struct vp_name_dictionary_config nc;
    SceNetInitParam net_config;
    char line[192];
    int i, result, module_result, net_result;
    if (enabled) return;
    module_result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    net_module_owned = module_result == 0;
    memset(&net_config, 0, sizeof(net_config));
    net_config.memory = debugnet_memory;
    net_config.size = sizeof(debugnet_memory);
    net_result = sceNetInit(&net_config);
    net_owned = net_result == 0;
    /* A host network owner may have initialized SceNet before the profiler.
     * In that case sceNetInit reports an existing lifecycle rather than
     * granting ownership. Let DebugNet probe asynchronously after startup;
     * failures remain harmless and are retried from the frame loop. */
    debugnet_network_ready = module_result >= 0;
    debugnet_retry_frames = REPORT_FRAMES;
    memset(&nc, 0, sizeof(nc));
    nc.entries = name_entries; nc.entry_capacity = MDKR_VP_ZONE_COUNT + 2;
    nc.text = name_text; nc.text_capacity = sizeof(name_text);
    if (vp_vita_init(&ctx, slots, RING_CAPACITY) != VP_RESULT_OK || vp_name_dictionary_init(&names, &nc) != VP_RESULT_OK) return;
    for (i = 0; i < MDKR_VP_ZONE_COUNT; ++i)
        if (vp_name_dictionary_register(&names, zone_names[i], &zone_ids[i]) != VP_RESULT_OK) return;
    if (vp_name_dictionary_register(&names, "game.frame", &frame_id) != VP_RESULT_OK ||
        vp_name_dictionary_register(&names, "vitagl.draw_calls", &draw_id) != VP_RESULT_OK ||
        vp_name_dictionary_seal(&names) != VP_RESULT_OK) return;
    enabled = 1; memset(&pmu_info, 0, sizeof(pmu_info));
    pmu_info.struct_size = sizeof(pmu_info); pmu_info.abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    result = vdKernelPmuProfilerGetInfo(&pmu_info);
    snprintf(line, sizeof(line), "[VPROF] enabled pmu_info=%d abi=%u caps=0x%08x core=%u lane=%u", result,
             (unsigned)pmu_info.abi_version, result == 0 ? (unsigned)pmu_info.capabilities : 0,
             (unsigned)pmu_info.fixed_core, (unsigned)pmu_info.fixed_counter);
    if (debugnet_started) (void)uvdb_debugnet_write(UVDB_LOG_INFO, line);
    if (result != 0 || pmu_info.abi_version != VD_KERNEL_PMU_PROFILER_ABI_VERSION) memset(&pmu_info, 0, sizeof(pmu_info));
}

void mdkr_vita_profiler_shutdown(void) {
    struct uvdb_debugnet_stats stats;
    char line[160];
    if (enabled) {
        pmu_close_report();
        drain_events();
        if (frames) report();
        vp_name_dictionary_deinit(&names);
        vp_deinit(&ctx);
        enabled = 0;
    }
    if (debugnet_started) {
        if (uvdb_debugnet_get_stats(&stats) == 0) {
            snprintf(line, sizeof(line),
                     "[VPROF] debugnet sent=%u dropped=%u truncated=%u errors=%u",
                     stats.sent, stats.dropped, stats.truncated,
                     stats.send_errors);
            (void)uvdb_debugnet_write(UVDB_LOG_INFO, line);
        }
        (void)uvdb_debugnet_stop();
        debugnet_started = 0;
    }
    if (net_owned) {
        (void)sceNetTerm();
        net_owned = 0;
    }
    if (net_module_owned) {
        (void)sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        net_module_owned = 0;
    }
    debugnet_network_ready = 0;
    debugnet_retry_frames = 0;
}
void mdkr_vita_profiler_frame_begin(void) {
    debugnet_try_start();
    if (!enabled) return;
    if (!pmu_active && frames == 0u) pmu_open();
    (void)vp_frame_mark(&ctx, frame_id);
}
void mdkr_vita_profiler_frame_end(void) {
    if (!enabled) return;
    drain_events();
    frames++;
    if (frames >= REPORT_FRAMES) { pmu_close_report(); report(); pmu_open(); }
}
void mdkr_vita_profiler_zone_begin(MdkrVitaProfileZone z, MdkrVitaProfileScope *s) {
    if (s) memset(s, 0, sizeof(*s));
    if (enabled && s && z >= 0 && z < MDKR_VP_ZONE_COUNT) (void)vp_zone_begin(&ctx, zone_ids[z], (struct vp_zone_scope *)s);
}
void mdkr_vita_profiler_zone_end(MdkrVitaProfileScope *s) { if (enabled && s) (void)vp_zone_end(&ctx, (struct vp_zone_scope *)s); }
void mdkr_vita_profiler_count_draw(void) { if (enabled) draw_total++; }
#endif
