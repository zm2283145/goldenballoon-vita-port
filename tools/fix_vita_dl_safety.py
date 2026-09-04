import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1. extern decl + a small Vita-only guard helper, right before dkr_scan_overlay_order.
anchor = "static void dkr_scan_overlay_order(Gfx *cmd, int depth, int limit,\n                                   DkrOverlayScan *scan) {\n"
assert src.count(anchor) == 1, src.count(anchor)

helper = (
    "#if defined(__vita__)\n"
    "extern void mdkr_vita_boot_log(const char *msg);\n"
    "/* A genuine Gfx* is always 4-byte aligned (two uint32_t words) and must lie\n"
    " * either inside the DKR arena stand-in or be a recognizable host pointer.\n"
    " * dkr_ptr_plausible()'s sign-extension guard is a no-op on 32-bit targets\n"
    " * (nothing to sign-extend into), so a resolution bug that would be caught\n"
    " * there on desktop sails through unfiltered here. This is a Vita-only\n"
    " * belt-and-suspenders check to turn a wild jump into a logged, safe abort\n"
    " * of just this sub-list walk instead of a crash into unrelated code. */\n"
    "static inline bool dkr_vita_sub_ptr_safe(const Gfx *sub, uint32_t raw_addr,\n"
    "                                          const char *where) {\n"
    "    uintptr_t up = (uintptr_t)sub;\n"
    "    static int s_rejectLogCount = 0;\n"
    "    if ((up & 3u) != 0u) {\n"
    "        if (s_rejectLogCount < 20) {\n"
    "            char lb[128];\n"
    "            snprintf(lb, sizeof(lb),\n"
    "                     \"dl-safety: rejecting misaligned %s sub=%p raw_addr=0x%x\",\n"
    "                     where, (void *)sub, (unsigned)raw_addr);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            s_rejectLogCount++;\n"
    "        }\n"
    "        return false;\n"
    "    }\n"
    "    return true;\n"
    "}\n"
    "#endif\n\n"
)
src = src.replace(anchor, helper + anchor, 1)

old_gdl = (
    "            case G_DL: {\n"
    "                uint8_t nopush = (uint8_t)C0(cmd, 16, 8);\n"
    "                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);\n"
    "                if (sub == NULL) {\n"
    "                    if (nopush == G_DL_NOPUSH) {\n"
    "                        return;\n"
    "                    }\n"
    "                    break;\n"
    "                }\n"
    "                if (nopush == G_DL_NOPUSH) {\n"
    "                    cmd = sub;\n"
    "                    start = cmd;\n"
    "                    continue;\n"
    "                }\n"
    "                dkr_scan_overlay_order(sub, depth + 1, 0, scan);\n"
    "                break;\n"
    "            }\n"
)
assert src.count(old_gdl) == 1, "old_gdl count=%d" % src.count(old_gdl)

new_gdl = (
    "            case G_DL: {\n"
    "                uint8_t nopush = (uint8_t)C0(cmd, 16, 8);\n"
    "                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);\n"
    "#if defined(__vita__)\n"
    "                if (sub != NULL &&\n"
    "                    !dkr_vita_sub_ptr_safe(sub, cmd->words.w1, \"G_DL\")) {\n"
    "                    sub = NULL;\n"
    "                }\n"
    "#endif\n"
    "                if (sub == NULL) {\n"
    "                    if (nopush == G_DL_NOPUSH) {\n"
    "                        return;\n"
    "                    }\n"
    "                    break;\n"
    "                }\n"
    "                if (nopush == G_DL_NOPUSH) {\n"
    "                    cmd = sub;\n"
    "                    start = cmd;\n"
    "                    continue;\n"
    "                }\n"
    "                dkr_scan_overlay_order(sub, depth + 1, 0, scan);\n"
    "                break;\n"
    "            }\n"
)
src = src.replace(old_gdl, new_gdl, 1)

old_gdmadl = (
    "            case G_DMADL: {\n"
    "                int count = (int)C0(cmd, 16, 8);\n"
    "                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);\n"
    "                if (sub != NULL && count > 0) {\n"
    "                    dkr_scan_overlay_order(sub, depth + 1, count, scan);\n"
    "                }\n"
    "                break;\n"
    "            }\n"
)
assert src.count(old_gdmadl) == 1, "old_gdmadl count=%d" % src.count(old_gdmadl)

new_gdmadl = (
    "            case G_DMADL: {\n"
    "                int count = (int)C0(cmd, 16, 8);\n"
    "                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);\n"
    "#if defined(__vita__)\n"
    "                if (sub != NULL &&\n"
    "                    !dkr_vita_sub_ptr_safe(sub, cmd->words.w1, \"G_DMADL\")) {\n"
    "                    sub = NULL;\n"
    "                }\n"
    "#endif\n"
    "                if (sub != NULL && count > 0) {\n"
    "                    dkr_scan_overlay_order(sub, depth + 1, count, scan);\n"
    "                }\n"
    "                break;\n"
    "            }\n"
)
src = src.replace(old_gdmadl, new_gdmadl, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
