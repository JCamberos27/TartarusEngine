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

include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/PhysX.cmake)

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/glloader/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_dxt_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImGuiColorTextEdit/TextEditor.cpp
)

# Gameplay code lives in a separately-built DLL so it can be rebuilt and reloaded while the
# editor executable remains open. Keep module entry points and systems out of the host target.
# The reloadable modules' sources. Module-only files leave the engine; the first-person rules are
# shared: the engine keeps its own copy (used until a module with matching frames loads, and by
# the unit tests) and TartarusGame compiles another, so editing them hot-reloads.
set(TARTARUS_GAME_MODULE_ONLY_SOURCES
  ${CMAKE_SOURCE_DIR}/src/Game/GameModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/TransformControllerSystem.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/LightFlickerSystem.cpp
)
set(TARTARUS_GAME_SHARED_SOURCES
  ${CMAKE_SOURCE_DIR}/src/Game/WeaponRules.cpp
  ${CMAKE_SOURCE_DIR}/src/Game/BodyLocomotion.cpp
)
set(TARTARUS_EDITOR_MODULE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModule.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleToolbar.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleAssetBrowser.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleHierarchy.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleInspector.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleConsole.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleStats.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleHistory.cpp
  ${CMAKE_SOURCE_DIR}/src/Editor/EditorModuleTestHooks.cpp
)
list(REMOVE_ITEM ENGINE_SOURCES ${TARTARUS_GAME_MODULE_ONLY_SOURCES} ${TARTARUS_EDITOR_MODULE_SOURCES})

# The Script IDE asks for a module rebuild when it saves one of these (GameModuleBuild.cpp reads
# this list), so the list lives here once instead of being copied into the editor.
set(_tartarus_module_manifest "")
foreach(_src IN LISTS TARTARUS_GAME_MODULE_ONLY_SOURCES TARTARUS_GAME_SHARED_SOURCES TARTARUS_EDITOR_MODULE_SOURCES)
  file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR} ${_src})
  string(APPEND _tartarus_module_manifest "${_rel}\n")
endforeach()
file(WRITE ${CMAKE_BINARY_DIR}/TartarusModules.sources.tmp "${_tartarus_module_manifest}")
configure_file(${CMAKE_BINARY_DIR}/TartarusModules.sources.tmp ${CMAKE_BINARY_DIR}/TartarusModules.sources COPYONLY)

include(${CMAKE_SOURCE_DIR}/cmake/ImGuiPatches.cmake)

# #202 / #346 - Release keeps its optimisations but now also emits a PDB (/Zi + /DEBUG).
# Without one, dbghelp has no symbols in the shipped configuration, which made two features
# silently do nothing there: the Console stack traces on warnings and errors, and the crash
# handler minidumps - a dump from a Release build could not be symbolicated, so the one artifact
# produced when the engine actually falls over was unreadable.
#
# /DEBUG defaults the linker to /OPT:NOREF, which would stop it folding unused code and bloat the
# binary, so the usual pair is restored explicitly. Code generation is unaffected: /Zi changes
# what is written alongside the object, not what is in it. (/Z7 instead was measured in 2026-11:
# a full engine recompile went 158 s -> 154 s, inside run-to-run noise, so it wasn't adopted.)
function(tartarus_enable_release_debug_info target)
  if(MSVC)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/Zi>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/DEBUG /OPT:REF /OPT:ICF>)
  endif()
endfunction()

