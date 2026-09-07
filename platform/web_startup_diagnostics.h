#ifndef MDKR64_WEB_STARTUP_DIAGNOSTICS_H
#define MDKR64_WEB_STARTUP_DIAGNOSTICS_H

// Test-only, bounded browser-side phase history. Install the hook before module
// startup; enabling it later is intentionally unsupported. Ordinary web play
// performs one presence check per translation unit, then no per-frame JS calls.
// Native builds have no hook, logging, clock reads or additional work.
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
static inline void mdkr_web_startup_phase(const char *phase) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = EM_ASM_INT({
            return typeof globalThis.__mdkrStartupTrace === 'function' ? 1 : 0;
        });
    }
    if (!enabled) return;
    EM_ASM({
        // Diagnostic collection must never throw through renderer control flow.
        try {
            if (typeof globalThis.__mdkrStartupTrace === 'function') {
                globalThis.__mdkrStartupTrace(UTF8ToString($0));
            }
        } catch (error) {}
    }, phase);
}
#else
#define mdkr_web_startup_phase(phase) ((void)0)
#endif

#endif // MDKR64_WEB_STARTUP_DIAGNOSTICS_H
