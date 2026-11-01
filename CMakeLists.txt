cmake_minimum_required(VERSION 3.15)
project(QUBE LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- scheduling engine: pure C++17, no graphics dependency
add_library(qube_engine STATIC src/engine/Engine.cpp)
target_include_directories(qube_engine PUBLIC src)

# --- raylib (downloaded and built automatically the first time)
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(raylib)

# --- graphical app
add_executable(qube src/main.cpp)
target_link_libraries(qube PRIVATE qube_engine raylib)
if(MINGW)
  target_link_options(qube PRIVATE -static-libgcc -static-libstdc++)
endif()

# --- engine tests (no graphics)
add_executable(qube_tests tests/engine_tests.cpp)
target_link_libraries(qube_tests PRIVATE qube_engine)
enable_testing()
add_test(NAME engine COMMAND qube_tests)
