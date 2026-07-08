cmake_minimum_required(VERSION 3.16)
project(s411_pi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wxWidgets must match the build OpenCPN itself uses (3.0/3.2).
# 'aui' is required by ocpn_plugin.h, 'xml' by the GML parser.
find_package(wxWidgets 3.0 REQUIRED COMPONENTS core base xml gl aui)
include(${wxWidgets_USE_FILE})
find_package(OpenGL REQUIRED)

add_library(s411_pi MODULE
  src/s411_pi.cpp
  src/S411Data.cpp
  src/S411Tess.cpp
  src/S411Router.cpp
  src/S411Boundary.cpp)

target_include_directories(s411_pi PRIVATE api src)
target_link_libraries(s411_pi PRIVATE ${wxWidgets_LIBRARIES} OpenGL::GL)
if(TARGET OpenGL::GLU)
  target_link_libraries(s411_pi PRIVATE OpenGL::GLU)
else()
  target_link_libraries(s411_pi PRIVATE ${OPENGL_glu_LIBRARY})
endif()

if(WIN32)
  # On Windows a plugin must link against the OpenCPN import library
  # (opencpn.lib from an OpenCPN build or the plugin SDK).
  set(OCPN_IMPORT_LIB "" CACHE FILEPATH "Path to opencpn.lib (Windows only)")
  if(OCPN_IMPORT_LIB)
    target_link_libraries(s411_pi PRIVATE ${OCPN_IMPORT_LIB})
  else()
    message(WARNING "OCPN_IMPORT_LIB not set - the Windows link step will "
                    "fail. Point it at opencpn.lib from an OpenCPN build.")
  endif()
  target_link_libraries(s411_pi PRIVATE glu32 opengl32)
endif()

if(APPLE)
  # host symbols (OpenCPN plugin API) are resolved at load time
  set_target_properties(s411_pi PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()

# ---------------------------------------------------------------------------
# headless parser/loader tests (no GUI, no OpenCPN needed):
#   cmake -DS411_BUILD_TESTS=ON .. && make && ./s411_test <fixtures-dir>
# ---------------------------------------------------------------------------
option(S411_BUILD_TESTS "Build the headless parser tests" OFF)
if(S411_BUILD_TESTS)
  # console-only wx (base + xml): the test binary must not require a display
  find_package(wxWidgets 3.0 REQUIRED COMPONENTS xml base)
  add_executable(s411_test
    tests/test_main.cpp
    src/S411Data.cpp
    src/S411Tess.cpp
    src/S411Router.cpp
    src/S411Boundary.cpp)
  target_include_directories(s411_test PRIVATE src)
  target_link_libraries(s411_test PRIVATE ${wxWidgets_LIBRARIES})
  if(TARGET OpenGL::GLU)
    target_link_libraries(s411_test PRIVATE OpenGL::GLU)
  else()
    target_link_libraries(s411_test PRIVATE ${OPENGL_glu_LIBRARY})
  endif()
  if(WIN32)
    target_link_libraries(s411_test PRIVATE glu32 opengl32)
  endif()
endif()
