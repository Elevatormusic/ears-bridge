#include "state/Settings.h"

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

double Settings::sampleRate() const { return file->getDoubleValue (kRate, 48000.0); }
void Settings::setSampleRate (double sr) { file->setValue (kRate, sr); }

int Settings::outputBitDepth() const { return file->getIntValue (kBits, 24); }
void Settings::setOutputBitDepth (int b) { file->setValue (kBits, b); }

CombineMode Settings::combineMode() const {
    // Default to AutoPerEar: the recommended Dirac headphone mode (one routine, both ears). A fresh
    // install should start in it.
    return static_cast<CombineMode> (file->getIntValue (kCombine, (int) CombineMode::AutoPerEar));
}
void Settings::setCombineMode (CombineMode m) { file->setValue (kCombine, (int) m); }

juce::String Settings::leftCalPath()  const { return file->getValue (kLeftCal); }
void Settings::setLeftCalPath (const juce::String& v) { file->setValue (kLeftCal, v); }

juce::String Settings::rightCalPath() const { return file->getValue (kRightCal); }
void Settings::setRightCalPath (const juce::String& v) { file->setValue (kRightCal, v); }

double Settings::outputTrimDb() const { return file->getDoubleValue (kTrim, 0.0); }
void Settings::setOutputTrimDb (double db) { file->setValue (kTrim, db); }

// Default 0 = "Auto": use numTapsForRate(activeRate) so taps scale with the sample rate.
// A non-zero value is an explicit user override (4096/8192/16384/32768).
int Settings::firLength() const { return file->getIntValue (kFirLen, 0); }
void Settings::setFirLength (int n) { file->setValue (kFirLen, n); }

bool Settings::complexPhase() const { return file->getBoolValue (kComplex, false); }
void Settings::setComplexPhase (bool b) { file->setValue (kComplex, b); }

bool Settings::autoCheckUpdates() const { return file->getBoolValue (kAutoCheck, true); }
void Settings::setAutoCheckUpdates (bool b) { file->setValue (kAutoCheck, b); }

void Settings::flush() { file->saveIfNeeded(); }

} // namespace eb
