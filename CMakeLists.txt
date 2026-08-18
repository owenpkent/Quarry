cmake_minimum_required(VERSION 3.16)
project(QuarryPlugin VERSION 1.1.0)

# C++ settings
set(CMAKE_CXX_STANDARD 17)
enable_language(CXX)

#Compile commands, useful for some IDEs like VS-Code
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

#Minimum MacOS target, set globally
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.11" CACHE STRING "Minimum OS X deployment version" FORCE)

option(UniversalBinary "Build universal binary for mac" OFF)
option(RTNeural_Release "When CMAKE_BUILD_TYPE=Debug, overwrite it to Release for RTNeural only" OFF)
option(LTO "Enable Link Time Optimization" ON)

if (UniversalBinary)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE INTERNAL "")
    message(STATUS "Building Universal Binary")
endif ()

#static linking in Windows
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(ThirdParty/JUCE)
add_subdirectory(ThirdParty/RTNeural)

set(BaseTargetName Quarry)

# Set COPY_PLUGIN_AFTER_BUILD to TRUE on Mac and to FALSE on Windows due to permission issues
if (APPLE)
    set(COPY_PLUGIN_AFTER_BUILD TRUE)
else ()
    set(COPY_PLUGIN_AFTER_BUILD FALSE)
endif ()

juce_add_plugin("${BaseTargetName}"
        # VERSION ...                               # Set this if the plugin version is different to the project version
        ICON_BIG "${CMAKE_CURRENT_LIST_DIR}/Quarry/Assets/logo.png"
        ICON_SMALL "${CMAKE_CURRENT_LIST_DIR}/Quarry/Assets/logo.png"
        COMPANY_NAME "OK Studio"
        BUNDLE_ID "com.okstudio.quarry"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD}
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Need access to Microphone"
        HARDENED_RUNTIME_ENABLED TRUE
        # Distinct from upstream NeuralNote (DrAu/NRNT) so both can be installed
        # side by side without a DAW treating them as the same plugin.
        PLUGIN_MANUFACTURER_CODE OKSt
        PLUGIN_CODE Quar
        FORMATS AU VST3 Standalone
        PRODUCT_NAME "Quarry")

juce_generate_juce_header(${BaseTargetName})

