import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
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
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "    if (addr >= 0x80000000u) {\n"
    "#if defined(__vita__)\n"
    "        /* This platform's own convention (see dkr_arena_init / the arena\n"
    "         * ceiling checks elsewhere in this file) treats anything below the\n"
    "         * 256 MB segment-token ceiling as NOT a real host pointer -- code,\n"
    "         * the arena, and every other legitimate Vita allocation this app\n"
    "         * owns live well above it. Unlike the arena-reconstruction loop\n"
    "         * just above (which only accepts a candidate that lands INSIDE the\n"
    "         * known arena window), this 'direct recovery' fast path had no\n"
    "         * floor at all and would hand back literally any bit pattern with\n"
    "         * bit 31 set as a trusted pointer. That is what produced the\n"
    "         * observed wild-jump crash: addr=0x815041f0 flipped to 0x015041f0,\n"
    "         * which is not a valid Vita host address, and got dereferenced\n"
    "         * anyway. Reject it here instead of one call site at a time. */\n"
    "        if (flip != 0 && flip < 0x10000000u) {\n"
    "            static int s_resolveRejectLogCount = 0;\n"
    "            if (s_resolveRejectLogCount < 20) {\n"
    "                char lb[128];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"resolve: REJECTING implausible flip addr=0x%x -> flip=0x%x (below 256MB floor)\",\n"
    "                         (unsigned)addr, (unsigned)flip);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_resolveRejectLogCount++;\n"
    "            }\n"
    "            return NULL;\n"
    "        }\n"
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
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
