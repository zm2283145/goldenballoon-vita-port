import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old_entry = (
    "    if (cmd == NULL || scan == NULL || depth >= DKR_DL_MAX_DEPTH) {\n"
    "        return;\n"
    "    }\n"
    "    start = cmd;\n"
)
assert src.count(old_entry) == 1, "entry count=%d" % src.count(old_entry)

new_entry = (
    "    if (cmd == NULL || scan == NULL || depth >= DKR_DL_MAX_DEPTH) {\n"
    "#if defined(__vita__)\n"
    "        if (depth == 0) {\n"
    "            static int s_earlyOutLogCount = 0;\n"
    "            if (s_earlyOutLogCount < 20) {\n"
    "                char lb[96];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"dl-safety: top-level early-out cmd=%p depth=%d\",\n"
    "                         (void *)cmd, depth);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_earlyOutLogCount++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
    "        return;\n"
    "    }\n"
    "#if defined(__vita__)\n"
    "    if (depth == 0) {\n"
    "        static int s_topEnterLogCount = 0;\n"
    "        if (s_topEnterLogCount < 20) {\n"
    "            char lb[96];\n"
    "            snprintf(lb, sizeof(lb), \"dl-safety: top-level ENTER cmd=%p limit=%d\",\n"
    "                     (void *)cmd, limit);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            s_topEnterLogCount++;\n"
    "        }\n"
    "    }\n"
    "#endif\n"
    "    start = cmd;\n"
)
src = src.replace(old_entry, new_entry, 1)

old_arena_bound = (
    "            if ((uc >= ae && uc < ae + 0x00010000u) ||\n"
    "                dkr_arena_room(cmd) < sizeof(Gfx)) {\n"
    "                return;\n"
    "            }\n"
)
assert src.count(old_arena_bound) == 1, "arena_bound count=%d" % src.count(old_arena_bound)

new_arena_bound = (
    "            if ((uc >= ae && uc < ae + 0x00010000u) ||\n"
    "                dkr_arena_room(cmd) < sizeof(Gfx)) {\n"
    "#if defined(__vita__)\n"
    "                if (depth == 0) {\n"
    "                    static int s_arenaBoundLogCount = 0;\n"
    "                    if (s_arenaBoundLogCount < 20) {\n"
    "                        char lb[96];\n"
    "                        snprintf(lb, sizeof(lb),\n"
    "                                 \"dl-safety: arena-bound return cmd=%p depth=%d\",\n"
    "                                 (void *)cmd, depth);\n"
    "                        mdkr_vita_boot_log(lb);\n"
    "                        s_arenaBoundLogCount++;\n"
    "                    }\n"
    "                }\n"
    "#endif\n"
    "                return;\n"
    "            }\n"
)
src = src.replace(old_arena_bound, new_arena_bound, 1)

# Also log a normal top-level EXIT so we know the scan actually completed and
# the hang (if any) is downstream of it, not inside it.
old_enddl = "            case (uint8_t)G_ENDDL:\n                return;\n"
assert src.count(old_enddl) == 1, "enddl count=%d" % src.count(old_enddl)
new_enddl = (
    "            case (uint8_t)G_ENDDL:\n"
    "#if defined(__vita__)\n"
    "                if (depth == 0) {\n"
    "                    static int s_topExitLogCount = 0;\n"
    "                    if (s_topExitLogCount < 20) {\n"
    "                        char lb[64];\n"
    "                        snprintf(lb, sizeof(lb), \"dl-safety: top-level ENDDL exit\");\n"
    "                        mdkr_vita_boot_log(lb);\n"
    "                        s_topExitLogCount++;\n"
    "                    }\n"
    "                }\n"
    "#endif\n"
    "                return;\n"
)
src = src.replace(old_enddl, new_enddl, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
