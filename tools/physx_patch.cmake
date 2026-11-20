# Make PhysX 5's public source tree configurable straight from CMake, without NVIDIA's
# 'packman' dependency fetch (#185). Run as the ExternalProject PATCH_COMMAND:
#   cmake -DPHYSX_SRC=<SOURCE_DIR> -P physx_patch.cmake
#
# physx/compiler/public/CMakeLists.txt sets PUBLIC_RELEASE=1 unconditionally, which makes
# source/compiler/cmake/windows/CMakeLists.txt do an unconditional FILE(COPY) of freeglut /
# PhysXDevice DLLs from $ENV{PM_*} paths. Those are only populated when the SDK is built via
# generate_projects.bat; from a plain CMake configure they're empty and FILE(COPY) hard-errors
# before any target is defined. Gating the copy on PX_COPY_EXTERNAL_DLL alone (which we never
# set) skips it — it only ever mattered for the GLUT-based snippets, which we don't build.
#
# Idempotent: a second run over an already-patched tree changes nothing. Fails when the tree has neither the
# original nor the patched line (a PHYSX_GIT_TAG bump that changed it): otherwise the configure would fail
# later in that FILE(COPY), far from the cause.

if(NOT DEFINED PHYSX_SRC)
  message(FATAL_ERROR "physx_patch: PHYSX_SRC must be set to the ExternalProject SOURCE_DIR")
endif()

set(_f "${PHYSX_SRC}/physx/source/compiler/cmake/windows/CMakeLists.txt")
if(NOT EXISTS "${_f}")
  message(FATAL_ERROR "physx_patch: ${_f} not found")
endif()

set(_old "IF(PX_COPY_EXTERNAL_DLL OR PUBLIC_RELEASE)")
set(_new "IF(PX_COPY_EXTERNAL_DLL) # patched for #185 (was: OR PUBLIC_RELEASE) — skip packman DLL copy")

file(READ "${_f}" _c)
string(FIND "${_c}" "${_old}" _hasOld)
string(FIND "${_c}" "${_new}" _hasNew)
if(NOT _hasOld EQUAL -1)
  string(REPLACE "${_old}" "${_new}" _c "${_c}")
  file(WRITE "${_f}" "${_c}")
  message(STATUS "physx_patch: gated external-DLL copy on PX_COPY_EXTERNAL_DLL in ${_f}")
elseif(NOT _hasNew EQUAL -1)
  message(STATUS "physx_patch: already patched")
else()
  message(FATAL_ERROR "physx_patch: '${_old}' not found in ${_f} (upstream changed it?): update this patch for the new PhysX tag")
endif()
