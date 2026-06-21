#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "state/Settings.h"
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>   // juce::PropertiesFile (for the corrupt-file fixtures)
#include <cmath>                                          // std::nan / std::numeric_limits

using Catch::Matchers::WithinAbs;

// Each test gets a private temp dir so the PropertiesFile can't collide with a
// real user-settings file or another test run.
static juce::File makeTempDir() {
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("eb_settings_test_" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

// Open the EXACT same backing file eb::Settings loads (EarsBridge.settings, storeAsXML in `dir`)
// so a test can write raw key/values into it, then have a fresh eb::Settings read them back. This
// matches the Options that Settings::Settings(dir) uses.
static std::unique_ptr<juce::PropertiesFile> openBackingFile (const juce::File& dir) {
    juce::PropertiesFile::Options opts;
    opts.applicationName          = "EarsBridge";
    opts.filenameSuffix           = ".settings";
    opts.folderName               = "EarsBridge";
    opts.osxLibrarySubFolder      = "Application Support";
    opts.storageFormat            = juce::PropertiesFile::storeAsXML;
    opts.millisecondsBeforeSaving = 0;
    return std::make_unique<juce::PropertiesFile> (dir.getChildFile ("EarsBridge.settings"), opts);
}

TEST_CASE("Settings round-trips every field through a PropertiesFile") {
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        s.setInputKey  ("WASAPI|EARS Pro|VID_2752&PID_0034");
        s.setOutputKey ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}");
        s.setSampleRate (96000.0);
        s.setOutputBitDepth (24);
        s.setCombineMode (eb::CombineMode::Sum);
        s.setLeftCalPath  ("C:/cal/L_HPN_0000000.txt");
        s.setRightCalPath ("C:/cal/R_HPN_0000000.txt");
        s.setOutputTrimDb (-3.5);
        s.setFirLength (16384);
        s.setComplexPhase (true);
        s.flush();   // force write-through
    }

    eb::Settings reloaded (dir);   // re-open same folder -> reads back the file
    CHECK (reloaded.inputKey()  == juce::String ("WASAPI|EARS Pro|VID_2752&PID_0034"));
    CHECK (reloaded.outputKey() == juce::String ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}"));
    CHECK_THAT (reloaded.sampleRate(), WithinAbs (96000.0, 1e-9));
    CHECK (reloaded.outputBitDepth() == 24);
    CHECK (reloaded.combineMode() == eb::CombineMode::Sum);
    CHECK (reloaded.leftCalPath()  == juce::String ("C:/cal/L_HPN_0000000.txt"));
    CHECK (reloaded.rightCalPath() == juce::String ("C:/cal/R_HPN_0000000.txt"));
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
    CHECK_THAT (s.sampleRate(), WithinAbs (48000.0, 1e-9));  // default native EARS rate
    CHECK (s.outputBitDepth() == 24);
    CHECK (s.combineMode() == eb::CombineMode::AutoPerEar);   // recommended Dirac headphone mode by default
    CHECK_THAT (s.outputTrimDb(), WithinAbs (0.0, 1e-9));
    CHECK (s.firLength() == 0);   // 0 = Auto: taps scale with rate via numTapsForRate()
    CHECK (s.complexPhase() == false);
    dir.deleteRecursively();
}

TEST_CASE("Settings round-trips a non-default output bit depth") {
    // The DEPTH selector is a persisted *preference* (relabelled "PREFERRED DEPTH" in the UI;
    // WASAPI shared mode delivers float regardless). The schema/key/default must be untouched:
    // a fresh store reads 24, and a chosen 16 survives a write-through + reload.
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        CHECK (s.outputBitDepth() == 24);   // default unchanged by the relabel
        s.setOutputBitDepth (16);
        CHECK (s.outputBitDepth() == 16);   // in-memory round-trip
        s.flush();
    }
    eb::Settings reloaded (dir);            // re-open same folder -> reads back the file
    CHECK (reloaded.outputBitDepth() == 16);
    dir.deleteRecursively();
}

TEST_CASE("Settings persists update-check preferences", "[update]") {
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        CHECK (s.autoCheckUpdates() == true);   // default ON
        s.setAutoCheckUpdates (false);
        s.flush();
    }
    eb::Settings reloaded (dir);                 // re-open same folder -> reads back the file
    CHECK (reloaded.autoCheckUpdates() == false);
    dir.deleteRecursively();
}

TEST_CASE("Settings persists the advanced Dirac-path override") {
    // Opt-in advanced override for the combine-mode / output Start gates (#3).
    // Default OFF (the standard guarded Dirac path); a chosen ON survives a reload.
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        CHECK (s.advancedOverride() == false);   // default OFF
        s.setAdvancedOverride (true);
        CHECK (s.advancedOverride() == true);     // in-memory round-trip
        s.flush();
    }
    eb::Settings reloaded (dir);                  // re-open same folder -> reads back the file
    CHECK (reloaded.advancedOverride() == true);
    dir.deleteRecursively();
}

