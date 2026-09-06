#include <stdio.h>
#include "void_render_policy.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int main(void) {
    const char *token = "mdkr64-void-coverage-v1";
    const char *names[] = {"authored", "background", "disabled", "depth-write"};
    const MdkrVoidRenderPolicy modes[] = {MDKR_VOID_AUTHORED, MDKR_VOID_BACKGROUND,
        MDKR_VOID_DISABLED_CONTROL, MDKR_VOID_DEPTH_WRITE_CONTROL};
    for (int pure = 0; pure < 2; ++pure) {
        MdkrVoidRenderPolicy normal = pure ? MDKR_VOID_AUTHORED : MDKR_VOID_BACKGROUND;
        CHECK(mdkr_void_render_policy(pure, NULL, NULL, NULL) == normal);
        CHECK(mdkr_void_render_policy(pure, NULL, token, "1") == normal);
        CHECK(mdkr_void_render_policy(pure, "typo", token, "1") == normal);
        CHECK(mdkr_void_render_policy(pure, "", token, "1") == normal);
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            CHECK(mdkr_void_render_policy(pure, names[i], NULL, "1") == normal);
            CHECK(mdkr_void_render_policy(pure, names[i], token, NULL) == normal);
            CHECK(mdkr_void_render_policy(pure, names[i], "wrong", "1") == normal);
            CHECK(mdkr_void_render_policy(pure, names[i], token, "0") == normal);
            CHECK(mdkr_void_render_policy(pure, names[i], token, "11") == normal);
            CHECK(mdkr_void_render_policy(pure, names[i], token, "1") == modes[i]);
        }
    }
    CHECK(!mdkr_void_before_scenery(MDKR_VOID_AUTHORED));
    CHECK(!mdkr_void_before_scenery(MDKR_VOID_DISABLED_CONTROL));
    CHECK(mdkr_void_before_scenery(MDKR_VOID_BACKGROUND));
    CHECK(mdkr_void_before_scenery(MDKR_VOID_DEPTH_WRITE_CONTROL));
    puts("void render policy: PASS (defaults, Pure, token/desktop gates, controls)");
    return 0;
}
