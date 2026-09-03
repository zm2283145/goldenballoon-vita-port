import sys

path = sys.argv[1] + r"/platform/platform_sdl_min.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "int platform_sdl_surface_presentable(void) {\n"
    "#ifdef __EMSCRIPTEN__\n"
    "    return 1;\n"
    "#else\n"
    "    if (s_window == NULL) {\n"
    "        return 0;\n"
    "    }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "int platform_sdl_surface_presentable(void) {\n"
    "#ifdef __EMSCRIPTEN__\n"
    "    return 1;\n"
    "#elif defined(__vita__)\n"
    "    /* vitaGL owns display/context creation directly via sceGxm; s_window\n"
    "     * is intentionally always NULL here (see sdl_init_gl's Vita branch), so\n"
    "     * the s_window == NULL check below -- meant to detect \"no window has\n"
    "     * been created yet\" on desktop -- misfires as \"never presentable\" on\n"
    "     * Vita. That silently and permanently elided every present after the\n"
    "     * very first frame (present_sched_set_surface_elided(true) latches),\n"
    "     * which is why the game ran (audio/input/logic all fine) behind a\n"
    "     * black screen: vglSwapBuffers was simply never being called again.\n"
    "     * There is no SDL-window occlusion/minimize concept on Vita, so the\n"
    "     * surface is always presentable here. */\n"
    "    return 1;\n"
    "#else\n"
    "    if (s_window == NULL) {\n"
    "        return 0;\n"
    "    }\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
