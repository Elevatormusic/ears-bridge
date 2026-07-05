#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// The descriptor-consuming layout/HIG scorer: a byte-faithful, FONT-FREE C++ port of native-review.mjs's
// scoring math (WCAG contrast + geometry). It scores a PARSED native-render descriptor (the JSON the vendored
// probe juce_design_probe.h emits) - it never re-measures the live tree, so textOverflows/colours come from
// the one measurement source (the probe) and the C++ gate + the JS advisory agree on findings by construction.
namespace eb::hig {

struct Finding {
    juce::String category;   // contrast | overlap | duplicate | target-size | clip
    juce::String severity;   // high | medium
    juce::String element;    // the offending element id
    juce::String message;
};

// Score a parsed native-render descriptor (root object with an "elements" array). Reproduces native-review's
// contrast + geometry categories. Hierarchy/coverage are advisory-only and intentionally NOT produced here.
std::vector<Finding> scoreDescriptor (const juce::var& descriptorRoot);

// T10 EB extension BEYOND the native-review parity set (kept OUT of scoreDescriptor so the
// golden parity contract in test_hig_parity.cpp is untouched): visible text whose probe
// fontPt sits under the macOS legibility floor. Category "min-font", severity medium.
std::vector<Finding> scoreMinFont (const juce::var& descriptorRoot);

} // namespace eb::hig
