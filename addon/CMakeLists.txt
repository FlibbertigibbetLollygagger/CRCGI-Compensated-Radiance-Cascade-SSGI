cmake_minimum_required(VERSION 3.15)
project(crcgi_injector CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Path to a checkout of https://github.com/crosire/reshade (the folder that contains include/, deps/imgui and examples/utils).
# Use the SAME ReShade revision as the one you run: the add-on API and the bundled ImGui must match it (reshade_overlay.hpp
# refuses any other ImGui version at compile time).
set(RESHADE_DIR "" CACHE PATH "ReShade source checkout")
# Optional pin: the tag or commit this build must use. Empty = accept whatever RESHADE_DIR holds (the revision is still recorded).
set(RESHADE_EXPECTED_REF "" CACHE STRING "Required ReShade tag or commit (empty = do not enforce)")

if(NOT RESHADE_DIR)
  message(FATAL_ERROR "Set -DRESHADE_DIR=<path to the ReShade source checkout>")
endif()
foreach(required include/reshade.hpp include/reshade_events.hpp deps/imgui/imgui.h)
  if(NOT EXISTS "${RESHADE_DIR}/${required}")
    message(FATAL_ERROR "RESHADE_DIR is missing ${required} (check the path, and that the deps/imgui submodule is checked out)")
  endif()
endforeach()

# Record the exact ReShade revision (shown in the add-on's Build section and in the CI manifest).
set(RESHADE_REVISION "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${RESHADE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${RESHADE_DIR}" rev-parse HEAD
                  OUTPUT_VARIABLE RESHADE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${RESHADE_DIR}" describe --tags --always
                  OUTPUT_VARIABLE RESHADE_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(RESHADE_EXPECTED_REF)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${RESHADE_DIR}" rev-parse "${RESHADE_EXPECTED_REF}^{commit}"
                    OUTPUT_VARIABLE RESHADE_EXPECTED_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT RESHADE_EXPECTED_SHA STREQUAL RESHADE_REVISION)
      message(FATAL_ERROR "RESHADE_DIR is at ${RESHADE_REVISION}, but RESHADE_EXPECTED_REF=${RESHADE_EXPECTED_REF} is '${RESHADE_EXPECTED_SHA}'")
    endif()
  endif()
endif()
message(STATUS "Building against ReShade ${RESHADE_DESCRIBE} (${RESHADE_REVISION})")

# ReShade's examples/utils/state_tracking is NOT built in: D3D12 injection uses the add-on's own tracker (tracker.hpp, own GUID). That
# helper's private-data GUID collides with other add-ons' modified copies (ReShade Effect Shader Toggler), which corrupts memory.
add_library(crcgi_injector SHARED crcgi_injector.cpp state.hpp tracker.hpp gpu.hpp profiler.hpp camera.hpp events.hpp ui.hpp)
target_include_directories(crcgi_injector PRIVATE "${RESHADE_DIR}/include" "${RESHADE_DIR}/deps/imgui")
target_compile_definitions(crcgi_injector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN "CRCGI_RESHADE_REVISION=\"${RESHADE_REVISION}\"")
if(MINGW)
  # Cross-compile syntax/link check only. NOT a usable add-on: MinGW GCC returns small structs from member functions differently
  # from MSVC, so it cannot call an MSVC-built ReShade correctly. Release builds must use MSVC or clang-cl (MSVC ABI).
  target_link_options(crcgi_injector PRIVATE -static)
endif()
if(MSVC)
  # Warnings are reviewed, not silenced.
  target_compile_options(crcgi_injector PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(CRCGI_ADDON_SUFFIX ".addon64")
else()
  set(CRCGI_ADDON_SUFFIX ".addon32")   # 32-bit games (build with -A Win32)
endif()
set_target_properties(crcgi_injector PROPERTIES PREFIX "" SUFFIX "${CRCGI_ADDON_SUFFIX}")