TEST_CASE("Settings rejects corrupt/stale stored values and falls back to defaults", "[validation]") {
    // A key PRESENT with a garbage value (a stale rate, a hand-edited bit depth, a combine-mode int
    // outside the enum, a NaN/huge trim, a negative FIR length) must NOT reach the engine -- the
    // getter validates the VALUE and returns the same default as the absent-key path.
    auto dir = makeTempDir();
    {
        auto raw = openBackingFile (dir);
        raw->setValue ("sampleRate",      12345.0);                 // not in the allowed rate set
        raw->setValue ("outputBitDepth",  7);                       // not 16/24/32
        raw->setValue ("combineMode",     99);                      // outside the CombineMode enum
        raw->setValue ("outputTrimDb",    1.0e9);                   // way past the +/-24 dB slider range
        raw->setValue ("firLength",       -5);                      // negative -> nonsense
        raw->saveIfNeeded();
    }
    eb::Settings s (dir);
    CHECK_THAT (s.sampleRate(),  WithinAbs (48000.0, 1e-9));        // default native EARS rate
    CHECK (s.outputBitDepth() == 24);                              // default
    CHECK (s.combineMode() == eb::CombineMode::AutoPerEar);        // default
    CHECK_THAT (s.outputTrimDb(), WithinAbs (0.0, 1e-9));          // default
    CHECK (s.firLength() == 0);                                    // default (Auto)
    dir.deleteRecursively();
}

TEST_CASE("Settings rejects a NaN output trim", "[validation]") {
    // A non-finite trim (NaN/inf) is the classic corrupt-float case: it would otherwise sail through
    // as a finite-looking but meaningless gain. It must fall back to 0.
    auto dir = makeTempDir();
    {
        auto raw = openBackingFile (dir);
        raw->setValue ("outputTrimDb", std::numeric_limits<double>::quiet_NaN());
        raw->saveIfNeeded();
    }
    eb::Settings s (dir);
    CHECK_THAT (s.outputTrimDb(), WithinAbs (0.0, 1e-9));
    dir.deleteRecursively();
}

TEST_CASE("Settings returns VALID stored values unchanged (no regression)", "[validation]") {
    // The validation must not touch the normal path: every in-range value reads back verbatim.
    auto dir = makeTempDir();
    {
        auto raw = openBackingFile (dir);
        raw->setValue ("sampleRate",      88200.0);                // a valid EARS Pro native rate
        raw->setValue ("outputBitDepth",  16);                     // valid
        raw->setValue ("combineMode",     (int) eb::CombineMode::Sum);
        raw->setValue ("outputTrimDb",    -12.5);                  // inside [-24, 0]
        raw->setValue ("firLength",       16384);                  // a valid explicit override
        raw->saveIfNeeded();
    }
    eb::Settings s (dir);
    CHECK_THAT (s.sampleRate(),  WithinAbs (88200.0, 1e-9));
    CHECK (s.outputBitDepth() == 16);
    CHECK (s.combineMode() == eb::CombineMode::Sum);
    CHECK_THAT (s.outputTrimDb(), WithinAbs (-12.5, 1e-9));
    CHECK (s.firLength() == 16384);
    dir.deleteRecursively();
}

TEST_CASE("Settings boundary values for the validated fields", "[validation]") {
    // Edge of each allowed range/set is INSIDE: lowest valid rate, both ends of the trim slider,
    // the lowest FIR override. (Just-outside is covered by the corrupt-value test above.)
    auto dir = makeTempDir();
    {
        auto raw = openBackingFile (dir);
        raw->setValue ("sampleRate",     44100.0);   // lowest allowed rate
        raw->setValue ("outputTrimDb",  -24.0);      // slider minimum -> valid
        raw->setValue ("firLength",      4096);      // lowest explicit override
        raw->saveIfNeeded();
    }
    eb::Settings lo (dir);
    CHECK_THAT (lo.sampleRate(), WithinAbs (44100.0, 1e-9));
    CHECK_THAT (lo.outputTrimDb(), WithinAbs (-24.0, 1e-9));
    CHECK (lo.firLength() == 4096);

    {
        auto raw = openBackingFile (dir);
        raw->setValue ("outputTrimDb", 0.0);         // slider maximum -> valid
        raw->saveIfNeeded();
    }
    eb::Settings hi (dir);
    CHECK_THAT (hi.outputTrimDb(), WithinAbs (0.0, 1e-9));
    dir.deleteRecursively();
}
