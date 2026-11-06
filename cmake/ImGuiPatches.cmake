# Local patches to two Dear ImGui backends, applied to copies in the build tree. Included from
# the root CMakeLists.txt after Dependencies.cmake; sets IMGUI_GLFW_BACKEND and IMGUI_GL3_BACKEND.

# The GLFW backend sets every viewport's GLFW_MOUSE_PASSTHROUGH each frame, and GLFW applies it
# unconditionally: a SetWindowLongW + SetLayeredWindowAttributes round trip per window per frame
# (~0.3 ms in the Sandbox). Compile a copy that only sets it when it changes. A copy in the build
# tree, so the fetched source (or a FETCHCONTENT_SOURCE_DIR_IMGUI override) is never modified;
# if the line isn't there (a newer imgui), the original compiles unchanged.
set(IMGUI_GLFW_BACKEND ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
file(READ ${IMGUI_GLFW_BACKEND} _imgui_glfw_src)
set(_imgui_passthrough_line "glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH, window_no_input);")
string(FIND "${_imgui_glfw_src}" "${_imgui_passthrough_line}" _imgui_passthrough_at)
if(_imgui_passthrough_at GREATER -1)
  string(REPLACE "${_imgui_passthrough_line}"
    "if (glfwGetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH) != (window_no_input ? 1 : 0)) ${_imgui_passthrough_line}"
    _imgui_glfw_src "${_imgui_glfw_src}")
  # Two more per-frame Win32 round trips, each only where the line is found:
  # - GLFW_HOVERED is a WindowFromPoint per viewport per frame (~3% of the play frame). While the
  #   game has captured the cursor (GLFW_CURSOR_DISABLED) no viewport is hovered in any useful sense.
  # - The monitor list (EnumDisplaySettings per monitor) is rebuilt every frame: every 0.5 s is
  #   plenty to follow a display change.
  string(REPLACE "if (glfwGetWindowAttrib(window, GLFW_HOVERED))"
    "if (glfwGetInputMode(window, GLFW_CURSOR) != GLFW_CURSOR_DISABLED && glfwGetWindowAttrib(window, GLFW_HOVERED))"
    _imgui_glfw_src "${_imgui_glfw_src}")
  string(REPLACE "&io.DisplayFramebufferScale);\n    ImGui_ImplGlfw_UpdateMonitors();"
    "&io.DisplayFramebufferScale);\n    { static double s_NextMonitors = 0.0; const double now = glfwGetTime(); if (now >= s_NextMonitors) { s_NextMonitors = now + 0.5; ImGui_ImplGlfw_UpdateMonitors(); } }"
    _imgui_glfw_src "${_imgui_glfw_src}")
  set(IMGUI_GLFW_BACKEND ${CMAKE_BINARY_DIR}/imgui_patched/imgui_impl_glfw.cpp)
  set(_imgui_glfw_old "")
  if(EXISTS ${IMGUI_GLFW_BACKEND})
    file(READ ${IMGUI_GLFW_BACKEND} _imgui_glfw_old)
  endif()
  if(NOT _imgui_glfw_old STREQUAL _imgui_glfw_src) # rewrite only on change: no rebuild per configure
    file(WRITE ${IMGUI_GLFW_BACKEND} "${_imgui_glfw_src}")
  endif()
else()
  message(WARNING "imgui_impl_glfw.cpp: mouse-passthrough line not found; building it unpatched")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)

# The OpenGL backend backs up ~20 GL states with glGetIntegerv / glIsEnabled around every render;
# on a threaded driver each read waits for the driver's worker to drain the frame (~9% of the
# Sandbox frame). Compile a copy whose reads go to src/Editor/ImGuiGLHooks.cpp, which answers from
# the loader's state shadow or with the canonical values the engine doesn't depend on. Same
# build-tree-copy rules as the GLFW patch above.
set(IMGUI_GL3_BACKEND ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
file(READ ${IMGUI_GL3_BACKEND} _imgui_gl3_src)
set(_imgui_gl3_anchor "#include \"imgui_impl_opengl3.h\"")
string(FIND "${_imgui_gl3_src}" "${_imgui_gl3_anchor}" _imgui_gl3_at)
if(_imgui_gl3_at GREATER -1)
  string(REPLACE "glGetIntegerv(" "TartarusImGui_GetIntegerv(" _imgui_gl3_src "${_imgui_gl3_src}")
  string(REPLACE "glIsEnabled(" "TartarusImGui_IsEnabled(" _imgui_gl3_src "${_imgui_gl3_src}")
  # It also creates and deletes a VAO every render, and creating one returns a name, so it is a
  # driver round trip too. The engine has one GL context (no ImGui platform windows): keep one.
  string(REPLACE "GL_CALL(glGenVertexArrays(1, &vertex_array_object));"
    "{ static GLuint s_Vao = 0; if (!s_Vao) GL_CALL(glGenVertexArrays(1, &s_Vao)); vertex_array_object = s_Vao; }"
    _imgui_gl3_src "${_imgui_gl3_src}")
  string(REPLACE "GL_CALL(glDeleteVertexArrays(1, &vertex_array_object));" "" _imgui_gl3_src "${_imgui_gl3_src}")
  string(REPLACE "${_imgui_gl3_anchor}"
    "${_imgui_gl3_anchor}\nextern \"C\" void TartarusImGui_GetIntegerv(unsigned int pname, int* data);\nextern \"C\" unsigned char TartarusImGui_IsEnabled(unsigned int cap);"
    _imgui_gl3_src "${_imgui_gl3_src}")
  set(IMGUI_GL3_BACKEND ${CMAKE_BINARY_DIR}/imgui_patched/imgui_impl_opengl3.cpp)
  set(_imgui_gl3_old "")
  if(EXISTS ${IMGUI_GL3_BACKEND})
    file(READ ${IMGUI_GL3_BACKEND} _imgui_gl3_old)
  endif()
  if(NOT _imgui_gl3_old STREQUAL _imgui_gl3_src)
    file(WRITE ${IMGUI_GL3_BACKEND} "${_imgui_gl3_src}")
  endif()
else()
  message(FATAL_ERROR "imgui_impl_opengl3.cpp: include line not found; ImGuiGLHooks.cpp needs the patched backend")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
