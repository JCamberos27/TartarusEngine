# Applies the patches in tools/assimp_patches/*.patch to a FetchContent-populated assimp tree.
#
#   cmake -DASSIMP_SRC=<dir> -DASSIMP_PATCH_DIR=<dir> -P apply_assimp_patches.cmake
#
# assimp is consumed straight from github at a pinned tag rather than forked, so every fix we
# need locally rides along as a patch file. This runs at configure time - after FetchContent
# has written the sources, before cmake --build compiles them - which is what lets a fresh
# clone build correctly with no manual step.
#
# Deliberately not stamp-driven: each patch is probed with `git apply --check`, and if that
# fails, with `--check --reverse`. Forward succeeds => apply it. Reverse succeeds => it is
# already there, skip. Both fail => the tree does not match the patch, which is worth failing
# the configure over rather than silently building assimp without our fix. That makes the step
# self-healing if _deps is deleted, re-cloned, or bumped to a new tag.

if(NOT DEFINED ASSIMP_SRC)
    message(FATAL_ERROR "apply_assimp_patches.cmake: ASSIMP_SRC is not set")
endif()
if(NOT DEFINED ASSIMP_PATCH_DIR)
    message(FATAL_ERROR "apply_assimp_patches.cmake: ASSIMP_PATCH_DIR is not set")
endif()
if(NOT EXISTS "${ASSIMP_SRC}")
    message(FATAL_ERROR "apply_assimp_patches.cmake: assimp source tree not found: ${ASSIMP_SRC}")
endif()

file(GLOB _patches "${ASSIMP_PATCH_DIR}/*.patch")
if(NOT _patches)
    return()
endif()

find_package(Git REQUIRED)

foreach(_patch IN LISTS _patches)
    get_filename_component(_name "${_patch}" NAME)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${_patch}"
        WORKING_DIRECTORY "${ASSIMP_SRC}"
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err
        OUTPUT_QUIET)

    if(_rc EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${ASSIMP_SRC}"
            RESULT_VARIABLE _rc
            ERROR_VARIABLE _err
            OUTPUT_QUIET)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "assimp: failed to apply ${_name}\n${_err}")
        endif()
        message(STATUS "assimp: applied ${_name}")
        continue()
    endif()

    # Already applied? That is the normal case on every configure after the first.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --reverse "${_patch}"
        WORKING_DIRECTORY "${ASSIMP_SRC}"
        RESULT_VARIABLE _rev_rc
        OUTPUT_QUIET
        ERROR_QUIET)
    if(_rev_rc EQUAL 0)
        continue()
    endif()

    message(FATAL_ERROR
        "assimp: ${_name} applies to neither the pristine nor the patched source tree.\n"
        "The assimp checkout at ${ASSIMP_SRC} no longer matches the patch - did the GIT_TAG\n"
        "change, or did something edit the sources directly?\n\n"
        "git apply --check said:\n${_err}")
endforeach()
