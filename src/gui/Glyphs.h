#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// P2.9 icon language - the app's ONLY glyph source (W2 / SF-symbol feel). Every recipe is authored
// in the UNIT BOX (0..1 x 0..1) and drawn into a caller-supplied box (uniform scale). Stroke widths
// are fractions of the box edge. Coordinates transcribed from docs/gui-redesign/redesign-W2-spine.html
// where the frame shows the glyph (play/tick/info/refresh); the rest are authored in the same
// language (thin rounded strokes, geometric). DO NOT redraw by eye - edit coordinates only.
namespace eb::glyph {

namespace detail {
inline juce::AffineTransform unitTo (juce::Rectangle<float> box) {
    return juce::AffineTransform::scale (box.getWidth(), box.getHeight())
                                 .translated (box.getX(), box.getY());
}
inline void stroke (juce::Graphics& g, const juce::Path& unitPath, juce::Rectangle<float> box,
                    juce::Colour c, float unitThickness) {
    g.setColour (c);
    g.strokePath (unitPath, juce::PathStrokeType (unitThickness * box.getWidth(),
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded),
                  unitTo (box));
}
inline void fill (juce::Graphics& g, const juce::Path& unitPath, juce::Rectangle<float> box, juce::Colour c) {
    g.setColour (c);
    g.fillPath (unitPath, unitTo (box));
}
} // namespace detail

// W2 .btn.primary play (viewBox 14: M3 2.5 L11.5 7 L3 11.5 Z) - FILLED.
inline void drawPlay (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.startNewSubPath (0.214f, 0.179f);
    p.lineTo (0.821f, 0.500f);
    p.lineTo (0.214f, 0.821f);
    p.closeSubPath();
    detail::fill (g, p, box, c);
}

// Stop square (companion state of play) - FILLED rounded square.
inline void drawStop (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.addRoundedRectangle (0.21f, 0.21f, 0.58f, 0.58f, 0.10f);
    detail::fill (g, p, box, c);
}

// W2 tick (viewBox 13: M2.5 6.8 L5 9.3 L10.5 3.5, stroke 2) - STROKE 0.154.
inline void drawTick (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.startNewSubPath (0.192f, 0.523f);
    p.lineTo (0.385f, 0.715f);
    p.lineTo (0.808f, 0.269f);
    detail::stroke (g, p, box, c, 0.154f);
}

// W2 info-circle (viewBox 15: circle r6.6 stroke 1.3 + dot c(7.5,4.6) r1 + stem rect 1.6x4.6 r0.8).
inline void drawInfo (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path ring;  ring.addEllipse (0.060f, 0.060f, 0.880f, 0.880f);
    detail::stroke (g, ring, box, c, 0.087f);
    juce::Path dots;
    dots.addEllipse (0.433f, 0.240f, 0.133f, 0.133f);
    dots.addRoundedRectangle (0.447f, 0.440f, 0.107f, 0.307f, 0.053f);
    detail::fill (g, dots, box, c);
}

// W2 re-run refresh (viewBox 14: 328-degree arc r4.6 + arrow head at the gap) - STROKE 0.114.
// JUCE angles: radians CLOCKWISE from 12 o'clock; from 1.40 down to -4.32 = the long counter-clockwise arc,
// leaving the gap on the upper right where the arrow head sits (head points transcribed from the frame).
inline void drawRefresh (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.addCentredArc (0.5f, 0.5f, 0.33f, 0.33f, 0.0f, 1.40f, -4.32f, true);
    p.startNewSubPath (0.843f, 0.214f);
    p.lineTo (0.829f, 0.457f);
    p.lineTo (0.593f, 0.429f);
    detail::stroke (g, p, box, c, 0.114f);
}

// Folder (Open log folder) - tab + body outline, STROKE 0.11.
inline void drawFolder (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.startNewSubPath (0.12f, 0.78f);
    p.lineTo (0.12f, 0.24f);
    p.lineTo (0.40f, 0.24f);
    p.lineTo (0.48f, 0.34f);
    p.lineTo (0.88f, 0.34f);
    p.lineTo (0.88f, 0.78f);
    p.closeSubPath();
    detail::stroke (g, p, box, c, 0.11f);
}

// Export / share (Export log...) - square.and.arrow.up, STROKE 0.11.
inline void drawExport (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path p;
    p.startNewSubPath (0.34f, 0.40f); p.lineTo (0.16f, 0.40f); p.lineTo (0.16f, 0.90f);
    p.lineTo (0.84f, 0.90f); p.lineTo (0.84f, 0.40f); p.lineTo (0.66f, 0.40f);
    p.startNewSubPath (0.50f, 0.66f); p.lineTo (0.50f, 0.10f);
    p.startNewSubPath (0.32f, 0.26f); p.lineTo (0.50f, 0.08f); p.lineTo (0.68f, 0.26f);
    detail::stroke (g, p, box, c, 0.11f);
}

// Warning triangle (regression banner) - outline triangle + filled exclamation.
inline void drawWarning (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) {
    juce::Path tri;
    tri.startNewSubPath (0.50f, 0.10f);
    tri.lineTo (0.94f, 0.86f);
    tri.lineTo (0.06f, 0.86f);
    tri.closeSubPath();
    detail::stroke (g, tri, box, c, 0.11f);
    juce::Path marks;
    marks.addRoundedRectangle (0.455f, 0.36f, 0.09f, 0.26f, 0.045f);
    marks.addEllipse (0.455f, 0.68f, 0.09f, 0.09f);
    detail::fill (g, marks, box, c);
}

} // namespace eb::glyph
