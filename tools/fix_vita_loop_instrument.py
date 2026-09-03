import sys

path = sys.argv[1] + r"/game/src/thread3_main.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

anchor = 'void thread3_main(UNUSED void *unused) {\n'
assert src.count(anchor) == 1, src.count(anchor)

decl = (
    "#if defined(__vita__)\n"
    "extern void mdkr_vita_boot_log(const char *msg);\n"
    "#endif\n\n"
)
if "extern void mdkr_vita_boot_log" not in src:
    src = src.replace(anchor, decl + anchor, 1)

old_loop_call = (
    "        main_game_loop();\n"
    "#ifdef NATIVE_PORT\n"
    "        if (platform_exit_requested()) {\n"
    "            break;\n"
    "        }\n"
    "#endif\n"
    "        thread3_verify_stack();\n"
)
assert src.count(old_loop_call) == 1, src.count(old_loop_call)

new_loop_call = (
    "#if defined(__vita__)\n"
    "        {\n"
    "            static int s_vitaLoopLogCount = 0;\n"
    "            if (s_vitaLoopLogCount < 20) {\n"
    "                char llb[64];\n"
    "                snprintf(llb, sizeof(llb), \"loop: iteration %d starting main_game_loop()\", s_vitaLoopLogCount);\n"
    "                mdkr_vita_boot_log(llb);\n"
    "                s_vitaLoopLogCount++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
    "        main_game_loop();\n"
    "#if defined(__vita__)\n"
    "        {\n"
    "            static int s_vitaLoopLogCount2 = 0;\n"
    "            if (s_vitaLoopLogCount2 < 20) {\n"
    "                char llb[64];\n"
    "                snprintf(llb, sizeof(llb), \"loop: iteration %d main_game_loop() returned\", s_vitaLoopLogCount2);\n"
    "                mdkr_vita_boot_log(llb);\n"
    "                s_vitaLoopLogCount2++;\n"
    "            }\n"
    "        }\n"
    "#endif\n"
    "#ifdef NATIVE_PORT\n"
    "        if (platform_exit_requested()) {\n"
    "            break;\n"
    "        }\n"
    "#endif\n"
    "        thread3_verify_stack();\n"
)
src = src.replace(old_loop_call, new_loop_call, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
