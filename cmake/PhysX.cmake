# NVIDIA PhysX 5, built once per cache as an ExternalProject. Kept apart from the root
# CMakeLists.txt so CI's PhysX cache is keyed on this file (plus tools/physx_*.cmake) alone:
# editing the engine's own build rules no longer rebuilds PhysX in both configurations.

# --- NVIDIA PhysX 5 (collision / rigid bodies / character mover — #185) --------------
# Built from source, pinned by git tag, nothing vendored — issue #185's "Path B", but via
# ExternalProject rather than FetchContent_MakeAvailable. PhysX's public CMake cannot share
# this tree: it FORCE-overwrites CMAKE_CONFIGURATION_TYPES to its own set
# (debug/checked/profile/release) and writes binaries under a compiler+CRT-tagged folder that
# doesn't map to $<CONFIG>. ExternalProject runs it in an isolated CMake invocation instead;
# tools/physx_harvest.cmake then stages the output into a plain Debug/ Release/ layout.
#
# Host-only: linked into TartarusEngine, never the reloadable TartarusGame / TartarusEditor
# DLLs. PR 1 (#185) uses only PxFoundation/PxPhysics/PxScene/PxDefaultCpuDispatcher/PxMaterial
# — an empty world stepped during Play. Cooking / character-kinematic libs get added in later
# PRs as the code that needs them lands.
include(ExternalProject)

set(PHYSX_GIT_TAG "107.3-physx-5.6.1"  CACHE STRING "NVIDIA PhysX 5 release tag to build")
# ExternalProject's default prefix for this target (we don't override PREFIX). Clone lands at
# <prefix>/src/physx_external, and the SDK root (with include/ and compiler/public) is its
# physx/ subdir.
set(PHYSX_PREFIX  "${CMAKE_BINARY_DIR}/physx_external-prefix")
set(PHYSX_SDK_DIR "${PHYSX_PREFIX}/src/physx_external/physx") # PhysX headers live here (include/)
set(PHYSX_STAGE   "${PHYSX_PREFIX}/stage")                    # harvested libs/dlls: Debug/ Release/

# Engine config -> PhysX config: Debug uses PhysX 'debug' (/MDd, matches the engine's default
# MSVC Debug CRT); every optimized config uses PhysX 'release' (/MD).
set(_physx_stage_for_config "$<IF:$<CONFIG:Debug>,Debug,Release>")

# This tag's build emits un-suffixed names (no "_64") into win.x86_64.vc143.md/. The three
# core modules are DLLs (import lib = <name>.lib); Extensions and PvdSDK are always static.
set(_physx_link_libs
  PhysX
  PhysXCommon
  PhysXFoundation
  PhysXCooking                   # PxCreateConvexMesh / PxCreateTriangleMesh (#185 PR 6)
  PhysXExtensions_static        # PxDefaultCpuDispatcherCreate, PxDefaultPvdSocketTransportCreate
  PhysXPvdSDK_static            # PxCreatePvd (used only under TARTARUS_PHYSX_PVD, cheap to always link)
)
set(_physx_runtime_dlls PhysX PhysXCommon PhysXFoundation PhysXCooking)

# Declared as BUILD_BYPRODUCTS below so a Ninja/Make generator knows these files are produced
# by the physx_external step and orders the engine link after it.
set(_physx_byproducts "")
foreach(_cfg Debug Release)
  foreach(_lib ${_physx_link_libs})
    list(APPEND _physx_byproducts "${PHYSX_STAGE}/${_cfg}/${_lib}.lib")
  endforeach()
endforeach()

ExternalProject_Add(physx_external
  GIT_REPOSITORY  https://github.com/NVIDIA-Omniverse/PhysX.git
  GIT_TAG         ${PHYSX_GIT_TAG}
  GIT_SHALLOW     TRUE
  GIT_PROGRESS    TRUE
  SOURCE_SUBDIR   physx/compiler/public
  # Skip PhysX's unconditional packman DLL copy so it configures without generate_projects.bat.
  PATCH_COMMAND   ${CMAKE_COMMAND} -DPHYSX_SRC=<SOURCE_DIR> -P ${CMAKE_SOURCE_DIR}/tools/physx_patch.cmake
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_GENERATOR_PLATFORM "x64"  # PhysX on Windows is x64-only (win.x86_64); the engine is too
  CMAKE_CACHE_ARGS
    -DPHYSX_ROOT_DIR:PATH=${PHYSX_SDK_DIR}
    -DPX_OUTPUT_LIB_DIR:PATH=${PHYSX_PREFIX}/build-out
    -DPX_OUTPUT_BIN_DIR:PATH=${PHYSX_PREFIX}/build-out
    -DTARGET_BUILD_PLATFORM:STRING=windows
    -DPX_BUILDSNIPPETS:BOOL=OFF
    -DPX_BUILDPVDRUNTIME:BOOL=OFF
    -DPX_GENERATE_STATIC_LIBRARIES:BOOL=OFF
    # Precise math (/fp:precise, not /fp:fast): under /fp:fast, sweep CCD returned a NaN pose for
    # a CCD sphere hitting a static box at ~15 m/s or more (a thrown ball froze mid-air).
    -DPX_FLOAT_POINT_PRECISE_MATH:BOOL=ON
    -DPX_GENERATE_GPU_PROJECTS:BOOL=OFF
    -DNV_USE_STATIC_WINCRT:BOOL=OFF   # match the engine's /MD (dynamic CRT) — the #1 bring-up footgun
    -DNV_USE_DEBUG_WINCRT:BOOL=ON     # PhysX 'debug' config -> /MDd, matching engine Debug
    -DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5
  # Build both configs the engine consumes, once. PhysX is a dependency built and cached here,
  # not part of the per-config engine build.
  BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --config debug
        COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --config release
  # PhysX's public build ships no install() rules — stage the tagged output ourselves.
  INSTALL_COMMAND   ${CMAKE_COMMAND} -DSRC=${PHYSX_PREFIX}/build-out/bin -DDST=${PHYSX_STAGE}
                    -P ${CMAKE_SOURCE_DIR}/tools/physx_harvest.cmake
  BUILD_BYPRODUCTS  ${_physx_byproducts}
)

# INTERFACE target the engine links against. Not IMPORTED (the .libs don't exist at configure
# time — they're produced by physx_external at build time); a plain INTERFACE lib carrying the
# paths + a hard dependency is the way to express that.
add_library(physx INTERFACE)
add_dependencies(physx physx_external)
# SYSTEM so PhysX's headers don't flood the engine's /W4 build with their own warnings.
target_include_directories(physx SYSTEM INTERFACE ${PHYSX_SDK_DIR}/include)
foreach(_lib ${_physx_link_libs})
  target_link_libraries(physx INTERFACE "${PHYSX_STAGE}/${_physx_stage_for_config}/${_lib}.lib")
endforeach()
