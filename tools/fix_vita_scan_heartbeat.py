import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "        if (++safety > 4000000L) {\n"
    "            return;\n"
    "        }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "        if (++safety > 4000000L) {\n"
    "#if defined(__vita__)\n"
    "            {\n"
    "                char lb[96];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"dl-safety: dkr_scan_overlay_order hit 4M safety cap, depth=%d\",\n"
    "                         depth);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "            }\n"
    "#endif\n"
    "            return;\n"
    "        }\n"
    "#if defined(__vita__)\n"
    "        {\n"
    "            static int s_heartbeatLogCount = 0;\n"
    "            if ((safety % 200000L) == 0 && s_heartbeatLogCount < 20) {\n"
    "                char lb[96];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"dl-safety: heartbeat safety=%ld depth=%d cmd=%p\",\n"
    "                         safety, depth, (void *)cmd);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_heartbeatLogCount++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
