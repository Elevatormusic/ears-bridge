cmake_minimum_required(VERSION 3.22)
project(EarsBridge VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Statically link the MSVC runtime so the shipped EXE has NO Visual C++ redistributable
# dependency (otherwise it needs MSVCP140.dll / VCRUNTIME140.dll on the target machine).
# CMP0091 is NEW by default at cmake_minimum 3.22; this applies to all targets (incl. JUCE)
# for a consistent runtime.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
    src/cal/CalibrationPairValidator.cpp
    src/cal/FirDesigner.cpp
    src/audio/Deconvolver.cpp
    src/audio/ResponseShape.cpp
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
        juce::juce_core
        # juce_cryptography supplies juce::SHA256 used only inside CalFile.cpp (content-hash);
        # implementation-only -> PRIVATE so it does not leak into eb_core's public interface.
        juce::juce_cryptography)
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
target_sources(eb_engine PRIVATE src/audio/AsioFallback.cpp)
target_sources(eb_engine PRIVATE src/audio/LrVerify.cpp)
target_sources(eb_engine PRIVATE src/audio/MeasurementSession.cpp)
# The headless grade-poll DECISION (match-gate + stable-match debounce + grade), extracted from the GUI so
# it can be self-tested without the GUI/hardware. Pure logic over eb_core's Deconvolver/RefMonitor.
target_sources(eb_engine PRIVATE src/audio/ReferenceGradePoller.cpp)
# Task 3 (reference monitor): the loopback reference — pure validation + metadata
# everywhere, plus the WASAPI loopback capture (Windows-only, guarded inside the .cpp
# like EndpointFormat). Uses eb::referenceMatches (eb_core) for the single-sweep self-test
# and juce::SHA256 (juce_cryptography) for the content hash.
target_sources(eb_engine PRIVATE src/audio/LoopbackReference.cpp)
# Match-detection + diagnostic-logging build, Task 1: the rotating message-thread
# diagnostic logger. A leaf utility — juce_core only (no GUI/audio deps).
target_sources(eb_engine PRIVATE src/diag/DiagnosticLog.cpp)
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
        juce::juce_recommended_warning_flags
    PRIVATE
        # juce::SHA256 (LoopbackReference content hash) — implementation-only, like eb_core's use.
        juce::juce_cryptography)
target_compile_definitions(eb_engine PUBLIC
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ---- Plan 4 Task 7: CoreAudio aggregate device (portable link split) ----
# Portable TU compiled on ALL platforms: provides create()/destroy() stubs on non-macOS and the
# unconditional ~AggregateDevice() everywhere, so the symbols always resolve at link time (the
# Windows build excludes the .mm). The macOS-only .mm carries the real CoreAudio bodies.
target_sources(eb_engine PRIVATE src/platform/AggregateDevice.cpp)   # ALL platforms
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/AggregateDevice_mac.mm)
    set_source_files_properties(src/platform/AggregateDevice_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
    # CoreAudio / AudioToolbox / CoreFoundation are already linked transitively by
    # juce_audio_devices on macOS — no explicit -framework lines needed here.
endif()

# ---- Stable platform endpoint-id resolver (WASAPI IMMDevice id / CoreAudio device UID) ----
# Same portable split: the .cpp carries the Windows impl + a non-Apple fallback (empty on macOS);
# the .mm carries the CoreAudio impl (Apple only). Makes DeviceId.uid a real, replug-stable id so
# key() is replug/gain/rename-stable and the macOS aggregate gets the UIDs CoreAudio requires.
target_sources(eb_engine PRIVATE src/platform/EndpointUid.cpp)        # Windows impl + non-Apple stub
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/EndpointUid_mac.mm)
    set_source_files_properties(src/platform/EndpointUid_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

# ---- Endpoint shared mix-format sample-rate query (WASAPI PKEY_AudioEngine_DeviceFormat / CoreAudio) ----
# Same portable split as EndpointUid: the .cpp carries the Windows impl + a non-Apple fallback (0.0 on
# macOS); the .mm carries the CoreAudio impl (Apple only). Lets a later task detect whether the OS
# resampled the EARS capture stream (requested rate vs. endpoint mix rate); 0.0 means unverifiable.
target_sources(eb_engine PRIVATE src/platform/EndpointFormat.cpp)     # Windows impl + non-Apple stub
target_sources(eb_engine PRIVATE src/platform/OutputActivity.cpp)     # WASAPI render-peak read (hardware-Dirac detect); Windows impl + stub
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/EndpointFormat_mac.mm)
    set_source_files_properties(src/platform/EndpointFormat_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

# ---- GUI + state library (components, theme, persistence) ----
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp
    src/gui/HigScore.cpp
    src/gui/MainComponent.cpp
    src/net/UpdateChecker.cpp
    src/platform/DiracCompat.cpp     # Windows: Dirac shared-mode env-var helper (no-op elsewhere)
    src/platform/SystemA11y.cpp)     # Reduce Motion / Increase Contrast / Reduce Transparency (Windows impl + stub)
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
# Single source of truth for the running version (PROJECT_VERSION from project(...) above),
# used by the update checker. eb_gui is a plain static lib, so JUCE's generated ProjectInfo
# is not available here -- this macro replaces it.
target_compile_definitions(eb_gui PRIVATE EB_VERSION_STRING="${PROJECT_VERSION}")
# Dev-only design-QA harness (EB_HIG_STATES state sweep). OFF by default so shipped Release builds cannot
# be driven into fake UI states via an env var (audit #23); local dev opts in with -DEB_ENABLE_HIG_HARNESS=ON.
option(EB_ENABLE_HIG_HARNESS "Compile the EB_HIG_STATES design-QA harness into the app" OFF)
if(EB_ENABLE_HIG_HARNESS)
    target_compile_definitions(eb_gui PRIVATE EB_ENABLE_HIG_HARNESS=1)
endif()

# ---- GUI app ----
juce_add_gui_app(EarsBridge
    PRODUCT_NAME "EARS Bridge"
    COMPANY_NAME "EARS Bridge"
    # Explicit reverse-DNS bundle id: the default is derived from COMPANY_NAME and would be
    # "com.EARS Bridge.EarsBridge" — spaces make it an invalid macOS bundle identifier
    # (breaks code-signing / notarization).
    BUNDLE_ID "com.elevatormusic.earsbridge"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/installer/assets/icon.png")
target_sources(EarsBridge PRIVATE src/Main.cpp)
# Set these on the app directly: juce_gui_extra (curl / WebBrowserComponent) links to the app,
# not through eb_core, so don't rely on transitive defines reaching it.
target_compile_definitions(EarsBridge PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
# One source of truth for the app version: the update checker (in eb_gui) and the app's
# getApplicationVersion() both read EB_VERSION_STRING, derived from project(... VERSION ...).
target_compile_definitions(EarsBridge PRIVATE EB_VERSION_STRING="${PROJECT_VERSION}")
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
