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
add_library(frostmod SHARED src/frostmod.cpp)
target_link_libraries(frostmod PRIVATE minhook gdi32 user32)
# Be explicit about MinHook's headers in case the target doesn't export them.
target_include_directories(frostmod PRIVATE "${minhook_SOURCE_DIR}/include")
set_target_properties(frostmod PROPERTIES OUTPUT_NAME "frostmod" PREFIX "")

# --- the injector exe ----------------------------------------------------------
add_executable(injector src/injector.cpp)

# put both next to each other so `injector.exe` finds `frostmod.dll` by default
set_target_properties(frostmod injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
