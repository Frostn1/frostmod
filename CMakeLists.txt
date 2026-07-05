cmake_minimum_required(VERSION 3.15)
project(FrostMod CXX)

# FrostMod injects into a 64-bit process, so it MUST be built x64.
#   cmake -B build -A x64
#   cmake --build build --config Release
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FrostMod must be built as x64 (mxbikes.exe is 64-bit). Use: cmake -A x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- MinHook (function hooking) ------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# --- the mod DLL ---------------------------------------------------------------
add_library(frostmod SHARED src/frostmod.cpp src/serverfilter.cpp)
target_link_libraries(frostmod PRIVATE minhook gdi32 user32)
# Be explicit about MinHook's headers in case the target doesn't export them.
target_include_directories(frostmod PRIVATE "${minhook_SOURCE_DIR}/include")
set_target_properties(frostmod PROPERTIES OUTPUT_NAME "frostmod" PREFIX "")

# --- the frostmod launcher / monitor exe --------------------------------------
# This is the app you actually run. It injects frostmod.dll, then STAYS OPEN as a
# console: it lists the .pkz mods it found, streams frostmod.log live, and lets
# you trigger a reload (R). The CMake target is named frostmod_app to avoid a
# clash with the DLL target, but its OUTPUT_NAME makes it build as frostmod.exe.
add_executable(frostmod_app src/launcher.cpp)
target_link_libraries(frostmod_app PRIVATE shell32)
set_target_properties(frostmod_app PROPERTIES OUTPUT_NAME "frostmod")

# put the exe and dll next to each other so frostmod.exe finds frostmod.dll by
# default. frostmod.exe + frostmod.dll coexist fine (different extensions).
set_target_properties(frostmod frostmod_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
