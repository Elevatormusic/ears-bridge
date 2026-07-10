#include <catch2/catch_test_macros.hpp>
#include "gui/HigScore.h"
#include <juce_core/juce_core.h>

// Golden parity: HigScore (the C++ build gate) must produce the SAME gate findings as native-review.mjs on a
// corpus of knife-edge descriptors. The expected.json files were captured by running native-review's
// reviewNativeDescriptor on each descriptor (see tests/fixtures/hig/gen.mjs), filtered to the gate categories
// the C++ reproduces (contrast/overlap/duplicate/target-size/clip; hierarchy/coverage are advisory-only). Both
// implementations are bound to this golden, so neither can drift from the other. Parity = the multiset of
// (category | severity | element); message text is intentionally out of scope.
TEST_CASE("HigScore matches the native-review golden findings on every fixture") {
    juce::File dir (EB_HIG_FIXTURE_DIR);
    auto descriptors = dir.findChildFiles (juce::File::findFiles, false, "*.descriptor.json");
    REQUIRE (descriptors.size() > 0);

    for (auto& descFile : descriptors) {
        const auto name = descFile.getFileName().upToLastOccurrenceOf (".descriptor.json", false, false);
        if (name.startsWith ("state-")) continue;   // P4 T7: the state corpus parity-locks scoreStateSweep
                                                    // (next case), not scoreDescriptor - split, never mixed
        auto expectedFile = dir.getChildFile (name + ".expected.json");
        REQUIRE (expectedFile.existsAsFile());

        auto findings = eb::hig::scoreDescriptor (juce::JSON::parse (descFile));
        // Hold the parsed var + the findings var in named locals: getArray() returns a pointer INTO the var, so
        // a one-line `parse(...).getProperty(...).getArray()` would dangle (the temporaries die end-of-expression).
        const auto expectedRoot     = juce::JSON::parse (expectedFile);
        const auto expectedFindings = expectedRoot.getProperty ("findings", {});
        auto* expected = expectedFindings.getArray();
        REQUIRE (expected != nullptr);

        juce::StringArray got, want;
        for (auto& f : findings) got.add (f.category + "|" + f.severity + "|" + f.element);
        for (auto& e : *expected) want.add (e.getProperty ("category", {}).toString() + "|"
                                          + e.getProperty ("severity", {}).toString() + "|"
                                          + e.getProperty ("element", {}).toString());
        got.sort (false); want.sort (false);
        INFO ("fixture: " << name << "  got=[" << got.joinIntoString (", ") << "]  want=[" << want.joinIntoString (", ") << "]");
        CHECK (got == want);
    }
}

// P4 T7: the state-sweep corpus. Same golden discipline for scoreStateSweep (stateFindings tiers 1-2,
// plugin v1.10.0): expected.json captured by gen.mjs from the SAME plugin source the checker ports, so
// the C++ port and the JS advisory cannot drift. Parity = (category | severity | element); messages out
// of scope. Fix port defects against the fixture truth - never regenerate fixtures to match the port.
TEST_CASE("scoreStateSweep matches the native-review golden on every state fixture") {
    juce::File dir (EB_HIG_FIXTURE_DIR);
    auto descriptors = dir.findChildFiles (juce::File::findFiles, false, "state-*.descriptor.json");
    REQUIRE (descriptors.size() > 0);
    for (auto& descFile : descriptors) {
        const auto name = descFile.getFileName().upToLastOccurrenceOf (".descriptor.json", false, false);
        auto expectedFile = dir.getChildFile (name + ".expected.json");
        REQUIRE (expectedFile.existsAsFile());
        auto findings = eb::hig::scoreStateSweep (juce::JSON::parse (descFile));
        const auto expectedRoot     = juce::JSON::parse (expectedFile);
        const auto expectedFindings = expectedRoot.getProperty ("findings", {});
        auto* expected = expectedFindings.getArray();
        REQUIRE (expected != nullptr);
        juce::StringArray got, want;
        for (auto& f : findings) got.add (f.category + "|" + f.severity + "|" + f.element);
        for (auto& e : *expected) want.add (e.getProperty ("category", {}).toString() + "|"
                                          + e.getProperty ("severity", {}).toString() + "|"
                                          + e.getProperty ("element", {}).toString());
        got.sort (false); want.sort (false);
        INFO ("fixture: " << name << "  got=[" << got.joinIntoString (", ") << "]  want=[" << want.joinIntoString (", ") << "]");
        CHECK (got == want);
    }
}
