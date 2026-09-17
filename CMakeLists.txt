# gba-quad-link — four libmgba cores in one window, linked to Dolphin over TCP.
#
# This file is licensed under the Mozilla Public License 2.0; see LICENSE.
cmake_minimum_required(VERSION 3.20)
project(gba-quad-link C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(MGBA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mgba")
set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
foreach(dep "${MGBA_ROOT}/CMakeLists.txt" "${IMGUI_ROOT}/imgui.cpp")
    if(NOT EXISTS "${dep}")
        message(FATAL_ERROR
            "Missing ${dep}\n"
            "Submodules are not checked out. Run:\n"
            "  git submodule update --init --depth 1")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# libmgba
#
# mGBA reads the EXCLUDE_FROM_ALL directory property into LIBMGBA_ONLY, which
# forces a static core and turns off every frontend and optional dependency:
# no Qt, no SDL frontend, no ffmpeg, libzip, sqlite or minizip. That is the
# whole configuration — the settings below only trim what is left.
set(BINARY_NAME mgba CACHE INTERNAL "")
set(M_CORE_GBA ON  CACHE BOOL "" FORCE)
set(M_CORE_GB  OFF CACHE BOOL "" FORCE)   # no Game Boy core; nothing here uses it
add_subdirectory("${MGBA_ROOT}" mgba EXCLUDE_FROM_ALL)

# mGBA's own compile settings reach its sources through directory- and
# target-scoped properties, and it installs no headers, so nothing propagates
# to us. Both halves have to be copied across by hand.
#
# The include paths are the ordinary kind of nuisance. The defines are not:
# ENABLE_VFS, ENABLE_DIRECTORIES and MINIMAL_CORE each guard a member of
# `struct mCore` (include/mgba/core/core.h). Compile our side with a different
# set from the library's and the two disagree about where every field after
# them lives — which links cleanly and then misbehaves at runtime. So the set
# is read back off mGBA rather than written out again here, and it cannot
# drift when the submodule moves.
get_directory_property(MGBA_DIR_DEFS DIRECTORY "${MGBA_ROOT}" COMPILE_DEFINITIONS)
get_target_property(MGBA_TGT_DEFS mgba COMPILE_DEFINITIONS)
set(MGBA_DEFS)
foreach(d IN LISTS MGBA_DIR_DEFS MGBA_TGT_DEFS)
    if(d)
        list(APPEND MGBA_DEFS "${d}")
    endif()
endforeach()
list(REMOVE_DUPLICATES MGBA_DEFS)
message(STATUS "libmgba defines: ${MGBA_DEFS}")

add_library(mgba_headers INTERFACE)
target_include_directories(mgba_headers SYSTEM INTERFACE
    "${MGBA_ROOT}/include"
    "${MGBA_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/mgba/include")
target_compile_definitions(mgba_headers INTERFACE ${MGBA_DEFS})
target_link_libraries(mgba_headers INTERFACE mgba)

# ---------------------------------------------------------------------------
# Dear ImGui, with the SDL_Renderer backend so the menu draws over the
# composited quadrants without a GL context of its own.
add_library(imgui STATIC
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_sdlrenderer2.cpp")
target_include_directories(imgui SYSTEM PUBLIC
    "${IMGUI_ROOT}" "${IMGUI_ROOT}/backends")

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(imgui PUBLIC SDL2::SDL2)

# ---------------------------------------------------------------------------
add_executable(gba-quad-link
    src/main.cpp
    src/gba_instance.cpp
    src/controls.cpp
    src/paths.cpp)
target_include_directories(gba-quad-link PRIVATE src)
target_link_libraries(gba-quad-link PRIVATE
    mgba_headers imgui SDL2::SDL2 Threads::Threads)
