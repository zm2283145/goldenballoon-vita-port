import sys

path = sys.argv[1] + r"/platform/main_pc.c"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old = (
    "        {\n"
    "            static const char *const kShacccgCandidates[] = {\n"
    "                \"ur0:data/libshacccg.suprx\",       /* vitaShaRK's actual default */\n"
    "                \"ur0:data/external/libshacccg.suprx\",\n"
    "                \"ux0:data/external/libshacccg.suprx\",\n"
    "            };\n"
    "            size_t si;\n"
    "            for (si = 0; si < sizeof(kShacccgCandidates) / sizeof(kShacccgCandidates[0]); ++si) {\n"
    "                FILE *shacccg = fopen(kShacccgCandidates[si], \"rb\");\n"
    "                char lb[192];\n"
    "                if (shacccg) {\n"
    "                    fclose(shacccg);\n"
    "                    snprintf(lb, sizeof(lb), \"boot: libshacccg.suprx FOUND at %s%s\",\n"
    "                             kShacccgCandidates[si],\n"
    "                             si == 0 ? \" (the path vitashark actually uses)\" : \"\");\n"
    "                } else {\n"
    "                    snprintf(lb, sizeof(lb), \"boot: libshacccg.suprx missing at %s\",\n"
    "                             kShacccgCandidates[si]);\n"
    "                }\n"
    "                mdkr_vita_boot_log(lb);\n"
    "            }\n"
    "        }\n"
)
assert src.count(old) == 1, "count=%d" % src.count(old)

new = (
    "        {\n"
    "            static const char *const kShacccgCandidates[] = {\n"
    "                \"ur0:data/libshacccg.suprx\",       /* vitaShaRK's actual default */\n"
    "                \"ur0:data/external/libshacccg.suprx\",\n"
    "                \"ux0:data/external/libshacccg.suprx\",\n"
    "            };\n"
    "            size_t si;\n"
    "            const char *working_path = NULL;\n"
    "            for (si = 0; si < sizeof(kShacccgCandidates) / sizeof(kShacccgCandidates[0]); ++si) {\n"
    "                FILE *shacccg = fopen(kShacccgCandidates[si], \"rb\");\n"
    "                char lb[192];\n"
    "                if (shacccg) {\n"
    "                    fclose(shacccg);\n"
    "                    if (working_path == NULL) {\n"
    "                        working_path = kShacccgCandidates[si];\n"
    "                    }\n"
    "                    snprintf(lb, sizeof(lb), \"boot: libshacccg.suprx FOUND at %s%s\",\n"
    "                             kShacccgCandidates[si],\n"
    "                             si == 0 ? \" (the path vitashark actually uses)\" : \"\");\n"
    "                } else {\n"
    "                    snprintf(lb, sizeof(lb), \"boot: libshacccg.suprx missing at %s\",\n"
    "                             kShacccgCandidates[si]);\n"
    "                }\n"
    "                mdkr_vita_boot_log(lb);\n"
    "            }\n"
    "            /* vitaGL lazy-inits vitaShaRK internally on the FIRST real\n"
    "             * glCompileShader call, using shark_init() with vitaShaRK's own\n"
    "             * compiled-in default path -- which this project's own comment\n"
    "             * above already flags as \"ur0:/data/libshacccg.suprx\" (WITH a\n"
    "             * slash after the colon), not the \"ur0:data/...\" (no slash)\n"
    "             * form the fopen() check above just verified actually opens.\n"
    "             * fopen()/newlib is forgiving about that slash; vitaShaRK's own\n"
    "             * internal sceIoOpen is not guaranteed to be. If vitaGL's lazy\n"
    "             * shark_init() silently fails on that path mismatch and it does\n"
    "             * not check the return value before compiling, every later\n"
    "             * shark_compile_shader/shark_get_internal_compile_output call\n"
    "             * runs against an uninitialized compiler -- exactly the SceGxm-\n"
    "             * internal crash observed on the very first real shader link.\n"
    "             * Call shark_init() ourselves, explicitly, with the path we just\n"
    "             * proved actually opens, before vitaGL ever gets a chance to. */\n"
    "            if (working_path != NULL) {\n"
    "                int shark_rc = shark_init(working_path);\n"
    "                char lb[128];\n"
    "                snprintf(lb, sizeof(lb),\n"
    "                         \"boot: explicit shark_init(\\\"%s\\\") returned %d\",\n"
    "                         working_path, shark_rc);\n"
    "                mdkr_vita_boot_log(lb);\n"
    "            } else {\n"
    "                mdkr_vita_boot_log(\n"
    "                    \"boot: no libshacccg.suprx candidate found; skipping explicit shark_init\");\n"
    "            }\n"
    "        }\n"
)
src = src.replace(old, new, 1)

# Add the vitashark.h include, Vita-only, near the top of the file.
if "#include <vitashark.h>" not in src:
    # Anchor on the first line, which is guaranteed to exist and be unique.
    lines = src.split("\n", 1)
    assert len(lines) == 2
    include_block = (
        "#if defined(__vita__)\n"
        "#include <vitashark.h>\n"
        "#endif\n"
    )
    src = lines[0] + "\n" + include_block + lines[1]

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
