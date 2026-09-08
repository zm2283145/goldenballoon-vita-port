import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

# 1) Introduce a single named constant right after DKR_DL_MAX_DEPTH, instead of
# three separate magic-number 4,000,000 literals scattered through the file.
old_define = (
    "#define DKR_DL_MAX_DEPTH   16          /* nested G_DL / G_DMADL recursion cap */\n"
)
assert src.count(old_define) == 1, "define count=%d" % src.count(old_define)

new_define = (
    "#define DKR_DL_MAX_DEPTH   16          /* nested G_DL / G_DMADL recursion cap */\n"
    "/* Per-list command-count safety ceiling for dkr_scan_overlay_order(),\n"
 " * dkr_scan_future_deformations(), and dkr_run_dl(). This used to be a\n"
    " * 4,000,000-iteration cap, sized to comfortably out-run any real, finite\n"
    " * DKR display list (which are at most a few thousand commands even fully\n"
    " * unrolled) and only ever meant to be a last-resort backstop against a\n"
    " * truly unterminated list. On Vita it turned out to double as an\n"
    " * accidental worst-case amplifier: a mis-resolved sub-DL pointer (fixed\n"
    " * data being read as if it were a display list) doesn't reliably contain\n"
    " * a G_ENDDL opcode, so the walk ran all the way to the cap -- confirmed\n"
    " * on-device via the dl-safety heartbeat log, which showed a single\n"
    " * top-level walk advancing tens of megabytes through memory before the\n"
    " * (previous) cap finally cut it off, once per frame. With\n"
    " * DKR_DL_MAX_DEPTH-deep recursion each carrying its own budget, the\n"
    " * worst case was effectively DKR_DL_MAX_DEPTH times this number of\n"
    " * wasted iterations in a single frame -- the direct cause of the \"very\n"
    " * slow fps\" regression. Lowered by 40x: still far above any legitimate\n"
    " * DKR list length, but bounds a runaway walk into non-DL memory to a\n"
    " * cost that can't visibly matter next to a real frame budget. */\n"
    "#define DKR_DL_SAFETY_MAX  100000L\n"
)
src = src.replace(old_define, new_define, 1)

# 2) Replace the three hardcoded 4,000,000 safety comparisons with the named
# constant.
old_a = "        if (++safety > 4000000L) {\n#if defined(__vita__)\n"
assert src.count(old_a) == 1, "old_a count=%d" % src.count(old_a)
new_a = "        if (++safety > DKR_DL_SAFETY_MAX) {\n#if defined(__vita__)\n"
src = src.replace(old_a, new_a, 1)

old_b = "        if (++safety > 4000000L || dkr_arena_room(cmd) < sizeof(Gfx)) {\n"
assert src.count(old_b) == 1, "old_b count=%d" % src.count(old_b)
new_b = "        if (++safety > DKR_DL_SAFETY_MAX || dkr_arena_room(cmd) < sizeof(Gfx)) {\n"
src = src.replace(old_b, new_b, 1)

old_c = (
    "        if (++safety > 4000000L) {\n"
    "            dkr_dl_fault(\"unterminated display list\", cmd, depth);\n"
    "            return;\n"
    "        }\n"
)
assert src.count(old_c) == 1, "old_c count=%d" % src.count(old_c)
new_c = (
    "        if (++safety > DKR_DL_SAFETY_MAX) {\n"
    "            dkr_dl_fault(\"unterminated display list\", cmd, depth);\n"
    "            return;\n"
    "        }\n"
)
src = src.replace(old_c, new_c, 1)

assert src.count("4000000L") == 0, "leftover 4000000L count=%d" % src.count("4000000L")

# 3) The heartbeat log fired every 200,000 iterations against the old
# 4,000,000 cap (20 heartbeats total). Against the new, 40x-smaller cap that
# would never fire at all; lower it so a run that's still hitting the cap
# keeps giving visible, spaced-out progress in the boot log.
old_heartbeat = "if ((safety % 200000L) == 0 && s_heartbeatLogCount < 20) {"
assert src.count(old_heartbeat) == 1, "heartbeat count=%d" % src.count(old_heartbeat)
new_heartbeat = "if ((safety % 5000L) == 0 && s_heartbeatLogCount < 20) {"
src = src.replace(old_heartbeat, new_heartbeat, 1)

# 4) Fix the now-inaccurate "4M" wording in the cap-hit log message.
old_msg = "\"dl-safety: dkr_scan_overlay_order hit 4M safety cap, depth=%d\","
assert src.count(old_msg) == 1, "msg count=%d" % src.count(old_msg)
new_msg = "\"dl-safety: dkr_scan_overlay_order hit safety cap, depth=%d\","
src = src.replace(old_msg, new_msg, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
