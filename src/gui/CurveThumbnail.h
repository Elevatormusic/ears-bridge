#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "cal/CalFile.h"
#include <optional>

namespace eb {

// Static FR plot of a parsed CalFile: log-x 20 Hz..20 kHz, linear-dB y. Draws the
// raw cal curve (as loaded) so the user can eyeball the EARS canal resonance bump.
// Empty (placeholder) until setCalFile() is given a curve.
class CurveThumbnail : public juce::Component {
public:
    CurveThumbnail();
    void setCalFile (const eb::CalFile& cal);
    void clear();

    // The symmetric dB range this curve would auto-fit to (so a parent can pick one
    // shared scale across both ear plots), and an override that locks the axis to it.
    float autoFitTopDb() const;
    void  setRange (float topDb);

    void paint (juce::Graphics&) override;

    // M5/L1 (spec §10): the plot exposes the image a11y role + a live description.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    std::optional<eb::CalFile> curve;
    float topDb = 24.0f, botDb = -24.0f;   // auto-fit to the data on setCalFile
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveThumbnail)
};

} // namespace eb
