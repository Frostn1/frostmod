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

# --- PE version resource (AV false-positive hardening) ------------------------
# An unsigned binary with NO version-info block reads as a "suspicious PE" to AV
# ML heuristics (SentinelOne "Static AI", Symantec "ML.Attribute.HighConfidence",
# etc.). Embedding CompanyName/ProductName/FileDescription/FileVersion gives each
# binary a legitimate identity and removes one packaging red flag. Keep
# FROSTMOD_VERSION in sync with src/version.h. See docs/antivirus-false-positives.md.
set(FROSTMOD_VERSION "0.9.7")
string(REPLACE "." ";" _frostmod_ver_parts "${FROSTMOD_VERSION}")
list(GET _frostmod_ver_parts 0 FROSTMOD_VERSION_MAJOR)
list(GET _frostmod_ver_parts 1 FROSTMOD_VERSION_MINOR)
list(GET _frostmod_ver_parts 2 FROSTMOD_VERSION_PATCH)

# Generate a per-target .rc from version_info.rc.in. MSVC's rc.exe compiles a .rc
# automatically when it is listed among a target's sources. Only meaningful on
# Windows; on other hosts the helper returns "" so configure still succeeds.
function(frostmod_version_rc out_var filetype original_filename description)
    if(NOT WIN32)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    set(FROSTMOD_RC_FILETYPE          "${filetype}")
    set(FROSTMOD_RC_ORIGINAL_FILENAME "${original_filename}")
    set(FROSTMOD_RC_FILE_DESCRIPTION  "${description}")
    set(_rc_out "${CMAKE_BINARY_DIR}/generated/${original_filename}.rc")
    configure_file("${CMAKE_SOURCE_DIR}/src/version_info.rc.in" "${_rc_out}" @ONLY)
    set(${out_var} "${_rc_out}" PARENT_SCOPE)
endfunction()

frostmod_version_rc(FROSTMOD_DLL_RC VFT_DLL "frostmod.dll" "FrostMod in-game overlay / mod loader for MX Bikes")
frostmod_version_rc(FROSTMOD_EXE_RC VFT_APP "frostmod.exe" "FrostMod launcher / mod loader for MX Bikes")
frostmod_version_rc(FROSTSERVER_DLL_RC VFT_DLL "frostserver.dlo" "FrostServer - dedicated-server map/link API for MX Bikes")
frostmod_version_rc(FROSTSERVER_EXE_RC VFT_APP "frostserver.exe" "FrostServer standalone - map/link API for MX Bikes")

# --- MinHook (function hooking) ------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# --- the mod DLL ---------------------------------------------------------------
add_library(frostmod SHARED src/frostmod.cpp src/serverfilter.cpp ${FROSTMOD_DLL_RC})
target_link_libraries(frostmod PRIVATE minhook gdi32 user32 opengl32)
# Be explicit about MinHook's headers in case the target doesn't export them.
target_include_directories(frostmod PRIVATE "${minhook_SOURCE_DIR}/include")
set_target_properties(frostmod PROPERTIES OUTPUT_NAME "frostmod" PREFIX "")

# --- the frostmod launcher / monitor exe --------------------------------------
# This is the app you actually run. It injects frostmod.dll, then STAYS OPEN as a
# console: it lists the .pkz mods it found, streams frostmod.log live, and lets
# you trigger a reload (R). The CMake target is named frostmod_app to avoid a
# clash with the DLL target, but its OUTPUT_NAME makes it build as frostmod.exe.
add_executable(frostmod_app src/launcher.cpp ${FROSTMOD_EXE_RC})
# shell32: SHGetFolderPath; advapi32: the HKCU Run-key (auto-start); user32: ShowWindow;
# winhttp: the GitHub update check/download.
target_link_libraries(frostmod_app PRIVATE shell32 advapi32 user32 winhttp)
set_target_properties(frostmod_app PROPERTIES OUTPUT_NAME "frostmod")

# --- frostserver (dedicated-server companion) ---------------------------------
# frostserver.dlo is a PiBoSo plugin loaded INTO the MX Bikes dedicated server:
# it learns the running track (RaceEvent callback) and serves a tiny read-only
# HTTP API (track name + configured mxb-mods link) that FrostMod clients query.
# The same source also builds a standalone frostserver.exe (FROSTSERVER_EXE) that
# serves the API with no game attached, for testing the client / MXB App flow.
add_library(frostserver SHARED src/frostserver.cpp ${FROSTSERVER_DLL_RC})
target_link_libraries(frostserver PRIVATE ws2_32)
set_target_properties(frostserver PROPERTIES OUTPUT_NAME "frostserver" PREFIX "")

