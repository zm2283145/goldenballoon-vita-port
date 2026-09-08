import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "    fprintf(stderr,\n"
    "            \"[DL] %s at depth=%d cmd=%p words=%08x/%08x%s\\n\",\n"
    "            reason, depth, (const void *) cmd,\n"
    "            cmd != NULL ? cmd->words.w0 : 0,\n"
    "            cmd != NULL ? cmd->words.w1 : 0,\n"
    "            strict ? \" (strict: aborting)\" : \" (recovered: list stopped/skipped)\");\n"
    "    fflush(stderr);\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "    fprintf(stderr,\n"
    "            \"[DL] %s at depth=%d cmd=%p words=%08x/%08x%s\\n\",\n"
    "            reason, depth, (const void *) cmd,\n"
    "            cmd != NULL ? cmd->words.w0 : 0,\n"
    "            cmd != NULL ? cmd->words.w1 : 0,\n"
    "            strict ? \" (strict: aborting)\" : \" (recovered: list stopped/skipped)\");\n"
    "    fflush(stderr);\n"
    "#if defined(__vita__)\n"
    "    {\n"
    "        static int s_dlFaultLogCount = 0;\n"
    "        if (s_dlFaultLogCount < 20) {\n"
    "            char lb[160];\n"
    "            snprintf(lb, sizeof(lb),\n"
    "                     \"dl-fault: %s depth=%d cmd=%p words=%08x/%08x\",\n"
    "                     reason, depth, (const void *)cmd,\n"
    "                     cmd != NULL ? cmd->words.w0 : 0,\n"
    "                     cmd != NULL ? cmd->words.w1 : 0);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            s_dlFaultLogCount++;\n"
    "        }\n"
    "    }\n"
    "#endif\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
