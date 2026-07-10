#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// The descriptor-consuming layout/HIG scorer: a byte-faithful, FONT-FREE C++ port of native-review.mjs's
// scoring math (WCAG contrast + geometry). It scores a PARSED native-render descriptor (the JSON the vendored
// probe juce_design_probe.h emits) - it never re-measures the live tree, so textOverflows/colours come from
// the one measurement source (the probe) and the C++ gate + the JS advisory agree on findings by construction.
namespace eb::hig {

struct Finding {
    juce::String category;   // contrast | overlap | duplicate | target-size | clip | min-font
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

// WCAG 2.x contrast ratio over "#rrggbb" hex strings — the byte-faithful native-review port that
// scoreDescriptor already uses internally, exposed so the token tests (P4 T4) and the state-sweep
// checker (P4 T7) share the codebase's ONE WCAG implementation.
double contrastRatio (const juce::String& fgHex, const juce::String& bgHex);

// P4 EB extension (kept OUT of scoreDescriptor - the golden parity contract is untouched): the
// state-sweep checker, a port of native-review.mjs stateFindings tiers 1-2 (v1.10.0). Tier 1
// inertness: 'unstyled-control-states' (medium; high when element.primary==true) when >=3 measurable
// states are all identical; 'two-state-inert' (info) when exactly 2 (often sanctioned - stock V4
// sliders, Apple's Disabled==Idle identities). Tier 2: 'disabled-louder' (low) when the disabled
// sample's contrast-vs-bg exceeds normal by > kContrastLouderMargin (hue swaps skipped), else the
// alpha fallback. Tier 3 (macOS recipe diff) is deliberately NOT adopted - advisory-by-design and
// the frames are our design authority (P4 frozen decision 8).
std::vector<Finding> scoreStateSweep (const juce::var& descriptorRoot);

} // namespace eb::hig
