cmake_minimum_required(VERSION 3.22)

# Standalone on purpose: no JUCE, no Quarry, Windows SDK only. It configures and builds in
# seconds and can be thrown away without touching the app's build.
project(screen_spike LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Windows.Graphics.Capture and Media Foundation are Windows APIs. This tool is Windows-only.")
endif()

add_executable(screen_spike screen_spike.cpp)

target_compile_features(screen_spike PRIVATE cxx_std_17)

target_compile_definitions(screen_spike PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)

# windowsapp.lib carries the WinRT activation entry points that C++/WinRT needs.
target_link_libraries(screen_spike PRIVATE
    d3d11 dxgi dwmapi
    mfplat mfreadwrite mfuuid
    ole32 user32 mmdevapi winmm
    windowsapp)

if (MSVC)
    target_compile_options(screen_spike PRIVATE /W4 /permissive-)
endif()
