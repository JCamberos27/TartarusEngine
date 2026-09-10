cmake_minimum_required(VERSION 3.16)
project(TartarusEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# --- EnTT (entity-component system) — header-only, ships its own CMake target ---
FetchContent_Declare(
  entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.13.2
)
FetchContent_MakeAvailable(entt)

# --- Dear ImGui (editor UI) — no CMakeLists upstream, so populate + build manually ---
# On the "docking" branch (not a numbered release) specifically for real dock-node based
# panel layout: resizing one docked panel resizes its neighbors, independent of whether
# panels are locked from being moved/rearranged.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# --- ImGuizmo (editor viewport translate/rotate/scale gizmos) ---
# Its own CMakeLists.txt builds a standalone library we don't want (it doesn't know
# about our ImGui include path), so populate the source only and compile it ourselves.
FetchContent_Declare(
  imguizmo
  GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
  GIT_TAG master
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
    -DPX_FLOAT_POINT_PRECISE_MATH:BOOL=OFF
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
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
)

# Gameplay code lives in a separately-built DLL so it can be rebuilt and reloaded while the
# editor executable remains open. Keep module entry points and systems out of the host target.
list(REMOVE_ITEM ENGINE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/Game/GameModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/TransformControllerSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/SpinSystem.cpp
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
endif()

add_executable(TartarusEngine ${ENGINE_SOURCES} ${IMGUI_SOURCES})

add_library(TartarusGame SHARED
  ${CMAKE_SOURCE_DIR}/src/Game/GameModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/TransformControllerSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/SpinSystem.cpp
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
# Must match the host's ImGui configuration exactly — the two copies share one ImGuiContext.
target_compile_definitions(TartarusEditor PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_compile_options(TartarusEditor PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

target_include_directories(TartarusGame PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/Core
  ${CMAKE_SOURCE_DIR}/src/Game
  ${CMAKE_SOURCE_DIR}/src/Physics
)
target_link_libraries(TartarusGame PRIVATE glm::glm EnTT::EnTT)
target_compile_definitions(TartarusGame PRIVATE GLM_ENABLE_EXPERIMENTAL)
target_compile_options(TartarusGame PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

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
  target_link_libraries(TartarusEngine PRIVATE opengl32 comdlg32 dwmapi winmm)
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
target_compile_options(TartarusEngine PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# Vendored sources compiled directly into this target (ImGui, ImGuizmo, ImViewGuizmo, stb_image,
# miniaudio) are third-party code we don't own and aren't the "fallout" #180 means to fix — dial
# them back to the default warning level rather than flooding the build with warnings in code we
# can't reasonably change.
set(TARTARUS_VENDORED_SOURCES
  ${IMGUI_SOURCES}
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
)
set_source_files_properties(${TARTARUS_VENDORED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/W1>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>"
)

# Editor UI icon font (Font Awesome Free, SIL OFL 1.1) — copied next to the exe so it can be
# loaded at runtime by a path relative to the working directory, same as scene.json.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/fonts/fa-solid-900.ttf"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/fonts/fa-solid-900.ttf"
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
  # Startup splash. Pre-scaled to its display size on purpose: the 4K source would cost more
  # to decode than the splash is meant to hide.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/branding/splash.png"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding/splash.png"
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
  endif()
  add_executable(TifSplitter ${TIF_SPLITTER_SOURCES})
  target_include_directories(TifSplitter PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/TifSplitter
    ${CMAKE_SOURCE_DIR}/extern
  )
  target_link_libraries(TifSplitter PRIVATE TIFF::tiff)
  target_compile_features(TifSplitter PRIVATE cxx_std_20)
endif()
