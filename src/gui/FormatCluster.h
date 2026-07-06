#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/CombineMode.h"
#include "gui/Theme.h"
#include <array>

namespace eb {

// P2.9: the W2 title-bar format cluster ("48 kHz . 24-bit . Auto per-ear"). Pure text builder +
// a paint-only component (3px dot separators are PAINTED - the ASCII-copy rule bans a middot char).
inline std::array<juce::String, 3> formatClusterParts (double sampleRate, int bitDepth, CombineMode mode) {
    const double khz = sampleRate / 1000.0;
    juce::String rate = (khz == (double) (int) khz) ? juce::String ((int) khz) : juce::String (khz, 1);
    juce::String modeName;
    switch (mode) {
        case CombineMode::LeftOnly:   modeName = "Left ear";     break;
        case CombineMode::RightOnly:  modeName = "Right ear";    break;
        case CombineMode::Average:    modeName = "Average";      break;
        case CombineMode::Sum:        modeName = "Sum";          break;
        case CombineMode::AutoPerEar: modeName = "Auto per-ear"; break;
    }
    return { rate + " kHz", juce::String (bitDepth) + "-bit", modeName };
}

class FormatCluster : public juce::Component {
public:
    FormatCluster() {
        setInterceptsMouseClicks (false, false);
        setTitle ("Session format");            // a11y name for the painted cluster
    }
    void setParts (const std::array<juce::String, 3>& p) {
        if (p == parts_) return;                // set-if-changed (called every refreshWizardView tick)
        parts_ = p;
        markDirty();
    }

    // Repaint discipline (M-2): the count of repaints issued. setParts runs on every refreshWizardView
    // tick, so an identical re-set must leave this UNCHANGED; a changed set must increment it. Mirrors
    // WizardSpine's rowRepaintCountForTest markDirty() seam.
    int repaintCountForTest() const { return repaintCount_; }
    void paint (juce::Graphics& g) override {
        const juce::Font f (juce::FontOptions (12.0f));
        g.setFont (f);
        float x = (float) getWidth();           // right-aligned: draw parts from the right edge inward
        const float cy = (float) getHeight() * 0.5f;
        for (int i = 2; i >= 0; --i) {
            const float w = juce::GlyphArrangement::getStringWidth (f, parts_[(size_t) i]);
            x -= w;
            g.setColour (Theme::textDim());
            g.drawText (parts_[(size_t) i], juce::Rectangle<float> (x, 0.0f, w, (float) getHeight()),
                        juce::Justification::centredLeft, false);
            if (i > 0) {                         // W2 .fmt .dot: 3px circle, 8px gaps
                x -= 8.0f + 3.0f;
                g.setColour (Theme::textFaint());
                g.fillEllipse (x, cy - 1.5f, 3.0f, 3.0f);
                x -= 8.0f;
            }
        }
    }
private:
    void markDirty() { ++repaintCount_; repaint(); }   // every repaint funnels here so the guard is observable

    std::array<juce::String, 3> parts_ {};
    int repaintCount_ = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FormatCluster)
};

} // namespace eb