add_executable(frostserver_app src/frostserver.cpp ${FROSTSERVER_EXE_RC})
target_compile_definitions(frostserver_app PRIVATE FROSTSERVER_EXE)
target_link_libraries(frostserver_app PRIVATE ws2_32)
set_target_properties(frostserver_app PROPERTIES OUTPUT_NAME "frostserver")

# put the exe and dll next to each other so frostmod.exe finds frostmod.dll by
# default. frostmod.exe + frostmod.dll coexist fine (different extensions).
set_target_properties(frostmod frostmod_app frostserver frostserver_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- optional Authenticode signing (AV false-positive hardening) --------------
# Unsigned binaries are the single biggest AV-ML red flag; a signed binary that
# accrues reputation is the strongest benign signal. This is OFF by default and
# needs a certificate you supply. With an OV/PFX cert:
#   cmake -B build -A x64 -DFROSTMOD_SIGN=ON \
#         -DFROSTMOD_SIGN_PFX="C:/keys/frostmod.pfx" -DFROSTMOD_SIGN_PASSWORD="****"
# With an EV cert on a hardware token, leave PFX empty and pass the thumbprint:
#   -DFROSTMOD_SIGN=ON -DFROSTMOD_SIGN_SHA1=<cert-thumbprint>
# Each output binary is then signed + RFC3161-timestamped as a post-build step.
option(FROSTMOD_SIGN "Authenticode-sign the output binaries after build" OFF)
set(FROSTMOD_SIGNTOOL "signtool" CACHE STRING "signtool.exe (name on PATH or full path)")
set(FROSTMOD_SIGN_PFX "" CACHE FILEPATH "Path to code-signing .pfx (OV/PFX cert)")
set(FROSTMOD_SIGN_PASSWORD "" CACHE STRING "Password for FROSTMOD_SIGN_PFX")
set(FROSTMOD_SIGN_SHA1 "" CACHE STRING "Cert SHA1 thumbprint (EV/token cert; alternative to PFX)")
set(FROSTMOD_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING "RFC3161 timestamp URL")

if(FROSTMOD_SIGN)
    if(FROSTMOD_SIGN_PFX)
        set(_frostmod_sign_id /f "${FROSTMOD_SIGN_PFX}")
        if(FROSTMOD_SIGN_PASSWORD)
            list(APPEND _frostmod_sign_id /p "${FROSTMOD_SIGN_PASSWORD}")
        endif()
    elseif(FROSTMOD_SIGN_SHA1)
        set(_frostmod_sign_id /sha1 "${FROSTMOD_SIGN_SHA1}")
    else()
        message(FATAL_ERROR "FROSTMOD_SIGN=ON but neither FROSTMOD_SIGN_PFX nor FROSTMOD_SIGN_SHA1 is set.")
    endif()
    foreach(_frostmod_target frostmod frostmod_app frostserver frostserver_app)
        add_custom_command(TARGET ${_frostmod_target} POST_BUILD
            COMMAND "${FROSTMOD_SIGNTOOL}" sign /fd SHA256 ${_frostmod_sign_id}
                    /tr "${FROSTMOD_SIGN_TIMESTAMP_URL}" /td SHA256
                    "$<TARGET_FILE:${_frostmod_target}>"
            VERBATIM
            COMMENT "Authenticode-signing $<TARGET_FILE:${_frostmod_target}>")
    endforeach()
endif()

# --- frostmod.dlo (PiBoSo plugin artifact) ------------------------------------
# MX Bikes auto-loads plugins named *.dlo from its plugins\ folder, so plugin mode
# needs a frostmod.dlo. It is a byte-for-byte copy of frostmod.dll (identical PE, so
# any Authenticode signature carries over verbatim): injected mode loads the .dll,
# plugin mode the .dlo. This runs LAST so it copies the FINAL binary - after the
# signing step above when FROSTMOD_SIGN=ON, so the .dlo is signed too.
add_custom_command(TARGET frostmod POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:frostmod>" "$<TARGET_FILE_DIR:frostmod>/frostmod.dlo"
    VERBATIM
    COMMENT "Copying frostmod.dll -> frostmod.dlo (PiBoSo plugin)")

# frostserver.dlo - the server-side plugin artifact (byte-for-byte copy of the
# frostserver.dll, same as frostmod.dlo). Runs LAST so it inherits any signature.
add_custom_command(TARGET frostserver POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:frostserver>" "$<TARGET_FILE_DIR:frostserver>/frostserver.dlo"
    VERBATIM
    COMMENT "Copying frostserver.dll -> frostserver.dlo (PiBoSo server plugin)")
