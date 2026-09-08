# Harvest PhysX's build output into a stable, $<CONFIG>-mapped layout (#185).
#
# PhysX's public CMake writes its binaries under a compiler/CRT-tagged folder
# (bin/win.x86_64.vc143.md/{debug,release}/) with config names — debug/checked/profile/release
# — that don't line up with the engine's Debug/Release. This script, run as the ExternalProject
# install step, copies the .lib/.dll/.pdb set into <DST>/Debug and <DST>/Release so the engine
# can link and copy-next-to-exe with a simple $<IF:$<CONFIG:Debug>,Debug,Release> switch and
# never has to know PhysX's folder-naming scheme.
#
#   -DSRC=<physx build-out>/bin   -DDST=<stage dir>   -P physx_harvest.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
  message(FATAL_ERROR "physx_harvest: SRC and DST must both be set")
endif()

file(GLOB _platformDirs LIST_DIRECTORIES true "${SRC}/win.*")
if(NOT _platformDirs)
  message(FATAL_ERROR "physx_harvest: no 'win.*' output folder under ${SRC} — did the PhysX build run?")
endif()

set(_copied 0)
foreach(_platformDir ${_platformDirs})
  foreach(_pair "debug:Debug" "release:Release")
    string(REPLACE ":" ";" _pair "${_pair}")
    list(GET _pair 0 _pxCfg)
    list(GET _pair 1 _engineCfg)
    set(_from "${_platformDir}/${_pxCfg}")
    if(EXISTS "${_from}")
      set(_to "${DST}/${_engineCfg}")
      file(MAKE_DIRECTORY "${_to}")
      file(GLOB _files "${_from}/*.lib" "${_from}/*.dll" "${_from}/*.pdb")
      foreach(_f ${_files})
        file(COPY "${_f}" DESTINATION "${_to}")
        math(EXPR _copied "${_copied} + 1")
      endforeach()
    endif()
  endforeach()
endforeach()

message(STATUS "physx_harvest: staged ${_copied} file(s) into ${DST}")
if(_copied EQUAL 0)
  message(FATAL_ERROR "physx_harvest: matched a win.* folder but found no debug/ or release/ binaries")
endif()
