import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_opengl.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "    const char *input_interp =\n"
    "        (gfx_diag_noperspective_inputs_enabled() || cc_features.noperspective_inputs) ?\n"
    "        \"noperspective \" : \"\";\n"
    "    const char *texcoord_interp =\n"
    "        (gfx_diag_noperspective_texcoords_enabled() || cc_features.noperspective_texcoords) ?\n"
    "        \"noperspective \" : \"\";\n"
    "    const char *fog_interp = cc_features.noperspective_fog ? \"noperspective \" : \"\";\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "#if defined(__vita__)\n"
    "    /* DIAGNOSTIC: the crash that killed iteration 1 happened deep inside\n"
    "     * SceGxm's own shader-patcher link path (glLinkProgram), AFTER both\n"
    "     * shader stages compiled successfully -- so this is vitaShaRK/SceGxm\n"
    "     * choking on something in a valid-looking GLSL program, not a GLSL\n"
    "     * syntax error we'd catch. Frame 0's shader linked fine; frame 1 needs\n"
    "     * a different shader_id combination. `noperspective` is the most\n"
    "     * exotic interpolation qualifier this generator emits and is data-\n"
    "     * driven per shader (exactly the kind of thing that would differ\n"
    "     * between frame 0 and frame 1's shader needs) -- and vitaShaRK's\n"
    "     * GLSL->Cg/GXP translator is not guaranteed to support it correctly.\n"
    "     * Force it off on Vita as a testable hypothesis; a wrong perspective\n"
    "     * interpolation is a visual bug, not a crash, so this is safe to try. */\n"
    "    const char *input_interp = \"\";\n"
    "    const char *texcoord_interp = \"\";\n"
    "    const char *fog_interp = \"\";\n"
    "#else\n"
    "    const char *input_interp =\n"
    "        (gfx_diag_noperspective_inputs_enabled() || cc_features.noperspective_inputs) ?\n"
    "        \"noperspective \" : \"\";\n"
    "    const char *texcoord_interp =\n"
    "        (gfx_diag_noperspective_texcoords_enabled() || cc_features.noperspective_texcoords) ?\n"
    "        \"noperspective \" : \"\";\n"
    "    const char *fog_interp = cc_features.noperspective_fog ? \"noperspective \" : \"\";\n"
    "#endif\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
