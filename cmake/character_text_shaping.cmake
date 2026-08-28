# Pinned, ROM-independent Unicode shaping for custom-character names.
#
# HarfBuzz turns Unicode text into positioned OpenType glyphs; SheenBidi
# supplies the Unicode Bidirectional Algorithm. Both are linked statically and
# run only on bounded 96-codepoint identity fields. A local archive mirror
# keeps release builds reproducible when the network is unavailable.

if(TARGET mdkr_character_text_shaping)
    return()
endif()

include(FetchContent)

set(MDKR_HARFBUZZ_COMMIT
    "36cb489cb02ce4b92099669ba9f9bea348eff93f") # 14.4.0
set(MDKR_SHEENBIDI_COMMIT
    "cfe430e7375a7845b679adae9d51dac6deaa8858") # 3.0.0

function(mdkr_character_text_archive name remote out_url)
    set(url "${remote}")
    if(NOT "$ENV{MDKR_CHARACTER_TEXT_DEP_CACHE}" STREQUAL "")
        set(candidate "$ENV{MDKR_CHARACTER_TEXT_DEP_CACHE}/${name}")
        if(EXISTS "${candidate}")
            set(url "file://${candidate}")
        endif()
    endif()
    set(${out_url} "${url}" PARENT_SCOPE)
endfunction()

mdkr_character_text_archive(
    "harfbuzz-${MDKR_HARFBUZZ_COMMIT}.tar.gz"
    "https://github.com/harfbuzz/harfbuzz/archive/${MDKR_HARFBUZZ_COMMIT}.tar.gz"
    _mdkr_harfbuzz_url)
mdkr_character_text_archive(
    "sheenbidi-${MDKR_SHEENBIDI_COMMIT}.tar.gz"
    "https://github.com/Tehreer/SheenBidi/archive/${MDKR_SHEENBIDI_COMMIT}.tar.gz"
    _mdkr_sheenbidi_url)

# Keep HarfBuzz to its deterministic, platform-neutral OpenType core. Native
# system shapers and optional paint/subset stacks would add size and could make
# the same package name render differently between hosts.
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
set(HB_HAVE_DIRECTWRITE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GDI OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_UNISCRIBE OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU_DEMO OFF CACHE STRING "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
set(HB_HAVE_INTROSPECTION OFF CACHE BOOL "" FORCE)

set(SB_CONFIG_EXPERIMENTAL_TEXT_API OFF CACHE BOOL "" FORCE)
set(SB_CONFIG_UNITY ON CACHE BOOL "" FORCE)
set(BUILD_GENERATOR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mdkr_harfbuzz
    URL "${_mdkr_harfbuzz_url}"
    URL_HASH
        "SHA256=0afa12c8ef4bc4ffebd99e5d2a4a2c56dfe329c661feda08a9bc878b7352be89"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(mdkr_sheenbidi
    URL "${_mdkr_sheenbidi_url}"
    URL_HASH
        "SHA256=9546f423820d4618c704a30c51c191b0b002aa79100e0ec34cb2cf461fd6353e"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mdkr_harfbuzz mdkr_sheenbidi)

add_library(mdkr_character_text_shaping INTERFACE)
target_link_libraries(mdkr_character_text_shaping INTERFACE
    harfbuzz SheenBidi::SheenBidi)
