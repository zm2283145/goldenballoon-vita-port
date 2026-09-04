import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_pc_dkr.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

anchor = "static inline void *dkr_resolve(uint32_t addr) {\n"
n = src.count(anchor)
if n != 1:
    print("ERROR: anchor count=%d" % n)
    sys.exit(1)

decl = "#if defined(__vita__)\nextern void mdkr_vita_boot_log(const char *msg);\n#endif\n"
if "extern void mdkr_vita_boot_log" in src.split(anchor)[0]:
    print("decl already present before dkr_resolve; skipping")
else:
    src = src.replace(anchor, decl + anchor, 1)
    with open(path, "wb") as f:
        f.write(src.encode("utf-8"))
    print("patched", path)