# Source files
file(GLOB_RECURSE SOURCES_PLUGIN ${CMAKE_CURRENT_LIST_DIR}/Quarry/*.cpp ${CMAKE_CURRENT_LIST_DIR}/Lib/*.cpp)
list(REMOVE_ITEM SOURCES_PLUGIN ${CMAKE_CURRENT_LIST_DIR}/Lib/Model/BasicPitchCNN.cpp)
file(GLOB_RECURSE HEADERS_PLUGIN ${CMAKE_CURRENT_LIST_DIR}/Quarry/*.h ${CMAKE_CURRENT_LIST_DIR}/Lib/*.h)
list(REMOVE_ITEM HEADERS_PLUGIN ${CMAKE_CURRENT_LIST_DIR}/Lib/Model/BasicPitchCNN.h)

target_sources(${BaseTargetName} PRIVATE ${SOURCES_PLUGIN} ${HEADERS_PLUGIN})

file(GLOB_RECURSE lib_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_LIST_DIR}/Lib/*)

foreach (dir ${lib_dirs})
    IF (IS_DIRECTORY ${dir})
        target_include_directories(${BaseTargetName} PRIVATE ${dir})
    ELSE ()
        CONTINUE()
    ENDIF ()
endforeach ()

file(GLOB_RECURSE source_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_LIST_DIR}/Quarry/*)

foreach (dir ${source_dirs})
    IF (IS_DIRECTORY ${dir})
        target_include_directories(${BaseTargetName} PRIVATE ${dir})
    ELSE ()
        CONTINUE()
    ENDIF ()
endforeach ()

#Binary data
file(GLOB RESOURCES_FILES ${CMAKE_CURRENT_LIST_DIR}/Lib/ModelData/*.json
        ${CMAKE_CURRENT_LIST_DIR}/Lib/ModelData/*.ort
        ${CMAKE_CURRENT_LIST_DIR}/Quarry/Assets/*.ttf
        ${CMAKE_CURRENT_LIST_DIR}/Quarry/Assets/*.png
        ${CMAKE_CURRENT_LIST_DIR}/Quarry/Assets/*.svg)

juce_add_binary_data(bin_data SOURCES ${RESOURCES_FILES})

add_library(BasicPitchCNN STATIC ${CMAKE_CURRENT_LIST_DIR}/Lib/Model/BasicPitchCNN.cpp)
target_include_directories(BasicPitchCNN PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/RTNeural)
if ((CMAKE_BUILD_TYPE STREQUAL "Debug") AND RTNeural_Release)
    if (MSVC)
        target_compile_options(BasicPitchCNN PUBLIC /O2) # or maybe /Ox
    else () # clang or gcc
        target_compile_options(BasicPitchCNN PUBLIC -O3) # or maybe -Ofast
    endif ()
endif ()
target_link_libraries(BasicPitchCNN PUBLIC RTNeural bin_data)

target_include_directories(${BaseTargetName} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ONNXRuntime/${ONNXRUNTIME_DIRNAME}/include)
target_include_directories(${BaseTargetName} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/minimp3)

# okstudio JUCE kit, vendored copies from github.com/owenpkent/okstudio-juce-kit. That repo
# is private, so the headers live here instead of a submodule and must be re-synced by hand
# when the kit changes; see ThirdParty/okstudio/README.md. Header-only; we use
# okstudio/WasapiLoopback.h for recording what the computer is playing (Windows only, the
# header is empty elsewhere).
target_include_directories(${BaseTargetName} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/okstudio/include)

# Vendoring means the copies above can silently fall behind the kit. If this machine has a
# kit checkout, compare the two and say so. Clones without the kit see nothing, which is the
# point: the warning fires where the drift actually originates.
set(OKSTUDIO_KIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../okstudio-juce-kit"
        CACHE PATH "Optional okstudio-juce-kit checkout, used only to detect vendored header drift")
if (EXISTS "${OKSTUDIO_KIT_DIR}/include/okstudio")
    set(_okstudio_pin "unknown")
    if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/ThirdParty/okstudio/UPSTREAM.txt")
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/ThirdParty/okstudio/UPSTREAM.txt" _okstudio_pin_line
                REGEX "^commit=")
        if (_okstudio_pin_line)
            string(REPLACE "commit=" "" _okstudio_pin "${_okstudio_pin_line}")
        endif ()
    endif ()
    # Discover what is vendored rather than listing it, so adding a header to
    # tools/sync_okstudio.py cannot leave this check silently behind.
    file(GLOB _vendored_hdrs "${CMAKE_CURRENT_LIST_DIR}/ThirdParty/okstudio/include/okstudio/*.h")
    foreach (_vendored_hdr ${_vendored_hdrs})
        get_filename_component(_hdr "${_vendored_hdr}" NAME)
        set(_upstream_hdr "${OKSTUDIO_KIT_DIR}/include/okstudio/${_hdr}")
        if (EXISTS "${_upstream_hdr}")
            file(SHA256 "${_upstream_hdr}" _upstream_sha)
            file(SHA256 "${_vendored_hdr}" _vendored_sha)
            if (NOT _upstream_sha STREQUAL _vendored_sha)
                message(WARNING
                        "Vendored okstudio/${_hdr} has drifted from the kit at ${OKSTUDIO_KIT_DIR} "
                        "(vendored copy pinned at ${_okstudio_pin}). "
                        "Run: py tools/sync_okstudio.py")
            endif ()
        endif ()
    endforeach ()
endif ()

target_compile_definitions(${BaseTargetName}
        PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

add_library(onnxruntime STATIC IMPORTED)

if (APPLE)
    set_property(TARGET onnxruntime PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/onnxruntime/lib/libonnxruntime.a)

elseif (WIN32)
    set_property(TARGET onnxruntime APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)

    set_target_properties(onnxruntime PROPERTIES
            IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
            IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_LIST_DIR}/ThirdParty/onnxruntime/lib/onnxruntime.lib"
    )
    set_target_properties(onnxruntime PROPERTIES
            MAP_IMPORTED_CONFIG_DEBUG Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    )
elseif (UNIX)
    set_property(TARGET onnxruntime PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/onnxruntime/lib/libonnxruntime.a)
endif ()


target_include_directories("${BaseTargetName}" PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ThirdParty/onnxruntime/include)


target_link_libraries(${BaseTargetName}
        PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        BasicPitchCNN
        onnxruntime
        bin_data
        PUBLIC
        juce_recommended_config_flags)

if (${LTO})
    target_link_libraries(${BaseTargetName} PUBLIC juce_recommended_lto_flags)
    message(STATUS "Link Time Optimization (LTO) enabled")
endif ()

if (BUILD_UNIT_TESTS)
    add_subdirectory(Tests)
endif ()

# The transcription bench. Off by default because it is a measuring instrument rather than part
# of the product, and it links the whole engine, so it is not free to build. Prefixed because
# RTNeural already owns a cache variable called BUILD_BENCH and turning that on by accident
# builds a different tool entirely.
if (QUARRY_BUILD_BENCH)
    add_subdirectory(tools/bench)
endif ()

target_compile_definitions(${BaseTargetName}
        PRIVATE
        SAVE_DOWNSAMPLED_AUDIO=0
        USE_TEST_NOTE_FRAME_TO_TIME=0
)

# No ASIO. Steinberg's SDK cannot be redistributed without a signed agreement, and Quarry does
# not need it: it records this machine's own playback through WASAPI loopback, so the round-trip
# latency ASIO exists to cut is latency Quarry never spends. Windows Audio and DirectSound remain.

