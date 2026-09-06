#ifndef MDKR_VOID_RENDER_POLICY_H
#define MDKR_VOID_RENDER_POLICY_H

#include <string.h>

typedef enum MdkrVoidRenderPolicy {
    MDKR_VOID_AUTHORED,
    MDKR_VOID_BACKGROUND,
    MDKR_VOID_DISABLED_CONTROL,
    MDKR_VOID_DEPTH_WRITE_CONTROL
} MdkrVoidRenderPolicy;

/* Pure retains the authored depth-writing foreground curtain. Restored and
 * Remastered use it only behind actual scenery. Counterfactual render paths
 * require both the internal token and a dedicated-desktop attestation; an
 * ordinary inherited diagnostic variable cannot change player rendering. */
static inline MdkrVoidRenderPolicy mdkr_void_render_policy(
    int pure, const char *requested, const char *token, const char *dedicated) {
    if (requested && token && dedicated &&
        strcmp(token, "mdkr64-void-coverage-v1") == 0 && strcmp(dedicated, "1") == 0) {
        if (strcmp(requested, "authored") == 0) return MDKR_VOID_AUTHORED;
        if (strcmp(requested, "background") == 0) return MDKR_VOID_BACKGROUND;
        if (strcmp(requested, "disabled") == 0) return MDKR_VOID_DISABLED_CONTROL;
        if (strcmp(requested, "depth-write") == 0) return MDKR_VOID_DEPTH_WRITE_CONTROL;
    }
    return pure ? MDKR_VOID_AUTHORED : MDKR_VOID_BACKGROUND;
}

static inline int mdkr_void_before_scenery(MdkrVoidRenderPolicy policy) {
    return policy == MDKR_VOID_BACKGROUND || policy == MDKR_VOID_DEPTH_WRITE_CONTROL;
}

#endif
