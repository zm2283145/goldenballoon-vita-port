import sys

path = sys.argv[1] + r"/platform/fast3d/gfx_opengl.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "            char lb[600];\n"
    "            snprintf(lb, sizeof(lb), \"shader: rewritten VS (len=%u):\\n%.*s\",\n"
    "                     (unsigned)vs_len, (int)(vs_len < 500 ? vs_len : 500), vs_buf);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            snprintf(lb, sizeof(lb), \"shader: rewritten FS (len=%u):\\n%.*s\",\n"
    "                     (unsigned)fs_len, (int)(fs_len < 500 ? fs_len : 500), fs_buf);\n"
    "            mdkr_vita_boot_log(lb);\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "            char lb[1700];\n"
    "            snprintf(lb, sizeof(lb), \"shader: rewritten VS (len=%u):\\n%.*s\",\n"
    "                     (unsigned)vs_len, (int)(vs_len < 1600 ? vs_len : 1600), vs_buf);\n"
    "            mdkr_vita_boot_log(lb);\n"
    "            snprintf(lb, sizeof(lb), \"shader: rewritten FS (len=%u):\\n%.*s\",\n"
    "                     (unsigned)fs_len, (int)(fs_len < 1600 ? fs_len : 1600), fs_buf);\n"
    "            mdkr_vita_boot_log(lb);\n"
)
src = src.replace(old, new, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
