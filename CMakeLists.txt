cmake_minimum_required(VERSION 3.22)
project(EarsBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4)        # JUCE >= 7 required (Convolution realtime-safe)
FetchContent_MakeAvailable(juce)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0)
FetchContent_MakeAvailable(Catch2)
# Catch2's CMake helpers (Catch.cmake -> catch_discover_tests) are NOT registered by
# FetchContent; add its extras dir to the module path. Note the lowercase catch2_SOURCE_DIR
# (CMake lowercases the declared name). Without this, include(Catch) fails at configure time.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

# ---- Core DSP library (no devices, no GUI) ----
add_library(eb_core STATIC
    src/cal/CalFile.cpp
    src/cal/FirDesigner.cpp
    src/audio/ProcessingGraph.cpp)
target_include_directories(eb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_core PUBLIC cxx_std_20)
# Modules whose types appear in eb_core's PUBLIC headers (FirDesigner.h / ProcessingGraph.h
# include <juce_dsp/..> and <juce_audio_basics/..>) MUST be PUBLIC so their include dirs/defines
# reach consumers (eb_tests); juce_core is implementation-only -> PRIVATE. Link the recommended
# config/warning flags PUBLIC — every official JUCE example links them to the module-consuming target.
target_link_libraries(eb_core
    PUBLIC
        juce::juce_audio_basics juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    PRIVATE
        juce::juce_core)
target_compile_definitions(eb_core PUBLIC
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ---- Engine library (devices, clock bridge, health, facade) ----
add_library(eb_engine STATIC
    src/audio/DeviceId.cpp
    src/audio/ModelDetect.cpp
    src/audio/ClockBridge.cpp
    src/audio/HealthMonitor.cpp
    src/audio/DeviceManager.cpp
    src/audio/AudioEngine.cpp)
target_sources(eb_engine PRIVATE src/audio/CalBinder.cpp)
target_sources(eb_engine PRIVATE src/audio/AsioFallback.cpp)
target_sources(eb_engine PRIVATE src/audio/LrVerify.cpp)
target_include_directories(eb_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_engine PUBLIC cxx_std_20)
# DeviceManager.h / AudioEngine.h are PUBLIC headers that #include <juce_audio_devices/...> and
# declare juce::AudioDeviceManager / std::unique_ptr<juce::AudioIODevice> members, so juce_audio_devices
# is part of eb_engine's PUBLIC interface and MUST be linked PUBLIC (mirrors the eb_core rule) so its
# include dirs + JUCE config defines (JUCE_MODULE_AVAILABLE_juce_audio_devices, JUCE_WASAPI, ...) reach consumers.
target_link_libraries(eb_engine
    PUBLIC
        eb_core
        juce::juce_audio_basics juce::juce_dsp juce::juce_core
        juce::juce_audio_devices
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(eb_engine PUBLIC
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ---- GUI + state library (components, theme, persistence) ----
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp
    src/gui/MainComponent.cpp)
target_include_directories(eb_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_gui PUBLIC cxx_std_20)
target_link_libraries(eb_gui
    PUBLIC
        eb_engine
        juce::juce_gui_extra juce::juce_gui_basics
        juce::juce_audio_basics juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(eb_gui PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

# ---- Placeholder GUI app (proves JUCE links + builds on both OSes) ----
juce_add_gui_app(EarsBridge PRODUCT_NAME "EARS Bridge")
target_sources(EarsBridge PRIVATE src/Main.cpp)
# Set these on the app directly: juce_gui_extra (curl / WebBrowserComponent) links to the app,
# not through eb_core, so don't rely on transitive defines reaching it.
target_compile_definitions(EarsBridge PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(EarsBridge PRIVATE
    eb_gui eb_core juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- Console diagnostic (live device enumeration + passthrough; manual use) ----
juce_add_console_app(eb_diag PRODUCT_NAME "eb_diag")
target_sources(eb_diag PRIVATE src/tools/eb_diag_main.cpp)
target_compile_definitions(eb_diag PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(eb_diag PRIVATE
    eb_core eb_engine juce::juce_audio_devices
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

enable_testing()
add_subdirectory(tests)
