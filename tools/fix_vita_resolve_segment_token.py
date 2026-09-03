import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
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
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "        if (flip != 0 && flip < 0x10000000u) {\n"
    "            /* flip lands in this codebase's own documented \"genuine N64\n"
    "             * segment token\" range (0x01000000..0x0FFFFFFF, see the\n"
    "             * ILP32 DIRECT RECOVERY comment above and the addr<0x01000000\n"
    "             * handling below) -- it is NOT a real host pointer at all, so\n"
    "             * treating it as one (the bug that caused the original\n"
    "             * wild-jump crash) was wrong. But returning NULL outright was\n"
    "             * ALSO wrong: a value in exactly this range is a legitimate,\n"
    "             * still-unresolved segment-relative token that just needs the\n"
    "             * same gfx_resolve_addr() segment-table lookup used for the\n"
    "             * non-flipped `addr` case a few lines below -- not a direct\n"
    "             * pointer cast, and not a hard reject either. Blanket-\n"
    "             * rejecting instead of resolving silently discarded\n"
    "             * legitimate display-list data: observed on device as menu\n"
    "             * background art and other textures never appearing (boxes/\n"
    "             * flat colors instead) while content resolved through a\n"
    "             * different path rendered fine. gfx_resolve_addr() is\n"
    "             * documented to never return a wild pointer -- worst case is\n"
    "             * NULL -- so this keeps the original crash fixed while no\n"
    "             * longer dropping real data. */\n"
    "            void *seg = gfx_resolve_addr(flip);\n"
    "            static int s_resolveSegLogCount = 0;\n"
    "            if (s_resolveSegLogCount < 20) {\n"
    "                char lb[160];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"resolve: flip=0x%x is a segment token, gfx_resolve_addr -> %p\",\n"
    "                         (unsigned)flip, seg);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "                s_resolveSegLogCount++;\n"
    "            }\n"
    "            if (dkr_ptr_plausible(seg)) {\n"
    "                return dkr_retain_resolved_pointer(seg);\n"
    "            }\n"
    "            return NULL;\n"
    "        }\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
