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

add_executable(TartarusEngine ${ENGINE_SOURCES} ${IMGUI_SOURCES})

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

target_link_libraries(TartarusEngine PRIVATE glfw glm::glm assimp)

if(WIN32)
  target_link_libraries(TartarusEngine PRIVATE opengl32 comdlg32 dwmapi)
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

# Studio branding (watermark shown in the editor) — same copy-next-to-exe treatment as the icon font.
add_custom_command(TARGET TartarusEngine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/extern/branding/atrocity_exhibition_logo.png"
    "$<TARGET_FILE_DIR:TartarusEngine>/assets/branding/atrocity_exhibition_logo.png"
)
