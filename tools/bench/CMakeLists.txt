project(Bench VERSION 1.0)

# Linked exactly like the plugin, not like the unit tests: same engine, same posteriorgram code,
# and crucially the same USE_TEST_NOTE_FRAME_TO_TIME. Tests/ sets that to 1 to satisfy some
# timestamp arithmetic in basic-pitch's fixtures, and a bench built that way would report onset
# error the shipping build never produces.

juce_add_console_app(${PROJECT_NAME} PRODUCT_NAME "Quarry Bench")

juce_generate_juce_header(${PROJECT_NAME})

file(GLOB_RECURSE SOURCES_BENCH ${CMAKE_CURRENT_LIST_DIR}/../../Lib/*.cpp)
file(GLOB_RECURSE HEADERS_BENCH ${CMAKE_CURRENT_LIST_DIR}/../../Lib/*.h)

# Built separately as a static library by the top level, as it is for the plugin.
list(REMOVE_ITEM SOURCES_BENCH ${CMAKE_CURRENT_LIST_DIR}/../../Lib/Model/BasicPitchCNN.cpp)

target_sources(${PROJECT_NAME} PRIVATE Bench.cpp ${SOURCES_BENCH} ${HEADERS_BENCH})

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../ThirdParty/onnxruntime/include
        ${CMAKE_CURRENT_LIST_DIR}/../../ThirdParty/minimp3)

file(GLOB_RECURSE lib_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_LIST_DIR}/../../Lib/*)

foreach (dir ${lib_dirs})
    if (IS_DIRECTORY ${dir})
        target_include_directories(${PROJECT_NAME} PRIVATE ${dir})
    endif ()
endforeach ()

target_compile_definitions(${PROJECT_NAME} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        SAVE_DOWNSAMPLED_AUDIO=0
        USE_TEST_NOTE_FRAME_TO_TIME=0)

target_link_libraries(${PROJECT_NAME} PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        onnxruntime
        BasicPitchCNN
        bin_data
        PUBLIC
        juce_recommended_config_flags)

# The prebuilt onnxruntime.lib is compiled with /GL, so with LTO on the link fails outright
# (C1047) unless the compiler matches the one it was built with. Same guard as the plugin.
if (${LTO})
    target_link_libraries(${PROJECT_NAME} PUBLIC juce_recommended_lto_flags)
endif ()
