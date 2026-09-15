# Pinned Basis Universal transcoder support for custom-character KTX2 images.
#
# Only the bounded runtime transcoder and its Zstandard decoder are fetched.
# Pulling the complete upstream archive would add more than 100 MiB of sample
# images and encoder tools that are neither compiled nor shipped. Every source
# byte below is pinned twice: an immutable KTX-Software commit in the URL and an
# independently reviewed SHA-256. A local mirror can be supplied for offline
# release builds with MDKR_BASISU_LOCAL_CACHE; it must preserve these relative
# paths and is still hash checked.

if(TARGET mdkr_basisu_transcoder)
    return()
endif()

set(MDKR_BASISU_COMMIT "4d6fc70eaf62ad0558e63e8d97eb9766118327a6")
set(_mdkr_basisu_root "${CMAKE_BINARY_DIR}/_deps/mdkr_basisu-src")
set(_mdkr_basisu_remote
    "https://raw.githubusercontent.com/KhronosGroup/KTX-Software/${MDKR_BASISU_COMMIT}")

function(mdkr_basisu_source relative_path sha256 out_var)
    set(destination "${_mdkr_basisu_root}/${relative_path}")
    get_filename_component(destination_dir "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_dir}")
    set(source_url "${_mdkr_basisu_remote}/${relative_path}")
    if(NOT "$ENV{MDKR_BASISU_LOCAL_CACHE}" STREQUAL "")
        set(candidate "$ENV{MDKR_BASISU_LOCAL_CACHE}/${relative_path}")
        if(EXISTS "${candidate}")
            set(source_url "file://${candidate}")
        endif()
    endif()
    file(DOWNLOAD "${source_url}" "${destination}"
        EXPECTED_HASH "SHA256=${sha256}"
        TLS_VERIFY ON
        STATUS download_status
        LOG download_log)
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
        list(GET download_status 1 status_message)
        message(FATAL_ERROR
            "Could not fetch pinned Basis Universal source ${relative_path}: "
            "${status_message}. Set MDKR_BASISU_LOCAL_CACHE to an offline "
            "mirror of commit ${MDKR_BASISU_COMMIT}.\n${download_log}")
    endif()
    set(${out_var} "${destination}" PARENT_SCOPE)
endfunction()

set(_mdkr_basisu_specs
    "external/basisu/transcoder/basisu.h|c20c28277651f05a9938c62a8dac9ed34f4ea88a010534a177e46e27121ca758"
    "external/basisu/transcoder/basisu_containers.h|adb895921c00178d83b32e6e7aea86a80c2523c1aad39bbb6da5b8277346899f"
    "external/basisu/transcoder/basisu_containers_impl.h|eb15de276e2dba1c885b4aadcf77dd4349644e4e44c04f4f671b2d0566a3a531"
    "external/basisu/transcoder/basisu_file_headers.h|f0706e3c261bbcbaab4fde4286306ae7a6f1bfe18c1238a387f28615abfcf885"
    "external/basisu/transcoder/basisu_transcoder.cpp|5af226fc675155905208c590452d0b3d7539029e47d9bfabf7fea5d0baeaeb48"
    "external/basisu/transcoder/basisu_transcoder.h|99e76405952928a78604e8aaf4057f0d9765f58eda41ff295cc135f1d9a35ef5"
    "external/basisu/transcoder/basisu_transcoder_internal.h|4a6ea305d12f671abf775a9439312355a008b1e13b4c647d20529c3aa47228c4"
    "external/basisu/transcoder/basisu_transcoder_tables_astc.inc|7dd41c648d3ed8e7d511eb4af865669c4dd50b8ecead62a68fca471138c46e1a"
    "external/basisu/transcoder/basisu_transcoder_tables_bc7_m5_alpha.inc|4d713b839594f429d95f329d60f8ad495e932619111b5dfddffb688e3634b495"
    "external/basisu/transcoder/basisu_transcoder_tables_bc7_m5_color.inc|e6f63d3ebce0ac84d801ec54614b6a4eeda3be3e994f3abbfc732377cfe5a531"
    "external/basisu/transcoder/basisu_transcoder_tables_dxt1_5.inc|301dead99bbffa60c906dc4f89b8fb2b4976c30dbe7aa1b261d550b2107f89de"
    "external/basisu/transcoder/basisu_transcoder_tables_dxt1_6.inc|ab1974899976f555e4bfb47b37195ad4466e2c72b7bccb3439ff9032fed0de19"
    "external/basisu/transcoder/basisu_transcoder_uastc.h|bb8764bad84399d7dfe0c6d544b0e1b0a16750a09119edaed8e58b9218eea62b"
    "external/basisu/zstd/zstd.c|48f5c8afa98801b3dff7a6e8f24ba0fe0aa84f872b40b3d1d303edf5715f175f"
    "external/basisu/zstd/zstd.h|41d0f43747d0dee56f60bd10aed262f193d725b7e11eb9e94aa4ad80183c7da8")

set(_mdkr_basisu_sources)
foreach(spec IN LISTS _mdkr_basisu_specs)
    string(REPLACE "|" ";" fields "${spec}")
    list(GET fields 0 relative_path)
    list(GET fields 1 expected_hash)
    mdkr_basisu_source("${relative_path}" "${expected_hash}" downloaded_source)
    list(APPEND _mdkr_basisu_sources "${downloaded_source}")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/character_basisu_alignment.cmake)
mdkr_basisu_align_blocks(
    "${_mdkr_basisu_root}/external/basisu/transcoder/basisu_transcoder.cpp")

add_library(mdkr_basisu_transcoder STATIC ${_mdkr_basisu_sources})
target_include_directories(mdkr_basisu_transcoder SYSTEM PUBLIC
    "${_mdkr_basisu_root}/external/basisu/transcoder")
target_compile_features(mdkr_basisu_transcoder PUBLIC cxx_std_11)
target_compile_definitions(mdkr_basisu_transcoder PRIVATE
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1
    BASISD_SUPPORT_ATC=0
    BASISD_SUPPORT_PVRTC1=0
    BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_FXT1=0
    BASISD_SUPPORT_ASTC_HIGHER_OPAQUE_QUALITY=0
    BASISD_SUPPORT_ETC2_EAC_RG11=0
    BASISU_FORCE_DEVEL_MESSAGES=0)
if(NOT MSVC)
    target_compile_options(mdkr_basisu_transcoder PRIVATE
        -fno-strict-aliasing -Wno-unused-function -Wno-unused-parameter)
endif()
set_target_properties(mdkr_basisu_transcoder PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

# Keep the C++ bridge out of the decompilation target's intentionally unusual
# include path and C compatibility flags. In particular, game/src/memory.h
# must never shadow the C++ standard library's <memory.h> while BasisU is being
# parsed.
add_library(mdkr_character_ktx2_bridge STATIC
    ${CMAKE_SOURCE_DIR}/platform/modern_character_ktx2.cpp)
target_include_directories(mdkr_character_ktx2_bridge PUBLIC
    ${CMAKE_SOURCE_DIR}/platform)
target_compile_features(mdkr_character_ktx2_bridge PUBLIC cxx_std_11)
target_link_libraries(mdkr_character_ktx2_bridge PRIVATE
    mdkr_basisu_transcoder)
if(NOT MSVC)
    target_compile_options(mdkr_character_ktx2_bridge PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-strict-aliasing)
endif()
