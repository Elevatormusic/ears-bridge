#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

LevelMeter::LevelMeter (juce::String caption) : label (std::move (caption)) {
    setOpaque (false);
}

float LevelMeter::linearToFrac (float linear) {
    // Display window: -60 dBFS (bottom) .. 0 dBFS (top).
    constexpr float floorDb = -60.0f;
    const float db = (linear <= 1.0e-6f) ? floorDb : 20.0f * std::log10 (linear);
    const float t  = juce::jlimit (0.0f, 1.0f, (db - floorDb) / (0.0f - floorDb));
    return t;
}

void LevelMeter::setLevel (float peakLinear, bool clip) {
    // Fast attack, slow release for a readable bar.
    level = juce::jmax (peakLinear, level * 0.80f);
    peakHold = juce::jmax (peakLinear, peakHold * 0.95f);
    if (clip) clipLatched = true;        // latches until a manual repaint cycle clears it
    repaint();
}

void LevelMeter::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    // Caption + clip LED strip at the top.
    auto ledArea = r.removeFromTop (10.0f);
    g.setColour (clipLatched ? Theme::danger() : Theme::outline());
    g.fillRoundedRectangle (ledArea.removeFromRight (10.0f), 2.0f);
    if (label.isNotEmpty()) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (label, ledArea, juce::Justification::centredLeft);
    }

    r.removeFromTop (2.0f);
    // Track.
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (r, 3.0f);

    // Filled portion (bottom-up).
    const float frac = linearToFrac (level);
    auto fill = r.withTop (r.getBottom() - frac * r.getHeight());
    juce::ColourGradient grad (Theme::meterLo(), fill.getX(), fill.getBottom(),
                               Theme::meterHi(), fill.getX(), r.getY(), false);
    grad.addColour (0.7, Theme::meterMid());
    g.setGradientFill (grad);
    g.fillRoundedRectangle (fill, 3.0f);

    // Peak-hold tick.
    const float py = r.getBottom() - linearToFrac (peakHold) * r.getHeight();
    g.setColour (Theme::text());
    g.fillRect (r.getX(), py - 1.0f, r.getWidth(), 2.0f);

    g.setColour (Theme::outline());
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
}

} // namespace eb
