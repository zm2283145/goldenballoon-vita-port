import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

block = (
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

count = src.count(block)
assert count == 2, "expected exactly 2 duplicate copies, found %d" % count

# Collapse the two adjacent copies into one.
double = block + block
assert src.count(double) == 1, "double count=%d" % src.count(double)
src = src.replace(double, block, 1)

# Sanity: exactly one copy remains now.
assert src.count(block) == 1, "post-dedupe count=%d" % src.count(block)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("deduped", path)
