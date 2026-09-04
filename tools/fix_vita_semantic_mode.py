import sys

path = sys.argv[1] + r"/platform/platform_sdl_min.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "        snprintf(glb, sizeof(glb), \"vitaGL: mem free VRAM=%u RAM=%u PHYCONT=%u\",\n"
    "                 (unsigned)vglMemFree(VGL_MEM_VRAM), (unsigned)vglMemFree(VGL_MEM_RAM),\n"
    "                 (unsigned)vglMemFree(VGL_MEM_PHYCONT));\n"
    "        mdkr_vita_boot_log(glb);\n"
    "    }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "        snprintf(glb, sizeof(glb), \"vitaGL: mem free VRAM=%u RAM=%u PHYCONT=%u\",\n"
    "                 (unsigned)vglMemFree(VGL_MEM_VRAM), (unsigned)vglMemFree(VGL_MEM_RAM),\n"
    "                 (unsigned)vglMemFree(VGL_MEM_PHYCONT));\n"
    "        mdkr_vita_boot_log(glb);\n"
    "        /* DIAGNOSTIC: vitaGL defaults to VGL_MODE_POSTPONED, which per its\n"
    "         * own header comment moves the REAL shader compilation (the runtime\n"
    "         * GLSL->Cg translation + shark_compile_shader call) out of\n"
    "         * glCompileShader and into glLinkProgram -- exactly the call where\n"
    "         * this engine's very first real shader crashes on real hardware.\n"
    "         * gfx_opengl_create_and_load_new_shader() already compiles vertex\n"
    "         * then fragment back-to-back for every shader (the exact usage\n"
    "         * pattern VGL_MODE_SHADER_PAIR documents as its premise), so force\n"
    "         * that mode instead: it does the real translation/compile at\n"
    "         * glCompileShader time (proven working per our \"both stages\n"
    "         * compiled OK\" logs) rather than deferring it into glLinkProgram. */\n"
    "        vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);\n"
    "        mdkr_vita_boot_log(\"vitaGL: forced vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR)\");\n"
    "    }\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
