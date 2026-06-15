#include "gui/CurveThumbnail.h"
#include "gui/PlotMath.h"
#include "gui/Theme.h"
#include <algorithm>
#include <cmath>

namespace eb {

CurveThumbnail::CurveThumbnail() { setOpaque (false); }

void CurveThumbnail::clear() { curve.reset(); repaint(); }

void CurveThumbnail::setCalFile (const eb::CalFile& cal) {
    curve = cal;
    setRange (autoFitTopDb());
}

float CurveThumbnail::autoFitTopDb() const {
    // Symmetric range that comfortably contains the data, snapped to a 6 dB multiple
    // so 0 dB stays centred and the grid reads cleanly.
    if (! curve) return 6.0f;
    float lo = 0.0f, hi = 0.0f;
    for (auto& p : curve->points) {
        lo = std::min (lo, (float) p.splDb);
        hi = std::max (hi, (float) p.splDb);
    }
    const float mag = std::max ({ 6.0f, std::abs (lo), std::abs (hi) }) + 3.0f;
    return std::ceil (mag / 6.0f) * 6.0f;
}

void CurveThumbnail::setRange (float top) {
    topDb =  top;
    botDb = -top;
    repaint();
}

void CurveThumbnail::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Theme::plot());
    g.fillRoundedRectangle (r, 8.0f);

    const float W = r.getWidth(), H = r.getHeight();

    // Grid: decade verticals (100, 1k, 10k) + 0 dB horizontal.
    g.setColour (Theme::grid());
    for (float f : { 100.0f, 1000.0f, 10000.0f }) {
        const float x = r.getX() + freqToX (f, W);
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
    }
    const float zeroY = r.getY() + dbToY (0.0f, H, topDb, botDb);
    g.drawHorizontalLine ((int) zeroY, r.getX(), r.getRight());

    if (! curve || curve->points.size() < 2) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("no cal loaded", r, juce::Justification::centred);
        return;
    }

    // The cal curve.
    juce::Path path;
    bool started = false;
    for (auto& p : curve->points) {
        const float x = r.getX() + freqToX ((float) p.freqHz, W);
        const float y = r.getY() + dbToY ((float) p.splDb, H, topDb, botDb);
        if (! started) { path.startNewSubPath (x, y); started = true; }
        else            path.lineTo (x, y);
    }
    g.setColour (Theme::accent());
    g.strokePath (path, juce::PathStrokeType (1.5f));

    // dB axis label (top of the fitted range).
    g.setColour (Theme::axis());
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (juce::String ((int) topDb) + " dB",
                r.removeFromTop (13.0f), juce::Justification::topLeft);
}

} // namespace eb
