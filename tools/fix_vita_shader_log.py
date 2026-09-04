import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_opengl.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "    const GLchar *sources[2] = { vs_buf, fs_buf };\n"
    "    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };\n"
    "    GLint success;\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "#if defined(__vita__)\n"
    "    {\n"
    "        static int s_shaderLogCount = 0;\n"
    "        if (s_shaderLogCount < 20) {\n"
    "            char lb[192];\n"
    "            snprintf(lb, sizeof(lb),\n"
    "                     \"shader: about to compile+link id0=0x%llx id1=0x%x tex=%d,%d fog=%d \"\n"
    "                     \"alpha=%d 2cyc=%d inputs=%d worldpos=%d vs_len=%u fs_len=%u\",\n"
    "                     (unsigned long long)shader_id0, (unsigned)shader_id1,\n"
    "                     cc_features.used_textures[0], cc_features.used_textures[1],\n"
    "                     cc_features.opt_fog, cc_features.opt_alpha, cc_features.opt_2cyc,\n"
    "                     cc_features.num_inputs, cc_features.opt_world_pos,\n"
    "                     (unsigned)vs_len, (unsigned)fs_len);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            s_shaderLogCount++;\n"
    "        }\n"
    "    }\n"
    "#endif\n"
    "    const GLchar *sources[2] = { vs_buf, fs_buf };\n"
    "    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };\n"
    "    GLint success;\n"
)
src = src.replace(old, new, 1)

# extern decl, if not already present in this file
if "extern void mdkr_vita_boot_log" not in src:
    anchor = "static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {\n"
    assert src.count(anchor) == 1
    decl = "#if defined(__vita__)\nextern void mdkr_vita_boot_log(const char *msg);\n#endif\n"
    src = src.replace(anchor, decl + anchor, 1)

# Also log right after a successful link, and right after a successful
# compile of each stage, so if it still crashes we know exactly which of the
# three GL calls (vertex compile / fragment compile / link) it got to.
old_link = "    glLinkProgram(shader_program);\n"
assert src.count(old_link) == 1, "link count=%d" % src.count(old_link)
new_link = (
    "#if defined(__vita__)\n"
    "    mdkr_vita_boot_log(\"shader: both stages compiled OK, calling glLinkProgram\");\n"
    "#endif\n"
    "    glLinkProgram(shader_program);\n"
    "#if defined(__vita__)\n"
    "    mdkr_vita_boot_log(\"shader: glLinkProgram returned (survived)\");\n"
    "#endif\n"
)
src = src.replace(old_link, new_link, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
