# Pinned meshoptimizer simplifier for offline custom-character LOD authoring.
#
# The launcher never loads this code into the game process. The frozen importer
# invokes the bounded stdin/stdout helper beside it, records the exact version
# and error, and emits a new GLB rather than mutating an artist's original.

if(TARGET mdkr-character-lod)
    return()
endif()

set(MDKR_MESHOPTIMIZER_COMMIT "9d9890c73011d75920af614485296d1e03e95448")
set(_mdkr_meshoptimizer_root
    "${CMAKE_BINARY_DIR}/_deps/mdkr_meshoptimizer-src")
set(_mdkr_meshoptimizer_remote
    "https://raw.githubusercontent.com/zeux/meshoptimizer/${MDKR_MESHOPTIMIZER_COMMIT}")

function(mdkr_meshoptimizer_source relative_path sha256 out_var)
    set(destination "${_mdkr_meshoptimizer_root}/${relative_path}")
    get_filename_component(destination_dir "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_dir}")
    set(source_url "${_mdkr_meshoptimizer_remote}/${relative_path}")
    if(NOT "$ENV{MDKR_MESHOPTIMIZER_LOCAL_CACHE}" STREQUAL "")
        set(candidate "$ENV{MDKR_MESHOPTIMIZER_LOCAL_CACHE}/${relative_path}")
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
            "Could not fetch pinned meshoptimizer source ${relative_path}: "
            "${status_message}. Set MDKR_MESHOPTIMIZER_LOCAL_CACHE to an "
            "offline mirror of commit ${MDKR_MESHOPTIMIZER_COMMIT}.\n"
            "${download_log}")
    endif()
    set(${out_var} "${destination}" PARENT_SCOPE)
endfunction()

mdkr_meshoptimizer_source(
    "src/meshoptimizer.h"
    "21a72040a75bacf6ddefb7e74f1cf566af1e68ea5e4dc0db598278f4681e0b87"
    _mdkr_meshoptimizer_header)
mdkr_meshoptimizer_source(
    "src/simplifier.cpp"
    "dc40aadb307577ed3f7adb5102a506263de8b9fea1d5582a24c04bff2874a2cc"
    _mdkr_meshoptimizer_simplifier)

add_library(mdkr_meshoptimizer_simplifier STATIC
    ${_mdkr_meshoptimizer_header}
    ${_mdkr_meshoptimizer_simplifier})
target_include_directories(mdkr_meshoptimizer_simplifier SYSTEM PUBLIC
    "${_mdkr_meshoptimizer_root}/src")
target_compile_features(mdkr_meshoptimizer_simplifier PUBLIC cxx_std_11)
set_target_properties(mdkr_meshoptimizer_simplifier PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(mdkr_meshoptimizer_simplifier PRIVATE
        -Wno-unused-function -Wno-unused-parameter)
endif()

add_executable(mdkr-character-lod
    ${CMAKE_SOURCE_DIR}/tools/character_lod_simplifier.cpp)
target_compile_features(mdkr-character-lod PRIVATE cxx_std_11)
target_link_libraries(mdkr-character-lod PRIVATE
    mdkr_meshoptimizer_simplifier)
if(MSVC)
    target_compile_options(mdkr-character-lod PRIVATE /W4 /WX)
else()
    target_compile_options(mdkr-character-lod PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
# The same static runtime the main target takes, and for the same reason: this
# is a C++ tool, so a MinGW link imports libgcc_s_seh-1.dll and libstdc++-6.dll,
# neither of which exists on a player's machine. The portable package ships one
# executable per tool and no DLLs beside them, so the tool would simply fail to
# start. tools/check_windows_imports.sh names libgcc_s_seh-1.dll as a broken
# control in its own self-test -- the rule was always here, this target just
# never took it.
if(MINGW)
    target_link_options(mdkr-character-lod PRIVATE
        -static-libgcc -static-libstdc++)
    # -static outright, not the main target's selective -Bstatic winpthread:
    # this tool links no SDL and no system C++ library that needs to stay
    # shared, and the selective form still left libwinpthread-1.dll in the
    # import table because -static-libstdc++ pulls it back in on its own.
    target_link_options(mdkr-character-lod PRIVATE -static)
endif()

set_target_properties(mdkr-character-lod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
