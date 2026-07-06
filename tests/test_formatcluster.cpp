#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/FormatCluster.h"
#include "audio/CombineMode.h"

using eb::FormatCluster;
using eb::CombineMode;
using eb::formatClusterParts;

// The pure text builder: the three parts a session format renders (rate . bit-depth . mode).
TEST_CASE("formatClusterParts builds the three-part cluster from rate/depth/mode") {
    const auto p = formatClusterParts (48000.0, 24, CombineMode::AutoPerEar);
    CHECK (p[0] == "48 kHz");
    CHECK (p[1] == "24-bit");
    CHECK (p[2] == "Auto per-ear");

    // Fractional rate keeps one decimal; whole-kHz rate is integer-formatted (no trailing ".0").
    CHECK (formatClusterParts (44100.0, 16, CombineMode::Average)[0] == "44.1 kHz");
    CHECK (formatClusterParts (96000.0, 32, CombineMode::Sum)[0]     == "96 kHz");
}

// M-2: setParts runs on EVERY refreshWizardView tick (30 Hz). The set-if-changed guard
// (`if (p == parts_) return;`) exists so an identical re-set does NOT re-invalidate the painted
// cluster forever. Pin it: an identical second setParts must leave the repaint count UNCHANGED; a
// genuinely changed set must increment it. (Mirrors WizardSpine's M-1 repaint-discipline test.)
TEST_CASE("FormatCluster: an identical setParts does not repaint (M-2)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    FormatCluster cluster;
    cluster.setSize (200, 24);

    const auto a = formatClusterParts (48000.0, 24, CombineMode::AutoPerEar);

    // First set establishes the state and issues one repaint.
    cluster.setParts (a);
    const int afterFirst = cluster.repaintCountForTest();
    CHECK (afterFirst > 0);                                   // the change from empty DID repaint

    // A second identical set must be a no-op paint-wise (the guard short-circuits before markDirty).
    cluster.setParts (a);
    CHECK (cluster.repaintCountForTest() == afterFirst);      // no churn on the identical second call

    // Re-setting the SAME values yet again stays flat, even across many ticks.
    for (int i = 0; i < 5; ++i) cluster.setParts (a);
    CHECK (cluster.repaintCountForTest() == afterFirst);

    // A genuine change (mode Auto per-ear -> Left ear) MUST repaint.
    const auto b = formatClusterParts (48000.0, 24, CombineMode::LeftOnly);
    cluster.setParts (b);
    CHECK (cluster.repaintCountForTest() > afterFirst);
}
