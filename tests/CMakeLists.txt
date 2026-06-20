add_executable(eb_tests
    test_smoke.cpp
    test_calfile.cpp
    test_calpairvalidator.cpp
    test_firdesigner.cpp
    test_processinggraph.cpp
    test_deviceid.cpp
    test_modeldetect.cpp
    test_clockbridge.cpp
    test_clockbridge_transport.cpp
    test_healthmonitor.cpp
    test_devicemanager.cpp
    test_audioengine.cpp
    test_audioengine_callbacks.cpp
    test_settings.cpp
    test_plotmath.cpp
    test_ratemenu.cpp
    test_calbinder.cpp
    test_asiofallback.cpp
    test_lrverify.cpp
    test_levelmeter.cpp
    test_calslot.cpp
    test_updatecheck.cpp
    test_clipstatus.cpp
    test_rawrailstatus.cpp
    test_measurementsession.cpp
    test_combinegate.cpp)
# Link the JUCE modules the test sources include directly, plus the recommended config flags,
# so eb_tests compiles regardless of eb_core's link visibility.
target_link_libraries(eb_tests PRIVATE
    eb_core eb_engine eb_gui
    juce::juce_audio_basics juce::juce_dsp juce::juce_audio_devices
    juce::juce_recommended_config_flags
    Catch2::Catch2WithMain)
target_compile_definitions(eb_tests PRIVATE
    EB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

include(Catch)
catch_discover_tests(eb_tests)

add_executable(diag_clockbridge diag_clockbridge.cpp)
target_link_libraries(diag_clockbridge PRIVATE
    eb_core eb_engine
    juce::juce_audio_basics juce::juce_dsp juce::juce_audio_devices
    juce::juce_recommended_config_flags)
target_compile_definitions(diag_clockbridge PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
