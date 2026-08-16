project(UnitTests VERSION 1.0)

juce_add_console_app(${PROJECT_NAME} PRODUCT_NAME "Unit Tests")

juce_generate_juce_header(${PROJECT_NAME})

file(GLOB_RECURSE SOURCES_TESTS ${CMAKE_CURRENT_LIST_DIR}/../Lib/*.cpp)
file(GLOB_RECURSE HEADERS_TESTS ${CMAKE_CURRENT_LIST_DIR}/../Lib/*.h)

target_sources(${PROJECT_NAME} PRIVATE Tests.cpp ${SOURCES_TESTS} ${HEADERS_TESTS})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/ONNXRuntime/${ONNXRUNTIME_DIRNAME}/include)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/minimp3)

# The sampler's arithmetic is header-only and juce_core only precisely so it can be tested
# here, without the recorder that uses it dragging in juce_audio_formats and the Windows
# audio stack.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../Quarry/Source)

file(GLOB_RECURSE lib_sources LIST_DIRECTORIES true ${CMAKE_CURRENT_LIST_DIR}/../Lib/*)

foreach (dir ${lib_sources})
    IF (IS_DIRECTORY ${dir})
        target_include_directories(${PROJECT_NAME} PRIVATE ${dir})
    ELSE ()
        CONTINUE()
    ENDIF ()
endforeach ()

target_compile_definitions(${PROJECT_NAME} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(${PROJECT_NAME} PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        onnxruntime
        BasicPitchCNN
        bin_data
        PUBLIC
        juce_recommended_config_flags)
#juce_recommended_warning_flags

# Honour the top level LTO option, as the plugin target does. The prebuilt onnxruntime.lib is
# compiled with /GL by a specific MSVC version, so with LTO on, linking fails outright (C1047)
# unless your compiler matches the one it was built with.
if (${LTO})
    target_link_libraries(${PROJECT_NAME} PUBLIC juce_recommended_lto_flags)
endif ()

target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/test_data"
        USE_TEST_NOTE_FRAME_TO_TIME=1
)

# Built, not run. SampleRecorder reaches juce_audio_formats and the Windows audio stack, so
# UnitTests cannot include it, and without this target nothing but the plugin would ever
# compile it. Deliberately no add_test(): a runner with no audio records its own silence and
# proves nothing. It links neither onnxruntime nor the transcription engine, so it builds in
# seconds and is the fastest way to exercise a real capture.
if (WIN32)
    juce_add_console_app(SamplerSmoke PRODUCT_NAME "Sampler Smoke")
    juce_generate_juce_header(SamplerSmoke)

    target_sources(SamplerSmoke PRIVATE
            SamplerSmoke.cpp
            ${CMAKE_CURRENT_LIST_DIR}/../Quarry/Source/Sampler/SampleRecorder.cpp
            ${CMAKE_CURRENT_LIST_DIR}/../Quarry/Source/Sampler/SourceIdentity.cpp)

    target_include_directories(SamplerSmoke PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/../Quarry/Source
            ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/okstudio/include)

    target_compile_definitions(SamplerSmoke PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)

    target_link_libraries(SamplerSmoke PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_basics
            juce::juce_graphics
            juce::juce_core
            PUBLIC
            juce_recommended_config_flags)
endif ()



