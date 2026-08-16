cmake_minimum_required(VERSION 3.22)

# Standalone on purpose: this does not pull in JUCE or Quarry, so it configures and builds
# in seconds and can be thrown away without touching the plugin's build.
project(loopback_spike LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Process loopback is a Windows API. This tool is Windows-only.")
endif()

add_executable(loopback_spike loopback_spike.cpp)

target_compile_features(loopback_spike PRIVATE cxx_std_17)

# Process loopback needs the Windows 10 2004 (build 20348) SDK headers for
# audioclientactivationparams.h. _WIN32_WINNT_WIN10 keeps the guards in that header open.
target_compile_definitions(loopback_spike PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00)

target_link_libraries(loopback_spike PRIVATE ole32 mmdevapi)

if (MSVC)
    target_compile_options(loopback_spike PRIVATE /W4 /permissive-)
endif()