# ImGui's core, compiled once and linked into both the host and TartarusEditor.dll. Each module
# still gets its own copy of the code, as ImGui's multi-module setup needs (they share the host's
# ImGuiContext), but the two copies can no longer drift: one set of objects, one set of defines.
# IMGUI_ENABLE_TEST_ENGINE: the --editor-tests item hooks; the DLL forwards them to the host
# through EditorModuleTestHooks.cpp (see EditorTestHooks.h).
add_library(tartarus_imgui_core OBJECT
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(tartarus_imgui_core PRIVATE ${imgui_SOURCE_DIR})
target_compile_definitions(tartarus_imgui_core PRIVATE IMGUI_DEFINE_MATH_OPERATORS IMGUI_ENABLE_TEST_ENGINE)
target_compile_options(tartarus_imgui_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W1 /MP>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
)
tartarus_enable_release_debug_info(tartarus_imgui_core)

set(IMGUI_SOURCES
  ${IMGUI_GLFW_BACKEND}
  ${IMGUI_GL3_BACKEND}
  ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
  ${imanim_SOURCE_DIR}/im_anim.cpp
)
set(MESHOPT_SOURCES
  ${meshoptimizer_SOURCE_DIR}/src/allocator.cpp
  ${meshoptimizer_SOURCE_DIR}/src/vcacheoptimizer.cpp
  ${meshoptimizer_SOURCE_DIR}/src/simplifier.cpp
  ${meshoptimizer_SOURCE_DIR}/src/vfetchoptimizer.cpp
  ${meshoptimizer_SOURCE_DIR}/src/indexgenerator.cpp
)
file(GLOB RECAST_SOURCES
  ${recastnavigation_SOURCE_DIR}/Recast/Source/*.cpp
  ${recastnavigation_SOURCE_DIR}/Detour/Source/*.cpp
  ${recastnavigation_SOURCE_DIR}/DetourCrowd/Source/*.cpp
)

# Version info (Windows only — .rc is an MSVC resource script). The exe carries no icon of its own.
if(WIN32)
  list(APPEND ENGINE_SOURCES ${CMAKE_BINARY_DIR}/generated/TartarusVersion.rc)
  # UTF-8 active code page + long-path awareness (#139 / #149) — see the file's comment.
  list(APPEND ENGINE_SOURCES ${CMAKE_SOURCE_DIR}/src/TartarusEngine.manifest)
endif()

add_executable(TartarusEngine ${ENGINE_SOURCES} ${IMGUI_SOURCES} ${MESHOPT_SOURCES} ${RECAST_SOURCES}
  $<TARGET_OBJECTS:tartarus_imgui_core>)

add_library(TartarusGame SHARED ${TARTARUS_GAME_MODULE_ONLY_SOURCES} ${TARTARUS_GAME_SHARED_SOURCES})

# Reloadable editor tools intentionally use a narrow host callback table rather than sharing
# EditorLayer's ImGui/undo/scene lifetime across a DLL boundary. That lets a tool panel reload
# live without invalidating an open scene or an active ImGui frame.
#
# The module draws real ImGui panels (the Console, as of the first migrated panel), so it links
# its own copy of ImGui's core (tartarus_imgui_core) and is handed the host's ImGuiContext + allocator pair at
# Draw() time — ImGui's documented multi-module setup, where all shared state stays in that one
# host-owned context. Backends (glfw/opengl3) are deliberately NOT built here: the host alone
# drives platform/renderer. Core/Log.cpp is likewise NOT built here — a second copy would give the
# DLL its own private, permanently-empty log; it reads the host's through the callback table.
add_library(TartarusEditor SHARED
  ${TARTARUS_EDITOR_MODULE_SOURCES}
  $<TARGET_OBJECTS:tartarus_imgui_core>
)
target_include_directories(TartarusEditor PRIVATE
  ${CMAKE_SOURCE_DIR}/src/Editor
  ${CMAKE_SOURCE_DIR}/extern   # IconsFontAwesome6.h
  ${imgui_SOURCE_DIR}
)
# The module's own sources see ImGui configured as tartarus_imgui_core is.
target_compile_definitions(TartarusEditor PRIVATE IMGUI_DEFINE_MATH_OPERATORS IMGUI_ENABLE_TEST_ENGINE)
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
# No CMAKE_DEBUG_POSTFIX is set in this project, so today the names are the same in every
# config. If a postfix is ever added (globally or per target), it would apply to these two
# library targets in Debug builds and a hardcoded name would silently stop matching.
# $<TARGET_FILE_NAME:...> always resolves to whatever the linker actually produced for the
# active config.
target_compile_definitions(TartarusEngine PRIVATE
  TARTARUS_GAME_MODULE_FILENAME="$<TARGET_FILE_NAME:TartarusGame>"
  TARTARUS_EDITOR_MODULE_FILENAME="$<TARGET_FILE_NAME:TartarusEditor>"
  # The Script IDE rebuilds the reloadable modules in place (GameModuleBuild.cpp). A packaged
  # player has no source tree here and simply reports the rebuild as unavailable.
  TARTARUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  TARTARUS_BINARY_DIR="${CMAKE_BINARY_DIR}"
  TARTARUS_CMAKE_COMMAND="${CMAKE_COMMAND}"
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
  ${CMAKE_SOURCE_DIR}/src/Graffiti # the Graffiti Designer's model, paint program and compositor
  ${CMAKE_SOURCE_DIR}/extern/glloader
  ${CMAKE_SOURCE_DIR}/extern
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imguizmo_SOURCE_DIR}/src
  ${imanim_SOURCE_DIR}
  ${meshoptimizer_SOURCE_DIR}/src
  ${recastnavigation_SOURCE_DIR}/Recast/Include
  ${recastnavigation_SOURCE_DIR}/Detour/Include
  ${recastnavigation_SOURCE_DIR}/DetourCrowd/Include
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
# --editor-tests: ImGui's test-engine item hooks (id, rect, label per widget) in the host executable
# only. They cost one bool test per item while no test runs (ImGuiContext::TestEngineHookItems).
target_compile_definitions(TartarusEngine PRIVATE IMGUI_ENABLE_TEST_ENGINE)

# Warning flags (#180) — the build previously set none, so it ran at MSVC's default /W1 and hid
# most latent problems (unused results, signed/unsigned mismatch, uninitialized reads, shadowed
# variables, truncating conversions). /MP is unrelated to warnings but cheap to add here too —
# parallel compilation noticeably cuts build times.
# #173 - warnings are errors (MSVC /WX) for all three engine targets: the tree builds clean at /W4.
# /bigobj: main.cpp's unoptimized Debug object outgrows the default COFF section limit (C1128).
target_compile_options(TartarusEngine PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP /bigobj>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
tartarus_enable_release_debug_info(TartarusEngine)

# Vendored sources compiled directly into this target (ImGui, ImGuizmo, ImViewGuizmo, stb_image,
# miniaudio) are third-party code we don't own and aren't the "fallout" #180 means to fix — dial
# them back to the default warning level rather than flooding the build with warnings in code we
# can't reasonably change.
set(TARTARUS_VENDORED_SOURCES
  ${IMGUI_SOURCES}
  ${MESHOPT_SOURCES}
  ${RECAST_SOURCES}
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_dxt_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
)
set_source_files_properties(${TARTARUS_VENDORED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/W1>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>"
)

# Engine-owned content staged next to the exe (POST_BUILD) and shipped in the package (install),
# from ONE list so the two can't drift. The layout is what EnginePaths resolves against at runtime.
# Each entry: "<source, relative to the repo>|<destination folder>[|<new file name>]". A source
# folder copies its contents.
#   - fonts: Font Awesome Free (icons), Inter (UI text, #48), JetBrains Mono (Console, numbers); SIL OFL 1.1
#   - editor_sounds: EditorSound's UI sounds (CC BY-SA adaptations, see resources/EditorSounds/LICENSE.md)
#   - shaders: read by ShaderLibrary from assets/shaders
#   - licenses: the vendored code editor's MIT attribution
#   - test-scenes: engine-owned smoke scenes for --smoke-test (audit TEST-101), independent of project/
set(TARTARUS_STAGED_ASSETS
  "extern/fonts/fa-solid-900.ttf|assets/fonts"
  "extern/fonts/InterVariable.ttf|assets/fonts"
  "extern/fonts/JetBrainsMono-Regular.ttf|assets/fonts"
  "resources/EditorSounds|assets/editor_sounds"
  "src/Renderer/shaders|assets/shaders"
  "extern/ImGuiColorTextEdit/LICENSE|assets/licenses|ImGuiColorTextEdit.txt"
  "tests/smoke-scenes|assets/test-scenes"
)
set(_stage_commands "")
foreach(_entry IN LISTS TARTARUS_STAGED_ASSETS)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _src)
  list(GET _parts 1 _dst)
  set(_dst_dir "$<TARGET_FILE_DIR:TartarusEngine>/${_dst}")
  if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_src}")
    list(APPEND _stage_commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/${_src}" "${_dst_dir}")
  else()
    get_filename_component(_name "${_src}" NAME)
    list(LENGTH _parts _n)
    if(_n GREATER 2)
      list(GET _parts 2 _name)
    endif()
    list(APPEND _stage_commands
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/${_src}" "${_dst_dir}/${_name}")
  endif()
endforeach()
add_custom_command(TARGET TartarusEngine POST_BUILD ${_stage_commands} VERBATIM)

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

# Engine-owned content (TARTARUS_STAGED_ASSETS above), from the source tree rather than the
# build tree so `cpack` does not depend on which config was staged last.
foreach(_entry IN LISTS TARTARUS_STAGED_ASSETS)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _src)
  list(GET _parts 1 _dst)
  list(LENGTH _parts _n)
  if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_src}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/${_src}/" DESTINATION "${_dst}")
  elseif(_n GREATER 2)
    list(GET _parts 2 _name)
    install(FILES "${CMAKE_SOURCE_DIR}/${_src}" DESTINATION "${_dst}" RENAME "${_name}")
  else()
    install(FILES "${CMAKE_SOURCE_DIR}/${_src}" DESTINATION "${_dst}")
  endif()
endforeach()

# The sample project, as an ALLOW-LIST of directories rather than "project/ minus exclusions".
#
# That choice is load-bearing. project/assets/sky/ (Poly Haven HDRIs) is gitignored and absent
# on CI, but it DOES exist in a developer's working tree; and the third-party packs under
# assets/Characters/, assets/Weapons/ and assets/Animations/Mocap/ were never part of the sample
# (Mixamo-style licences don't allow redistributing such files). An exclude-list that someone
# later edits wrongly ships them; an allow-list
# that someone later forgets to extend merely omits a folder from the sample. Wrong in the safe
# direction.
#
# Library/ and screenshots/ are per-user output, not content. .shutter.wav is the exception -
# the Screenshots folder's own sound, which is tracked and has to come along.
#
# The allowed folders also hold third-party files a developer fetched from the shared Drive
# (project/external_assets.csv lists them): the recorded sounds under assets/Audio/<category>/,
# the Apartment's baked textures and the prop packs' meshes and textures under assets/Props/.
# Absent on CI, present locally - so they are excluded here too. What remains is what git
# tracks (the Apartment meshes, the prop materials and .meta sidecars, ...).
foreach(_projdir scenes assets/Audio assets/Environments assets/Props assets/Materials assets/Test
                 assets/Animations/Controllers)
  if(EXISTS "${CMAKE_SOURCE_DIR}/project/${_projdir}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/project/${_projdir}/"
            DESTINATION "project/${_projdir}"
            PATTERN "*.recovery.json" EXCLUDE
            PATTERN "*.json.bak"      EXCLUDE
            PATTERN "*.log"           EXCLUDE
            REGEX "/assets/Audio/[^/]+/.*\\.wav$" EXCLUDE
            REGEX "/assets/Environments/Apartment/Textures/.*\\.png$" EXCLUDE
            REGEX "/assets/Props/.*\\.(png|fbx)$" EXCLUDE)
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
