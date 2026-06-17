#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>   // juce::PropertiesFile / Options (NOT in juce_core)
#include "audio/EngineTypes.h"   // eb::EarsModel (Plan 2 SPINE)
#include "audio/CombineMode.h"   // eb::CombineMode (Plan 1)

namespace eb {

// Thin typed wrapper over a juce::PropertiesFile. Stores GUI/engine selections so
// the app restores its last configuration on launch. Constructed with an explicit
// settings folder (defaulting to the per-user app-data dir) so tests can use a temp
// dir. All getters fall back to sane defaults when a key is absent.
class Settings {
public:
    // dir == empty -> per-user application-data folder. Otherwise the given folder
    // (used by tests).
    explicit Settings (const juce::File& dir = {});

    juce::String inputKey()  const;   void setInputKey  (const juce::String&);
    juce::String outputKey() const;   void setOutputKey (const juce::String&);
    double       sampleRate() const;  void setSampleRate (double);
    int          outputBitDepth() const; void setOutputBitDepth (int);
    CombineMode  combineMode() const; void setCombineMode (CombineMode);
    juce::String leftCalPath()  const; void setLeftCalPath  (const juce::String&);
    juce::String rightCalPath() const; void setRightCalPath (const juce::String&);
    double       outputTrimDb() const; void setOutputTrimDb (double);
    int          firLength() const;   void setFirLength (int);   // 0 = Auto (numTapsForRate); else explicit override
    bool         complexPhase() const; void setComplexPhase (bool);
    bool         autoCheckUpdates() const; void setAutoCheckUpdates (bool);

    void flush();   // force the PropertiesFile to disk immediately

private:
    juce::PropertiesFile& props();
    std::unique_ptr<juce::PropertiesFile> file;
};

} // namespace eb
