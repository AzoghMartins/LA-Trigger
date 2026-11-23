cmake_minimum_required(VERSION 3.22)

project(LATrigger VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LATRIGGER_BUILD_TESTS "Build the headless DSP engine tests" ON)
option(LATRIGGER_COPY_AFTER_BUILD "Install plugins into ~/.vst3 and ~/.lv2 after building" OFF)
set(LATRIGGER_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (fetched from GitHub when empty)")

# ---------------------------------------------------------------------------------------------
# JUCE
if(LATRIGGER_JUCE_DIR)
    add_subdirectory(${LATRIGGER_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------------------------
# DSP engine (no JUCE dependency, unit tested separately)
add_library(latrigger_engine STATIC Source/dsp/TriggerEngine.cpp)
target_include_directories(latrigger_engine PUBLIC Source)
target_compile_options(latrigger_engine PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

# ---------------------------------------------------------------------------------------------
# Plugin
juce_add_plugin(LATrigger
    PRODUCT_NAME "LA-Trigger"
    COMPANY_NAME "LA Audio"
    BUNDLE_ID "com.la-audio.latrigger"
    PLUGIN_MANUFACTURER_CODE LaAu
    PLUGIN_CODE LaTr
    FORMATS VST3 LV2 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Dynamics Tools
    LV2URI "https://github.com/AzoghMartins/LA-Trigger"
    COPY_PLUGIN_AFTER_BUILD ${LATRIGGER_COPY_AFTER_BUILD})

target_sources(LATrigger PRIVATE
    Source/Parameters.cpp
    Source/PluginProcessor.cpp
    Source/gui/Theme.cpp
    Source/gui/Widgets.cpp
    Source/gui/Displays.cpp
    Source/gui/PluginEditor.cpp)

target_compile_definitions(LATrigger PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LATrigger
    PRIVATE
        latrigger_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
if(LATRIGGER_BUILD_TESTS)
    enable_testing()
    add_executable(latrigger_tests tests/EngineTests.cpp)
    target_link_libraries(latrigger_tests PRIVATE latrigger_engine)
    add_test(NAME engine COMMAND latrigger_tests)

    # Drives the real processor + editor like a host (needs an X display, e.g. xvfb-run).
    juce_add_console_app(latrigger_integration PRODUCT_NAME "LA-Trigger Integration Test")
    target_sources(latrigger_integration PRIVATE
        tests/PluginIntegrationTest.cpp
        Source/Parameters.cpp
        Source/PluginProcessor.cpp
        Source/gui/Theme.cpp
        Source/gui/Widgets.cpp
        Source/gui/Displays.cpp
        Source/gui/PluginEditor.cpp)
    target_compile_definitions(latrigger_integration PRIVATE
        JucePlugin_Name="LA-Trigger"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(latrigger_integration PRIVATE
        latrigger_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_recommended_config_flags)
    add_test(NAME integration COMMAND latrigger_integration)
endif()
