# Third-party dependencies fetched at configure time (FetchContent), plus the local assimp
# patches. Kept apart from the root CMakeLists.txt so CI's FetchContent cache is keyed on this
# file alone: editing the engine's own build rules no longer refetches every dependency.

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
  SOURCE_SUBDIR tartarus-no-cmake # MakeAvailable fetches only; EnTT is added below, EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(entt)
add_subdirectory(${entt_SOURCE_DIR} ${entt_BINARY_DIR} EXCLUDE_FROM_ALL)

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
  SOURCE_SUBDIR tartarus-no-cmake # source only: MakeAvailable fetches it without add_subdirectory
)
FetchContent_MakeAvailable(imguizmo)

# --- ImAnim (tweens, springs and easing for Dear ImGui; the opt-in Glass editor theme) ---
# Source only: one .cpp/.h pair compiled into the host (src/Editor/Glass/GlassMotion.cpp is its
# only user). Its own CMakeLists builds a demo against every ImPlatform backend, and its
# submodules (the demo's ImGui, ImPlatform, Sharpmake) are not needed, so they're not fetched.
# Pinned to a commit on "main" (no tagged releases); upgrade alongside ImGui.
FetchContent_Declare(
  imanim
  GIT_REPOSITORY https://github.com/soufianekhiat/ImAnim.git
  GIT_TAG 1ea19a55a28d54acb7f7f22a617f969aec2a073e # main
  GIT_SUBMODULES ""
  SOURCE_SUBDIR tartarus-no-cmake # source only: MakeAvailable fetches it without add_subdirectory
)
FetchContent_MakeAvailable(imanim)

# --- meshoptimizer (triangle order for the GPU's post-transform vertex cache) ---
# Source only: the engine compiles the files it uses (vertex cache optimizer, simplifier, vertex fetch, index generator, allocator)
# rather than building the library target, whose install rules would land in the package.
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v1.3
  SOURCE_SUBDIR tartarus-no-cmake # source only: MakeAvailable fetches it without add_subdirectory
)
FetchContent_MakeAvailable(meshoptimizer)

# --- Recast & Detour (navigation meshes, pathfinding and crowd steering for the enemy AI) ---
# Source only, like meshoptimizer: the engine compiles Recast (navmesh building), Detour (queries)
# and DetourCrowd (local avoidance) itself rather than building the library targets, whose install
# rules would land in the package.
FetchContent_Declare(
  recastnavigation
  GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
  GIT_TAG v1.6.0
  SOURCE_SUBDIR tartarus-no-cmake # source only: MakeAvailable fetches it without add_subdirectory
)
FetchContent_MakeAvailable(recastnavigation)
