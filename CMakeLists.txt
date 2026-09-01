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

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/glloader/*.cpp
  ${CMAKE_SOURCE_DIR}/extern/stb_image_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/miniaudio_impl.cpp
  ${CMAKE_SOURCE_DIR}/extern/ImViewGuizmo_impl.cpp
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
  ${CMAKE_SOURCE_DIR}/extern/glloader
  ${CMAKE_SOURCE_DIR}/extern
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imguizmo_SOURCE_DIR}/src
)

target_link_libraries(TartarusEngine PRIVATE glfw glm::glm assimp EnTT::EnTT)

if(WIN32)
  target_link_libraries(TartarusEngine PRIVATE opengl32 comdlg32 dwmapi winmm)
endif()

target_compile_definitions(TartarusEngine PRIVATE GLM_ENABLE_EXPERIMENTAL GLFW_INCLUDE_NONE IMGUI_DEFINE_MATH_OPERATORS)

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
