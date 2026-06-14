#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "state/Settings.h"
#include <juce_core/juce_core.h>

using Catch::Matchers::WithinAbs;

// Each test gets a private temp dir so the PropertiesFile can't collide with a
// real user-settings file or another test run.
static juce::File makeTempDir() {
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("eb_settings_test_" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

TEST_CASE("Settings round-trips every field through a PropertiesFile") {
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        s.setInputKey  ("WASAPI|EARS Pro|VID_2752&PID_0034");
        s.setOutputKey ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}");
        s.setInputModel (eb::EarsModel::EarsPro);
        s.setSampleRate (96000.0);
        s.setOutputBitDepth (24);
        s.setCombineMode (eb::CombineMode::TwoPassRight);
        s.setLeftCalPath  ("C:/cal/L_HPN_8604350.txt");
        s.setRightCalPath ("C:/cal/R_HPN_8604350.txt");
        s.setOutputTrimDb (-3.5);
        s.setFirLength (16384);
        s.setComplexPhase (true);
        s.flush();   // force write-through
    }

    eb::Settings reloaded (dir);   // re-open same folder -> reads back the file
    CHECK (reloaded.inputKey()  == juce::String ("WASAPI|EARS Pro|VID_2752&PID_0034"));
    CHECK (reloaded.outputKey() == juce::String ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}"));
    CHECK (reloaded.inputModel() == eb::EarsModel::EarsPro);
    CHECK_THAT (reloaded.sampleRate(), WithinAbs (96000.0, 1e-9));
    CHECK (reloaded.outputBitDepth() == 24);
    CHECK (reloaded.combineMode() == eb::CombineMode::TwoPassRight);
    CHECK (reloaded.leftCalPath()  == juce::String ("C:/cal/L_HPN_8604350.txt"));
    CHECK (reloaded.rightCalPath() == juce::String ("C:/cal/R_HPN_8604350.txt"));
    CHECK_THAT (reloaded.outputTrimDb(), WithinAbs (-3.5, 1e-9));
    CHECK (reloaded.firLength() == 16384);
    CHECK (reloaded.complexPhase() == true);

    dir.deleteRecursively();
}

TEST_CASE("Settings returns sane defaults on a fresh store") {
    auto dir = makeTempDir();
    eb::Settings s (dir);
    CHECK (s.inputKey().isEmpty());
    CHECK (s.outputKey().isEmpty());
    CHECK (s.inputModel() == eb::EarsModel::Unknown);
    CHECK_THAT (s.sampleRate(), WithinAbs (48000.0, 1e-9));  // default native EARS rate
    CHECK (s.outputBitDepth() == 24);
    CHECK (s.combineMode() == eb::CombineMode::TwoPassLeft);
    CHECK_THAT (s.outputTrimDb(), WithinAbs (0.0, 1e-9));
    CHECK (s.firLength() == 0);   // 0 = Auto: taps scale with rate via numTapsForRate()
    CHECK (s.complexPhase() == false);
    dir.deleteRecursively();
}
