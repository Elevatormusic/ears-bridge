#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

LevelMeter::LevelMeter (juce::String caption) : label (std::move (caption)) {
    setOpaque (false);
}

float LevelMeter::linearToFrac (float linear) {
    // Display window: -60 dBFS (left) .. 0 dBFS (right).
    constexpr float floorDb = -60.0f;
    const float db = (linear <= 1.0e-6f) ? floorDb : 20.0f * std::log10 (linear);
    return juce::jlimit (0.0f, 1.0f, (db - floorDb) / (0.0f - floorDb));
}

void LevelMeter::setLevel (float peakLinear, bool clip) {
    // Fast attack, slow release for a readable bar.
    level = juce::jmax (peakLinear, level * 0.80f);
    if (clip) clipLatched = true;
    if (level <= 1.0e-6f) clipLatched = false;   // clears once the signal is gone (stopped)

    // Expose the readout to assistive tech (VoiceOver), so the dB / clip state isn't
    // visual-only. Only refresh the accessible description when the announced value changes.
    const int db = (level <= 1.0e-5f) ? -120 : juce::roundToInt (20.0f * std::log10 (level));
    const juce::String desc = clipLatched ? "Clipping"
                            : (db <= -60)  ? "below -60 decibels"
                                           : juce::String (db) + " decibels";
    if (desc != lastDesc) { lastDesc = desc; setDescription (desc); }

    repaint();
}

void LevelMeter::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();

    auto lab = r.removeFromLeft (26.0f);
    g.setColour (Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    g.drawText (label, lab, juce::Justification::centredLeft);

    auto dbBox = r.removeFromRight (52.0f);
    r.removeFromRight (8.0f);

    auto track = r.withSizeKeepingCentre (r.getWidth(), 8.0f);
    const float W = track.getWidth();
    g.setColour (Theme::track());
    g.fillRoundedRectangle (track, 4.0f);

    // Zones near full scale: amber from -1.0 dB-ish, red at the very top.
    g.setColour (Theme::warn().withAlpha (0.65f));
    g.fillRect (juce::Rectangle<float> (track.getRight() - W * 0.050f, track.getY(),
                                        W * 0.034f, track.getHeight()));
    g.setColour (Theme::danger().withAlpha (0.85f));
    g.fillRect (juce::Rectangle<float> (track.getRight() - W * 0.016f, track.getY(),
                                        W * 0.016f, track.getHeight()));

    const float frac = linearToFrac (level);
    if (frac > 0.002f) {
        auto fill = track.withWidth (juce::jmax (4.0f, frac * W));
        juce::Colour c = (clipLatched || frac > 0.95f) ? Theme::danger()
                       : (frac > 0.92f)                ? Theme::warn()
                                                       : Theme::ok();
        g.setColour (c);
        g.fillRoundedRectangle (fill, 4.0f);
    }

    // Readout: a literal "CLIP" tag (not colour alone) latches on overload, else the dB level.
    const float db = (level <= 1.0e-5f) ? -120.0f : 20.0f * std::log10 (level);
    g.setColour (clipLatched ? Theme::danger() : Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle (clipLatched ? "Bold" : "Regular")));
    juce::String txt = clipLatched     ? juce::String ("CLIP")
                     : (db <= -60.0f)  ? juce::String ("-")
                                       : "-" + juce::String ((int) std::round (-db)) + " dB";
    g.drawText (txt, dbBox, juce::Justification::centredRight);
}

} // namespace eb
