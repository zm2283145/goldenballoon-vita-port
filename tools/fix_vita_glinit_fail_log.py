import sys

path = sys.argv[1] + r"/platform/platform_sdl_min.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "    GLboolean vglOk = vglInitExtended(0, s_initialWindowWidth, s_initialWindowHeight, 0x20000,\n"
    "                     SCE_GXM_MULTISAMPLE_NONE);\n"
    "    if (!vglOk) {\n"
    "        fprintf(stderr, \"[SDL] vglInitExtended FAILED\\n\");\n"
    "        return -1;\n"
    "    }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "    GLboolean vglOk = vglInitExtended(0, s_initialWindowWidth, s_initialWindowHeight, 0x20000,\n"
    "                     SCE_GXM_MULTISAMPLE_NONE);\n"
    "    if (!vglOk) {\n"
    "        fprintf(stderr, \"[SDL] vglInitExtended FAILED\\n\");\n"
    "        /* This used to be stderr-only, which is invisible on Vita (nothing\n"
    "         * captures it there) -- every prior \"platform_sdl_init FAILED\"\n"
    "         * boot-log line told us THAT init failed but never WHY, since this\n"
    "         * is the only call inside it that can actually fail. vglInitExtended\n"
    "         * failing outright (as opposed to crashing/aborting later) is a\n"
    "         * known vitaGL/sceGxm symptom of a previous process's GPU context\n"
    "         * not being released cleanly -- exactly what repeated abort()s /\n"
    "         * force-closes during bring-up leave behind -- so log it plainly. */\n"
    "        mdkr_vita_boot_log(\"vitaGL: vglInitExtended FAILED (returned 0)\");\n"
    "        return -1;\n"
    "    }\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
