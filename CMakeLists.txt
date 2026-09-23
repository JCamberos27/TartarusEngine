cmake_minimum_required(VERSION 3.16)
# The engine's own version, distinct from the per-project "product version" a user sets in
# Build Settings for the game they ship. Bump it here; the packaged ZIP name, the generated
# TartarusVersion.h and the About panel all follow from this one line. (#173 suggestion 4)
project(TartarusEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Version header ---------------------------------------------------------------------
# Generated into the build tree so nothing version-specific is checked in. Captured at CONFIGURE
# time on purpose: regenerating on every build would relink the engine after every commit, for a
# string whose only job is to identify a release build. CI configures from a clean checkout, so a
# packaged build always carries the right SHA.
find_package(Git QUIET)
set(TARTARUS_GIT_DESCRIBE "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _git_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_git_describe)
    set(TARTARUS_GIT_DESCRIBE "${_git_describe}")
  endif()
endif()
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/TartarusVersion.h.in
  ${CMAKE_BINARY_DIR}/generated/TartarusVersion.h
  @ONLY
)
if(WIN32)
  # VERSIONINFO resource: puts the version where Explorer and minidump module lists read it.
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/TartarusVersion.rc.in
    ${CMAKE_BINARY_DIR}/generated/TartarusVersion.rc
    @ONLY
  )
endif()

include(FetchContent)

# --- GLFW ---
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- GLM ---
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# --- Assimp (FBX / glTF / OBJ import + skeletal animation data) ---
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  assimp
  GIT_REPOSITORY https://github.com/assimp/assimp.git
  GIT_TAG v5.4.3
)
FetchContent_MakeAvailable(assimp)

