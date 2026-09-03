import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "    if (addr >= 0x80000000u) {\n"
    "        return flip\n"
    "            ? dkr_retain_resolved_pointer((void *)(uintptr_t)flip) : NULL;\n"
    "    }\n"
    "    if (addr != 0 && addr < 0x01000000u) {\n"
    "        return dkr_retain_resolved_pointer((void *)(uintptr_t)addr);\n"
    "    }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "    if (addr >= 0x80000000u) {\n"
    "#if defined(__vita__)\n"
    "        {\n"
    "            static int s_resolveLogCount = 0;\n"
    "            if (s_resolveLogCount < 20) {\n"
    "                char lb[128];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"resolve: ILP32 flip-path addr=0x%x -> flip=0x%x\",\n"
    "                         (unsigned)addr, (unsigned)flip);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_resolveLogCount++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
    "        return flip\n"
    "            ? dkr_retain_resolved_pointer((void *)(uintptr_t)flip) : NULL;\n"
    "    }\n"
    "    if (addr != 0 && addr < 0x01000000u) {\n"
    "#if defined(__vita__)\n"
    "        {\n"
    "            static int s_resolveLowLogCount = 0;\n"
    "            if (s_resolveLowLogCount < 20) {\n"
    "                char lb[128];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"resolve: ILP32 low-path addr=0x%x\", (unsigned)addr);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_resolveLowLogCount++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
    "        return dkr_retain_resolved_pointer((void *)(uintptr_t)addr);\n"
    "    }\n"
)
src = src.replace(old, new, 1)

# extern decl for mdkr_vita_boot_log if not already present from the dl-safety patch
if "extern void mdkr_vita_boot_log" not in src:
    anchor = "static inline void *dkr_resolve(uint32_t addr) {\n"
    assert src.count(anchor) == 1
    decl = (
        "#if defined(__vita__)\n"
        "extern void mdkr_vita_boot_log(const char *msg);\n"
        "#endif\n"
    )
    src = src.replace(anchor, decl + anchor, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
