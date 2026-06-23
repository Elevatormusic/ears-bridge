#include "state/Settings.h"
#include <cmath>

namespace eb {

namespace {
    constexpr const char* kInputKey   = "inputKey";
    constexpr const char* kOutputKey  = "outputKey";
    constexpr const char* kRate       = "sampleRate";
    constexpr const char* kBits       = "outputBitDepth";
    constexpr const char* kCombine    = "combineMode";
    constexpr const char* kLeftCal    = "leftCalPath";
    constexpr const char* kRightCal   = "rightCalPath";
    constexpr const char* kTrim       = "outputTrimDb";
    constexpr const char* kFirLen     = "firLength";
    constexpr const char* kComplex    = "complexPhase";
    constexpr const char* kAutoCheck  = "autoCheckUpdates";
    constexpr const char* kAdvOverride = "advancedOverride";
    constexpr const char* kDiracHwProc = "diracHardwareProcessor";

    // ---- Field-value validation (Plan: harden settings load) ----------------------------
    // A key PRESENT with a garbage value (a stale rate from an old version, a hand-edited
    // outputBitDepth=7, a combine-mode int outside the enum, a NaN/huge trim) must NOT reach the
    // engine. Each getter validates the stored VALUE against its allowed set/range and, when it's
    // invalid, returns the SAME default the absent-key path uses. We never rewrite the file here --
    // a later setter persists a corrected value naturally. (Bools via getBoolValue are total; key /
    // cal-path strings are validated downstream by device/cal load, so they're left as-is.)