# --- local assimp patches ---------------------------------------------------------------
# assimp comes straight off github at a pinned tag rather than forked, so fixes we need
# locally live in tools/assimp_patches/*.patch and are applied here - configure time, after
# the sources exist and before any of them compile - which is what makes a fresh clone build
# correctly with no manual step. CONFIGURE_DEPENDS makes editing a patch re-run configure;
# tools/apply_assimp_patches.cmake probes with `git apply --check` so each patch applies
# exactly once and a tag bump that invalidates one fails loudly instead of building without it.
file(GLOB ASSIMP_LOCAL_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/assimp_patches/*.patch")
if(ASSIMP_LOCAL_PATCHES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASSIMP_LOCAL_PATCHES})
  execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DASSIMP_SRC=${assimp_SOURCE_DIR}
            -DASSIMP_PATCH_DIR=${CMAKE_SOURCE_DIR}/tools/assimp_patches
            -P ${CMAKE_SOURCE_DIR}/tools/apply_assimp_patches.cmake
    RESULT_VARIABLE ASSIMP_PATCH_RESULT)
  if(NOT ASSIMP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply local assimp patches (see above)")
  endif()
endif()

# --- EnTT (entity-component system) — header-only, ships its own CMake target ---
# Populated + add_subdirectory(EXCLUDE_FROM_ALL) rather than FetchContent_MakeAvailable: v3.13.2's
# install() rules are UNCONDITIONAL (no ENTT_INSTALL option to switch off, unlike glfw and
# assimp above), so a plain MakeAvailable put include/entt - 73 headers - plus lib/EnTT CMake
# config files and a pkg-config file into the packaged ZIP. CMake ignores install rules from an
# EXCLUDE_FROM_ALL subdirectory, which is the documented way to consume a dependency you only
# want to build against. The EnTT::EnTT target is unaffected.
FetchContent_Declare(
  entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.13.2
)
FetchContent_GetProperties(entt)
if(NOT entt_POPULATED)
  FetchContent_Populate(entt)
  add_subdirectory(${entt_SOURCE_DIR} ${entt_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# --- Dear ImGui (editor UI) — no CMakeLists upstream, so populate + build manually ---
# On the "docking" branch (not a numbered release) specifically for real dock-node based
# panel layout: resizing one docked panel resizes its neighbors, independent of whether
# panels are locked from being moved/rearranged.
# Pinned to a commit on "docking", never the moving branch name: the host and TartarusEditor.dll
# each compile their own ImGui while sharing one ImGuiContext, so both must build from identical
# source for every clone. To upgrade, pick a newer docking commit, replace the SHA and note the
# date; CI's dependency cache is keyed on this file, so the bump invalidates it.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 545d1a884ab7be89bbab4600a649a206c8667773 # docking, 2026-09-16
)
FetchContent_MakeAvailable(imgui)

# --- ImGuizmo (editor viewport translate/rotate/scale gizmos) ---
# Its own CMakeLists.txt builds a standalone library we don't want (it doesn't know
# about our ImGui include path), so populate the source only and compile it ourselves.
# Pinned to a commit on "master" (no tagged releases track current ImGui); upgrade alongside ImGui.
FetchContent_Declare(
  imguizmo
  GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
  GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1 # master, 2026-08-08
)
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
  FetchContent_Populate(imguizmo)
endif()

# --- NVIDIA PhysX 5 (collision / rigid bodies / character controller — #185) --------------
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
  PhysXCharacterKinematic_static # PxCreateControllerManager + PxCapsuleController (#185 PR 3)
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
    # a CCD sphere hitting a static box at ~15 m/s or more (a thrown basketball froze mid-air).
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

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/glloader/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_dxt_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
)

# Gameplay code lives in a separately-built DLL so it can be rebuilt and reloaded while the
# editor executable remains open. Keep module entry points and systems out of the host target.
list(REMOVE_ITEM ENGINE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/Game/GameModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/TransformControllerSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/SpinSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/ScoringSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleConsole.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleStats.cpp
)

set(IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)

# App icon (Windows only — .rc is an MSVC resource script). Baked into the exe itself so any
# shortcut targeting it keeps showing the icon across rebuilds; nothing to re-link externally.
if(WIN32)
  list(APPEND ENGINE_SOURCES ${CMAKE_SOURCE_DIR}/extern/branding/app_icon.rc)
  list(APPEND ENGINE_SOURCES ${CMAKE_BINARY_DIR}/generated/TartarusVersion.rc)
  # UTF-8 active code page + long-path awareness (#139 / #149) — see the file's comment.
  list(APPEND ENGINE_SOURCES ${CMAKE_SOURCE_DIR}/src/TartarusEngine.manifest)
endif()

add_executable(TartarusEngine ${ENGINE_SOURCES} ${IMGUI_SOURCES})

add_library(TartarusGame SHARED
  ${CMAKE_SOURCE_DIR}/src/Game/GameModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/TransformControllerSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/SpinSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/ScoringSystem.cpp
)

# Reloadable editor tools intentionally use a narrow host callback table rather than sharing
# EditorLayer's ImGui/undo/scene lifetime across a DLL boundary. That lets a tool panel reload
# live without invalidating an open scene or an active ImGui frame.
#
# The module draws real ImGui panels (the Console, as of the first migrated panel), so it compiles
# its own ImGui core translation units and is handed the host's ImGuiContext + allocator pair at
# Draw() time — ImGui's documented multi-module setup, where all shared state stays in that one
# host-owned context. Backends (glfw/opengl3) are deliberately NOT built here: the host alone
# drives platform/renderer. Core/Log.cpp is likewise NOT built here — a second copy would give the
# DLL its own private, permanently-empty log; it reads the host's through the callback table.
add_library(TartarusEditor SHARED
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleToolbar.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleAssetBrowser.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleHierarchy.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleInspector.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleConsole.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleStats.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleHistory.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(TartarusEditor PRIVATE
  ${CMAKE_SOURCE_DIR}/src/Editor
  ${CMAKE_SOURCE_DIR}/extern   # IconsFontAwesome6.h
  ${imgui_SOURCE_DIR}
)
# #202 / #346 - Release keeps its optimisations but now also emits a PDB (/Zi + /DEBUG).
# Without one, dbghelp has no symbols in the shipped configuration, which made two features
# silently do nothing there: the Console stack traces on warnings and errors, and the crash
# handler minidumps - a dump from a Release build could not be symbolicated, so the one artifact
# produced when the engine actually falls over was unreadable.
#
# /DEBUG defaults the linker to /OPT:NOREF, which would stop it folding unused code and bloat the
# binary, so the usual pair is restored explicitly. Code generation is unaffected: /Zi changes
# what is written alongside the object, not what is in it.
function(tartarus_enable_release_debug_info target)
  if(MSVC)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
  endif()
endfunction()

# Must match the host's ImGui configuration exactly — the two copies share one ImGuiContext.
target_compile_definitions(TartarusEditor PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_options(TartarusEditor PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
tartarus_enable_release_debug_info(TartarusEditor)

target_include_directories(TartarusGame PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/Core
  ${CMAKE_SOURCE_DIR}/src/Game
  ${CMAKE_SOURCE_DIR}/src/Physics
  ${CMAKE_SOURCE_DIR}/src/Renderer # MaterialAsset (header-only use: the Score Digit segments' emission)
)
target_link_libraries(TartarusGame PRIVATE glm::glm EnTT::EnTT)
target_compile_definitions(TartarusGame PRIVATE GLM_ENABLE_EXPERIMENTAL)
target_compile_options(TartarusGame PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
tartarus_enable_release_debug_info(TartarusGame)

# A normal TartarusEngine build also produces its current gameplay module next to the exe.
add_dependencies(TartarusEngine TartarusGame TartarusEditor)

# The actual DLL filenames the host watches for (rather than a hardcoded "TartarusGame.dll"):
# CMAKE_DEBUG_POSTFIX is set globally to "d" (originally for the FetchContent'd dependencies),
# which also applies to these two library targets in Debug builds — TartarusGame.dll becomes
# TartarusGamed.dll, TartarusEditor.dll becomes TartarusEditord.dll. A hardcoded name only ever
# matched in Release/RelWithDebInfo, where there's no postfix; hot reload silently never found
# either DLL in a Debug build. $<TARGET_FILE_NAME:...> always resolves to whatever the linker
# actually produced for the active config.
target_compile_definitions(TartarusEngine PRIVATE
  TARTARUS_GAME_MODULE_FILENAME="$<TARGET_FILE_NAME:TartarusGame>"
  TARTARUS_EDITOR_MODULE_FILENAME="$<TARGET_FILE_NAME:TartarusEditor>"
  # Shown in the viewport status bar so the running build (Debug vs an optimized config) is
  # obvious at a glance — Debug is ~2x the frame time.
  TARTARUS_BUILD_CONFIG="$<CONFIG>"
)

# A console-subsystem build makes Windows open a terminal window alongside the editor, which
# looks like a launch error to anyone who didn't expect it. Release builds use the GUI
# subsystem so only the editor appears; Debug keeps the console, where seeing stdout/stderr
# immediately is worth more than the cosmetics.
#
# /ENTRY:mainCRTStartup is required alongside it: the GUI subsystem makes MSVC look for
# WinMain, and the engine has an ordinary main(). Log output still reaches the editor's own
# Console panel either way, and shell redirection to a file keeps working in both.
if(WIN32)
  set_target_properties(TartarusEngine PROPERTIES
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
  target_link_options(TartarusEngine PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
endif()

target_include_directories(TartarusEngine PRIVATE
  ${CMAKE_BINARY_DIR}/generated   # TartarusVersion.h
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/Core
  ${CMAKE_SOURCE_DIR}/src/Renderer
  ${CMAKE_SOURCE_DIR}/src/Game
  ${CMAKE_SOURCE_DIR}/src/Physics
  ${CMAKE_SOURCE_DIR}/src/Audio
  ${CMAKE_SOURCE_DIR}/src/Editor
  ${CMAKE_SOURCE_DIR}/src/Assets
  ${CMAKE_SOURCE_DIR}/extern/glloader
  ${CMAKE_SOURCE_DIR}/extern
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imguizmo_SOURCE_DIR}/src
)

target_link_libraries(TartarusEngine PRIVATE glfw glm::glm assimp EnTT::EnTT physx)

if(WIN32)
  target_link_libraries(TartarusEngine PRIVATE opengl32 comdlg32 dwmapi winmm Dbghelp ole32 shell32 uuid)
endif()

# PhysX Visual Debugger hookup (#185) — Debug builds only. Lets PR 2-3's colliders / character
# capsule be inspected live in PVD; a no-op when no PVD instance is listening.
target_compile_definitions(TartarusEngine PRIVATE $<$<CONFIG:Debug>:TARTARUS_PHYSX_PVD>)

# Linking an INTERFACE lib does NOT inherit its add_dependencies(), so state the build-order
# dependency on the ExternalProject directly: the staged .libs must exist before the engine links.
add_dependencies(TartarusEngine physx_external)

target_compile_definitions(TartarusEngine PRIVATE GLM_ENABLE_EXPERIMENTAL GLFW_INCLUDE_NONE IMGUI_DEFINE_MATH_OPERATORS)

# Warning flags (#180) — the build previously set none, so it ran at MSVC's default /W1 and hid
# most latent problems (unused results, signed/unsigned mismatch, uninitialized reads, shadowed
# variables, truncating conversions). /MP is unrelated to warnings but cheap to add here too —
# parallel compilation noticeably cuts build times.
# #173 - warnings are errors (MSVC /WX) for all three engine targets: the tree builds clean at /W4.
target_compile_options(TartarusEngine PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
tartarus_enable_release_debug_info(TartarusEngine)

# Vendored sources compiled directly into this target (ImGui, ImGuizmo, ImViewGuizmo, stb_image,
# miniaudio) are third-party code we don't own and aren't the "fallout" #180 means to fix — dial
# them back to the default warning level rather than flooding the build with warnings in code we
# can't reasonably change.
set(TARTARUS_VENDORED_SOURCES
  ${IMGUI_SOURCES}
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_dxt_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
)
set_source_files_properties(${TARTARUS_VENDORED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/W1>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>"
)

# Editor UI icon font (Font Awesome Free, SIL OFL 1.1), the Inter UI-text fallback face (Inter,
# SIL OFL 1.1 — #48) and the JetBrains Mono monospace face (SIL OFL 1.1 — Phase 1 item 5, the
# Console body / numeric-readout face) — copied next to the exe so they can be loaded at runtime
# by a path relative to the working directory, same as scene.json.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/fonts/fa-solid-900.ttf"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts/fa-solid-900.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/fonts/InterVariable.ttf"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts/InterVariable.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/fonts/JetBrainsMono-Regular.ttf"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts/JetBrainsMono-Regular.ttf"
)

# Editor wordmark ("TARTARUS ENGINE" text, no icon) and mark (the "TE" monogram, no text) —
# the two halves of the full logo lockup, cropped separately — same copy-next-to-exe treatment
# as the icon font.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/branding/tartarus_wordmark.png"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding/tartarus_wordmark.png"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/branding/tartarus_engine_mark.png"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding/tartarus_engine_mark.png"
)

# GLSL shader files — copied next to the exe so ShaderLibrary::ReadFile("assets/shaders/...")
# resolves correctly at runtime.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/src/Renderer/shaders"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/shaders"
)

# Engine-owned smoke/regression scenes (audit TEST-101) — staged next to the exe as
# assets/test-scenes/ so `--smoke-test` has meaningful, version-controlled coverage that does
# not depend on the (deletable, user-editable) project/scenes/ content.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/test-scenes"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/tests/smoke-scenes"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/test-scenes"
)

# PhysX runtime DLLs (#185) — copied next to the exe like the fonts above. The set for the
# CPU-only, non-GPU build is just these three (PhysX.dll load-time-depends on PhysXCommon +
# PhysXFoundation; Cooking / Gpu aren't built). Pulled from the staged Debug/ or Release/
# folder to match the config being built.
foreach(_pxdll ${_physx_runtime_dlls})
  add_custom_command(TARGET TartarusEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PHYSX_STAGE}/${_physx_stage_for_config}/${_pxdll}.dll"
      "$<TARGET_FILE_DIR:TartarusEngine>/${_pxdll}.dll"
    VERBATIM
  )
endforeach()

# --- TifSplitter (optional standalone tool) ---------------------------------------------
# Converts high-bit-depth / multi-channel TIFF texture sheets and heightmaps into 8-bit PNGs
# the engine's own stb_image-based Texture loader can actually read. A separate executable,
# not part of TartarusEngine, and off by default: pulling in libtiff (its own dependency,
# with a real build-time cost) shouldn't happen just to build/run the game. Enable with
# -DBUILD_TIF_SPLITTER=ON.
option(BUILD_TIF_SPLITTER "Build the TifSplitter TIFF-to-PNG conversion tool" OFF)
if(BUILD_TIF_SPLITTER)
  # Baseline TIFF support only (uncompressed / PackBits / LZW — all built into libtiff itself,
  # no external library needed). Every optional codec that WOULD need one is force-disabled so
  # this dependency stays self-contained instead of silently picking up whatever happens to be
  # on the build machine. Practical effect: a TIFF saved with ZIP/Deflate compression (a common
  # Photoshop default) will fail to decode under this configuration — re-save it as LZW or
  # uncompressed if that's ever hit.
  set(zlib OFF CACHE BOOL "" FORCE)
  set(libdeflate OFF CACHE BOOL "" FORCE)
  set(pixarlog OFF CACHE BOOL "" FORCE)
  set(jpeg OFF CACHE BOOL "" FORCE)
  set(old-jpeg OFF CACHE BOOL "" FORCE)
  set(jbig OFF CACHE BOOL "" FORCE)
  set(lzma OFF CACHE BOOL "" FORCE)
  set(zstd OFF CACHE BOOL "" FORCE)
  set(webp OFF CACHE BOOL "" FORCE)
  set(tiff-tools OFF CACHE BOOL "" FORCE)
  set(tiff-tests OFF CACHE BOOL "" FORCE)
  set(tiff-contrib OFF CACHE BOOL "" FORCE)
  set(tiff-docs OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    tiff
    GIT_REPOSITORY https://gitlab.com/libtiff/libtiff.git
    GIT_TAG v4.6.0
  )
  FetchContent_MakeAvailable(tiff)

  set(TIF_SPLITTER_SOURCES
    ${CMAKE_SOURCE_DIR}/tools/TifSplitter/main.cpp
    ${CMAKE_SOURCE_DIR}/tools/TifSplitter/TifSplitter.cpp
    ${CMAKE_SOURCE_DIR}/extern/stb_image_write_impl.cpp
  )
  if(WIN32)
    list(APPEND TIF_SPLITTER_SOURCES ${CMAKE_SOURCE_DIR}/extern/branding/tifsplitter_icon.rc)
    # Same UTF-8 active code page / long-path manifest as the engine (#185 / #149): non-ASCII
    # input and output paths work through argv, libtiff and stb's fopen alike.
    list(APPEND TIF_SPLITTER_SOURCES ${CMAKE_SOURCE_DIR}/src/TartarusEngine.manifest)
  endif()
  add_executable(TifSplitter ${TIF_SPLITTER_SOURCES})
  target_include_directories(TifSplitter PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/TifSplitter
    ${CMAKE_SOURCE_DIR}/extern
  )
  target_link_libraries(TifSplitter PRIVATE TIFF::tiff)
  target_compile_features(TifSplitter PRIVATE cxx_std_20)
endif()

# --- Install + package (#173 suggestion 4) -----------------------------------------------
# Produces a self-contained ZIP: unzip anywhere, run TartarusEngine.exe. The layout matches
# what the POST_BUILD steps above stage next to the exe, because that is the layout the engine
# resolves against at runtime (EnginePaths walks out from the executable, ProjectPaths looks for
# a "project" folder beside it).
#
#   cmake --build build --config Release --target package
#   cpack --config build/CPackConfig.cmake -C Release      # equivalent
#
# Deliberately NOT "install the whole build/<config> directory". That directory picks things up
# that must not ship: import libraries, imgui.ini, crash dumps, and - on CI - Mesa's software
# opengl32.dll, which would silently force every user onto llvmpipe. Everything below is
# enumerated instead, and the CI package check diffs the staged assets tree against the packaged
# one so a new POST_BUILD asset directory cannot be forgotten here without failing the build.

# Executable and the two hot-reloadable modules. $<TARGET_FILE:...> is the .dll for a SHARED
# library on Windows, which skips the .lib/.exp import pair an end user has no use for.
install(TARGETS TartarusEngine RUNTIME DESTINATION ".")
install(FILES
  $<TARGET_FILE:TartarusGame>
  $<TARGET_FILE:TartarusEditor>
  DESTINATION "."
)

# PhysX runtime, from the same staged per-config folder the POST_BUILD copy uses.
foreach(_pxdll ${_physx_runtime_dlls})
  install(FILES "${PHYSX_STAGE}/${_physx_stage_for_config}/${_pxdll}.dll" DESTINATION ".")
endforeach()

# Engine-owned content, from the source tree rather than the build tree so `cpack` does not
# depend on which config was staged last.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/Renderer/shaders/"    DESTINATION "assets/shaders")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/tests/smoke-scenes/"      DESTINATION "assets/test-scenes")
install(FILES
  "${CMAKE_SOURCE_DIR}/extern/fonts/fa-solid-900.ttf"
  "${CMAKE_SOURCE_DIR}/extern/fonts/InterVariable.ttf"
  "${CMAKE_SOURCE_DIR}/extern/fonts/JetBrainsMono-Regular.ttf"
  DESTINATION "assets/fonts"
)
install(FILES
  "${CMAKE_SOURCE_DIR}/extern/branding/tartarus_wordmark.png"
  "${CMAKE_SOURCE_DIR}/extern/branding/tartarus_engine_mark.png"
  DESTINATION "assets/branding"
)

# The sample project, as an ALLOW-LIST of directories rather than "project/ minus exclusions".
#
# That choice is load-bearing. project/assets/characters/ (Mixamo) and project/assets/sky/
# (Poly Haven HDRIs) are gitignored and absent on CI, but they DO exist in a developer's working
# tree - and Mixamo's licence does not permit redistributing those FBX files. An exclude-list
# that someone later edits wrongly ships them; an allow-list that someone later forgets to extend
# merely omits a folder from the sample. Wrong in the safe direction.
#
# Library/ and screenshots/ are per-user output, not content. .shutter.wav is the exception -
# the Screenshots folder's own sound, which is tracked and has to come along.
foreach(_projdir scenes materials models textures sounds animations scripts)
  if(EXISTS "${CMAKE_SOURCE_DIR}/project/${_projdir}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/project/${_projdir}/"
            DESTINATION "project/${_projdir}"
            PATTERN "*.recovery.json" EXCLUDE
            PATTERN "*.json.bak"      EXCLUDE)
  endif()
endforeach()
install(FILES
  "${CMAKE_SOURCE_DIR}/project/screenshots/.shutter.wav"
  "${CMAKE_SOURCE_DIR}/project/screenshots/.shutter.wav.meta"
  DESTINATION "project/screenshots"
)

# Licences and the first-run notes (which explain the two asset sets the ZIP cannot carry).
configure_file(
  "${CMAKE_SOURCE_DIR}/packaging/README-FIRST-RUN.txt.in"
  "${CMAKE_BINARY_DIR}/generated/README-FIRST-RUN.txt"
  @ONLY
)
install(FILES
  "${CMAKE_SOURCE_DIR}/LICENSE"
  "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
  "${CMAKE_BINARY_DIR}/generated/README-FIRST-RUN.txt"
  DESTINATION "."
)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "TartarusEngine")
set(CPACK_PACKAGE_VENDOR "Jacob Camberos")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tartarus Engine - a hand-rolled C++17 / OpenGL 4.6 game engine and editor")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
# Unzips to a single TartarusEngine-<version>-win64/ folder rather than spraying the contents
# into whatever directory the user extracted into.
set(CPACK_PACKAGE_FILE_NAME "TartarusEngine-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "TartarusEngine-${PROJECT_VERSION}-win64")
# Left OFF deliberately. Scoping the package to a "runtime" component does keep a dependency's
# own install rules out, but archive generators drop the top-level wrapper folder when component
# install is on - the ZIP then sprays its contents over whatever directory someone extracted
# into, and CPACK_INCLUDE_TOPLEVEL_DIRECTORY does not put it back. Dependencies are kept from
# installing at the source instead (see EnTT above), and CI asserts the package's top-level
# entries are exactly the expected set, so a new dependency that leaks fails the build.
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
# CPack otherwise sweeps in the FetchContent source trees (assimp's test data alone is larger
# than everything the engine actually ships).
set(CPACK_SOURCE_IGNORE_FILES "/\\.git/;/build/;/_deps/")
include(CPack)
