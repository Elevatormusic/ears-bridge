#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include "gui/SystemA11y.h"
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
    const bool snap = SystemA11y::reduceMotion();   // HIG Reduce Motion: snap to the value instead of easing
    // Bar: fast attack, slow release (the visual bar should still show peaks); snap when Reduce Motion is on.
    level = snap ? peakLinear : juce::jmax (peakLinear, level * 0.80f);
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
    smoothDb = snap ? instDb : (smoothDb + (instDb - smoothDb) * 0.10f);

    // Expose the readout to assistive tech (VoiceOver); only refresh when the announced value changes.
    const int db = juce::roundToInt (smoothDb);
    const juce::String desc = clipLatched ? "Clipping"
                            : (db <= -60)  ? "below -60 decibels"
                                           : juce::String (db) + " decibels";
    if (desc != lastDesc) { lastDesc = desc; setDescription (desc); }

    repaint();
}

juce::String LevelMeter::tagFor (float db, bool clip, bool isOut) {
    // Order is the honesty ladder: a latched clip beats everything (a clipped Out still says clip);
    // the Out meter is a routing readout, not a target to chase; then the capture-band words.
    if (clip)  return "clip";
    if (isOut) return "to Dirac";
    if (db >= kTargetLoDb && db <= kTargetHiDb) return "in band";
    if (db < -60.0f)       return "idle";
    if (db < kTargetLoDb)  return "low";
    return "hot";
}

void LevelMeter::paint (juce::Graphics& g) {
    // P3 W2 meter row (spec §5.3 [P3-refresh]): caption 34 | rounded 12px track (+ target band) |
    // dB readout 56 | worded tag 52. View restyle only - the level/clip model is setLevel's, untouched.
    auto r = getLocalBounds().toFloat();

    auto lab = r.removeFromLeft (34.0f);
    g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    if (active_) {
        // AutoPerEar "live" indicator: an accent DOT (a graphical object) before the label marks the
        // earcup currently fed to Dirac. The label text stays the readable primary colour -- the accent
        // is a fill, not low-contrast text. Drawn inside the 34 px caption column so the track width
        // never shifts when the active side toggles.
        g.setColour (Theme::accent());
        g.fillEllipse (lab.getX(), lab.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setColour (Theme::text());
        g.drawText (label, lab.withTrimmedLeft (11.0f), juce::Justification::centredLeft);
    } else {
        g.setColour (Theme::textDim());
        g.drawText (label, lab, juce::Justification::centredLeft);
    }

    auto tagBox = r.removeFromRight (52.0f);
    r.removeFromRight (6.0f);
    auto dbBox = r.removeFromRight (56.0f);
    r.removeFromRight (8.0f);

    auto track = r.withSizeKeepingCentre (r.getWidth(), 12.0f);
    const float W = track.getWidth();
    g.setColour (Theme::track());
    g.fillRoundedRectangle (track, 6.0f);

    // Green "aim here" target band (-18..-12 dBFS) on capture meters: gives the user a level to set
    // the amp TO, instead of only flagging clipping. A translucent fill (not text), so the soft tint
    // carries no contrast requirement; the level bar overlays it when the capture is in range.
    if (showTargetBand) {
        const float loF = dbToFrac (kTargetLoDb);
        const float hiF = dbToFrac (kTargetHiDb);
        auto bandR = juce::Rectangle<float> (track.getX() + loF * W, track.getY(),
                                             (hiF - loF) * W, track.getHeight());
        // Faint fill + crisp 1px edges, so the "aim here" zone stays legible even when the (also
        // green) level bar sits inside it -- a flat fill alone reads as one green block under the bar.
        g.setColour (Theme::okFill().withAlpha (0.14f));
        g.fillRect (bandR);
        g.setColour (Theme::okFill().withAlpha (0.4f));
        g.fillRect (bandR.getX(),            bandR.getY(), 1.0f, bandR.getHeight());
        g.fillRect (bandR.getRight() - 1.0f, bandR.getY(), 1.0f, bandR.getHeight());
    }
    // (The old amber/red full-scale zone stripes died in the W2 restyle: the worded tag now carries
    //  the hot/clip state, so the track keeps only the target band.)

    const float frac = linearToFrac (level);
    if (frac > 0.002f) {
        // Rounded-LEFT bar (W2): the leading edge stays square so the bar reads as a level, not a pill.
        auto fill = track.withWidth (juce::jmax (6.0f, frac * W));
        juce::Colour c = (clipLatched || frac > 0.95f) ? Theme::dangerFill()
                       : (frac > 0.92f)                ? Theme::warnFill()
                                                       : Theme::okFill();
        g.setColour (c);
        juce::Path p;
        p.addRoundedRectangle (fill.getX(), fill.getY(), fill.getWidth(), fill.getHeight(),
                               6.0f, 6.0f, true, false, true, false);
        g.fillPath (p);
    }

    // dB readout: the smoothed number (the worded state moved to the tag column).
    g.setColour (clipLatched ? Theme::danger() : Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const juce::String txt = (smoothDb <= -60.0f) ? juce::String ("-")
                                                  : "-" + juce::String (juce::roundToInt (-smoothDb)) + " dB";
    g.drawText (txt, dbBox, juce::Justification::centredRight);

    // Worded status tag (never colour alone). g.drawText inside LevelMeter is acceptable ONLY because
    // the meter is never a displaced-warn surface: it sits above the fold in both consumers (Level +
    // Measure), and the no-scroll fit gates pin that. Do not copy this exemption to a warn label.
    const juce::String tag = tagFor (smoothDb, clipLatched, ! showTargetBand);
    g.setColour (tag == "in band"                 ? Theme::ok()
               : (tag == "low" || tag == "hot")   ? Theme::warn()
               : tag == "clip"                    ? Theme::danger()
                                                  : Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle (tag == "clip" ? "Bold" : "Regular")));
    g.drawText (tag, tagBox, juce::Justification::centredLeft);
}

} // namespace eb