    // Canonical allowed sample-rate set: the EARS Pro native rates (ModelDetect::nativeSampleRates).
    // EARS-only devices expose only {48000}; the wrapper can't see the device, so accept the full
    // standard set and let the rate menu flag a non-native pick. Default 48000.
    constexpr double kDefaultRate = 48000.0;
    inline bool isAllowedRate (double sr) {
        for (double r : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
            if (std::abs (sr - r) < 0.5) return true;
        return false;
    }

    // Output bit depth: 16/24/32 (the engine relabels this a "preferred" depth). Default 24.
    constexpr int kDefaultBits = 24;
    inline bool isAllowedBits (int b) { return b == 16 || b == 24 || b == 32; }

    // FIR length override: 0 = Auto (numTapsForRate), else one of the menu's explicit power-of-two
    // overrides. Default 0 (Auto).
    constexpr int kDefaultFirLen = 0;
    inline bool isAllowedFirLen (int n) {
        return n == 0 || n == 4096 || n == 8192 || n == 16384 || n == 32768;
    }

    // Output trim: the trim slider's range is [-24, 0] dB (MainComponent). Must be finite and in
    // range. Default 0.
    constexpr double kDefaultTrim   = 0.0;
    constexpr double kTrimMinDb     = -24.0;
    constexpr double kTrimMaxDb     = 0.0;
    inline bool isAllowedTrim (double db) {
        return std::isfinite (db) && db >= kTrimMinDb && db <= kTrimMaxDb;
    }
}

Settings::Settings (const juce::File& dir) {
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "EarsBridge";
    opts.filenameSuffix      = ".settings";
    opts.folderName          = "EarsBridge";
    opts.osxLibrarySubFolder = "Application Support";
    opts.storageFormat       = juce::PropertiesFile::storeAsXML;
    opts.millisecondsBeforeSaving = 0;   // write on every change

    auto target = dir == juce::File()
        ? opts.getDefaultFile()
        : dir.getChildFile ("EarsBridge.settings");
    file = std::make_unique<juce::PropertiesFile> (target, opts);
}

juce::PropertiesFile& Settings::props() { return *file; }

juce::String Settings::inputKey()  const { return file->getValue (kInputKey); }
void Settings::setInputKey (const juce::String& v) { file->setValue (kInputKey, v); }

juce::String Settings::outputKey() const { return file->getValue (kOutputKey); }
void Settings::setOutputKey (const juce::String& v) { file->setValue (kOutputKey, v); }

double Settings::sampleRate() const {
    // Validate the stored VALUE: a stale rate from an old version (or a hand-edited one) outside the
    // allowed set falls back to the default, not verbatim through to the engine.
    const double sr = file->getDoubleValue (kRate, kDefaultRate);
    return isAllowedRate (sr) ? sr : kDefaultRate;
}
void Settings::setSampleRate (double sr) { file->setValue (kRate, sr); }

int Settings::outputBitDepth() const {
    const int b = file->getIntValue (kBits, kDefaultBits);
    return isAllowedBits (b) ? b : kDefaultBits;   // 16/24/32 else default 24
}
void Settings::setOutputBitDepth (int b) { file->setValue (kBits, b); }

CombineMode Settings::combineMode() const {
    // Default to AutoPerEar: the recommended Dirac headphone mode (one routine, both ears). A fresh
    // install should start in it. Validate the stored int is a real enumerator -- a value outside
    // [LeftOnly..AutoPerEar] (e.g. a hand-edited 99) falls back to the default.
    const int v = file->getIntValue (kCombine, (int) CombineMode::AutoPerEar);
    if (v >= (int) CombineMode::LeftOnly && v <= (int) CombineMode::AutoPerEar)
        return static_cast<CombineMode> (v);
    return CombineMode::AutoPerEar;
}
void Settings::setCombineMode (CombineMode m) { file->setValue (kCombine, (int) m); }

juce::String Settings::leftCalPath()  const { return file->getValue (kLeftCal); }
void Settings::setLeftCalPath (const juce::String& v) { file->setValue (kLeftCal, v); }

juce::String Settings::rightCalPath() const { return file->getValue (kRightCal); }
void Settings::setRightCalPath (const juce::String& v) { file->setValue (kRightCal, v); }

double Settings::outputTrimDb() const {
    // Must be finite AND within the trim slider's range [-24, 0] dB. A NaN/inf or out-of-range value
    // (e.g. a hand-edited 1e9) falls back to 0, never reaching the output-gain stage.
    const double db = file->getDoubleValue (kTrim, kDefaultTrim);
    return isAllowedTrim (db) ? db : kDefaultTrim;
}
void Settings::setOutputTrimDb (double db) { file->setValue (kTrim, db); }

// Default 0 = "Auto": use numTapsForRate(activeRate) so taps scale with the sample rate.
// A non-zero value is an explicit user override (4096/8192/16384/32768). A stored value outside
// {0,4096,8192,16384,32768} (e.g. a negative or stray length) falls back to 0 (Auto).
int Settings::firLength() const {
    const int n = file->getIntValue (kFirLen, kDefaultFirLen);
    return isAllowedFirLen (n) ? n : kDefaultFirLen;
}
void Settings::setFirLength (int n) { file->setValue (kFirLen, n); }

bool Settings::complexPhase() const { return file->getBoolValue (kComplex, false); }
void Settings::setComplexPhase (bool b) { file->setValue (kComplex, b); }

bool Settings::autoCheckUpdates() const { return file->getBoolValue (kAutoCheck, true); }
void Settings::setAutoCheckUpdates (bool b) { file->setValue (kAutoCheck, b); }

// Opt-in advanced override for the combine-mode / output Start gates (#3). Default OFF: a
// fresh install enforces the standard guarded Dirac path. An advanced user must expand
// Advanced AND tick the toggle to relax the two policy gates (never devices/calibration).
bool Settings::advancedOverride() const { return file->getBoolValue (kAdvOverride, false); }
void Settings::setAdvancedOverride (bool b) { file->setValue (kAdvOverride, b); }
bool Settings::diracHardwareProcessor() const { return file->getBoolValue (kDiracHwProc, false); }
void Settings::setDiracHardwareProcessor (bool b) { file->setValue (kDiracHwProc, b); }

void Settings::flush() { file->saveIfNeeded(); }

} // namespace eb
