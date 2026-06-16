#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

LevelMeter::LevelMeter (juce::String caption) : label (std::move (caption)) {
    setOpaque (false);
}

float LevelMeter::dbToFrac (float db) {
    // Display window: -60 dBFS (left) .. 0 dBFS (right).
    constexpr float floorDb = -60.0f;
    return juce::jlimit (0.0f, 1.0f, (db - floorDb) / (0.0f - floorDb));
}

float LevelMeter::linearToFrac (float linear) {
    constexpr float floorDb = -60.0f;
    const float db = (linear <= 1.0e-6f) ? floorDb : 20.0f * std::log10 (linear);
    return dbToFrac (db);
}

void LevelMeter::setLevel (float peakLinear, bool clip) {
    // Bar: fast attack, slow release (the visual bar should still show peaks).
    level = juce::jmax (peakLinear, level * 0.80f);
    // Clip latch with a timed auto-release: a clip arms the latch and (re)starts a ~1.5 s hold so
    // the user sees it; once no further clip arrives for the hold window it clears on its own.
    // Without this it only cleared on full silence, so a single transient clip left the readout
    // stuck on "CLIP" and the bar stuck red for the whole session. Silence still clears instantly.
    if (clip) { clipLatched = true; clipHold = kClipHoldTicks; }
    else if (clipHold > 0 && --clipHold == 0) clipLatched = false;
    if (level <= 1.0e-6f) { clipLatched = false; clipHold = 0; }   // instant clear on stop / silence

    // Readout: a SEPARATE, slowly-smoothed dB so the printed number doesn't chase peaks and
    // flicker — roughly a 0.3 s time constant at the 30 Hz GUI tick. The bar stays responsive.
    const float instDb = (level <= 1.0e-6f) ? -120.0f : 20.0f * std::log10 (level);
    smoothDb += (instDb - smoothDb) * 0.10f;

    // Expose the readout to assistive tech (VoiceOver); only refresh when the announced value changes.
    const int db = juce::roundToInt (smoothDb);
    const juce::String desc = clipLatched ? "Clipping"
                            : (db <= -60)  ? "below -60 decibels"
                                           : juce::String (db) + " decibels";
    if (desc != lastDesc) { lastDesc = desc; setDescription (desc); }

    repaint();
}

void LevelMeter::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();

    auto lab = r.removeFromLeft (26.0f);
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    if (active_) {
        // AutoPerEar "live" indicator: an accent dot (shape, not colour alone) before the label, both
        // in the accent colour, marking the earcup currently being fed to Dirac. Drawn inside the
        // 26 px label column so the track width never shifts when the active side toggles.
        g.setColour (Theme::accent());
        g.fillEllipse (lab.getX(), lab.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.drawText (label, lab.withTrimmedLeft (11.0f), juce::Justification::centredLeft);
    } else {
        g.setColour (Theme::textDim());
        g.drawText (label, lab, juce::Justification::centredLeft);
    }

    auto dbBox = r.removeFromRight (52.0f);
    r.removeFromRight (8.0f);

    auto track = r.withSizeKeepingCentre (r.getWidth(), 8.0f);
    const float W = track.getWidth();
    g.setColour (Theme::track());
    g.fillRoundedRectangle (track, 4.0f);

    // Green "aim here" target band (-18..-12 dBFS) on capture meters: gives the user a level to set
    // the amp TO, instead of only flagging clipping. A translucent fill (not text), so the soft tint
    // carries no contrast requirement; the level bar overlays it when the capture is in range.
    if (showTargetBand) {
        const float loF = dbToFrac (kTargetLoDb);
        const float hiF = dbToFrac (kTargetHiDb);
        auto bandR = juce::Rectangle<float> (track.getX() + loF * W, track.getY(),
                                             (hiF - loF) * W, track.getHeight());
        // Faint fill + crisp edges, so the "aim here" zone stays legible even when the (also green)
        // level bar sits inside it -- a flat fill alone reads as one green block under the bar.
        g.setColour (Theme::ok().withAlpha (0.16f));
        g.fillRect (bandR);
        g.setColour (Theme::ok().withAlpha (0.85f));
        g.fillRect (bandR.getX(),            bandR.getY(), 1.0f, bandR.getHeight());
        g.fillRect (bandR.getRight() - 1.0f, bandR.getY(), 1.0f, bandR.getHeight());
    }

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

    // Readout: a literal "CLIP" tag (not colour alone) latches on overload, else the smoothed dB.
    g.setColour (clipLatched ? Theme::danger() : Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle (clipLatched ? "Bold" : "Regular")));
    juce::String txt = clipLatched          ? juce::String ("CLIP")
                     : (smoothDb <= -60.0f) ? juce::String ("-")
                                            : "-" + juce::String (juce::roundToInt (-smoothDb)) + " dB";
    g.drawText (txt, dbBox, juce::Justification::centredRight);
}

} // namespace eb
