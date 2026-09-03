import sys

path = sys.argv[1] + r"/platform/main_pc.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

# The previous patch script naively inserted the vitashark.h include after
# the file's first line, assuming that line was a preprocessor directive or
# blank -- but main_pc.c actually opens with a "/**" block comment, so the
# #include ended up INSIDE the comment (inert). Remove that misplaced block
# and re-insert it right after the comment actually closes.
bad = (
    "/**\n"
    "#if defined(__vita__)\n"
    "#include <vitashark.h>\n"
    "#endif\n"
)
assert src.count(bad) == 1, "bad count=%d" % src.count(bad)
src = src.replace(bad, "/**\n", 1)

# Re-insert right after the first block comment's closing " */" line.
anchor = "\n */\n"
idx = src.find(anchor)
assert idx != -1, "could not find end of opening block comment"
insert_at = idx + len(anchor)
include_block = (
    "#if defined(__vita__)\n"
    "#include <vitashark.h>\n"
    "#endif\n"
)
assert "#include <vitashark.h>" not in src
src = src[:insert_at] + include_block + src[insert_at:]

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
