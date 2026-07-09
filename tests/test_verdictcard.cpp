#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/StatusLadder.h"
#include "gui/VerdictCard.h"
#include "gui/Theme.h"
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"

using eb::RefMonState;

namespace {
eb::EarGradeSnapshot ear (RefMonState st, float sweepSnr, float irSnr, float thd, float peak = -8.0f) {
    eb::EarGradeSnapshot e;
    e.state = (int) st; e.sweepSnrDb = sweepSnr; e.irSnrDb = irSnr; e.thdPercent = thd; e.peakDb = peak;
    return e;
}
} // namespace

TEST_CASE("VerdictCard model: clean / marginal / suspect / stale-state mapping") {
    const auto clean = eb::verdictCardModelAuto ("LEFT EAR", ear (RefMonState::GradedClean, 30, 56, 0.8f), 0u, {}, false, false);
    CHECK (clean.graded);
    CHECK (clean.badge == "Clean");
    CHECK (clean.gradeWord == "Clean");
    CHECK (clean.fixLead == "Safe to keep.");
    CHECK (clean.tally == "7 checks pass");
    CHECK (clean.snrVal == "30 dB");
    CHECK_FALSE (clean.snrFlag);

    const auto marg = eb::verdictCardModelAuto ("RIGHT EAR", ear (RefMonState::GradedMarginal, 18, 52, 0.3f), 0u, {}, false, false);
    CHECK (marg.badge == "Marginal SNR");
    CHECK (marg.badgeTone == eb::StatusTone::Warn);
    CHECK (marg.snrFlag);                                   // the flagged metric value
    CHECK (marg.fixLead == "Dirac may mark this ear imprecise.");
    CHECK (marg.fixBody.isNotEmpty());                      // the ladder's qualityNote action copy

    const auto susp = eb::verdictCardModelAuto ("LEFT EAR", ear (RefMonState::GradedSuspect, 8, 20, 6.0f), 0u, {}, false, false);
    CHECK (susp.badge == "Suspect");
    CHECK (susp.badgeTone == eb::StatusTone::Danger);

    const auto staleRef = eb::verdictCardModelAuto ("LEFT EAR", ear (RefMonState::ReferenceStale, 0, 0, 0), 0u, {}, false, false);
    CHECK (staleRef.badge == "Re-learn");
    CHECK (staleRef.gradeWord == "No match");

    const auto ungraded = eb::verdictCardModelAuto ("LEFT EAR", ear (RefMonState::Learned, 0, 0, 0), 0u, {}, false, false);
    CHECK_FALSE (ungraded.graded);                          // the grid renders a CaptureCard instead
}

TEST_CASE("VerdictCard model: promoted-fix priority - clip beats the band note; no clip -> band note") {
    // Clip on a MARGINAL ear: the clip action wins the one promoted line (ladder precedence).
    const auto clipped = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedMarginal, 18, 52, 0.3f, +1.6f), 0u, {}, false, false);
    CHECK (clipped.fixLead.startsWith ("Input clipped"));
    CHECK (clipped.fixBody == eb::clipFixBody (1.6f));
    // NEGATIVE: same ear, healthy peak -> the band note owns the line, no clip wording anywhere.
    const auto noClip = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedMarginal, 18, 52, 0.3f, -8.0f), 0u, {}, false, false);
    CHECK_FALSE (noClip.fixLead.containsIgnoreCase ("clip"));
    CHECK (noClip.fixLead == "Dirac may mark this ear imprecise.");
}

TEST_CASE("VerdictCard model: provisional findings NEVER headline; ratified shape may headline a Clean card") {
    eb::ShapeScalars s; s.stepDb = 4.2f;
    const auto m = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedClean, 30, 56, 0.5f),
                                             eb::ShapeFlag::kStep, s, false, false);
    if constexpr (! eb::kShapeCopyRatified) {
        CHECK (m.fixLead == "Safe to keep.");               // kStep is provisional: it must NOT headline
        REQUIRE (m.observations.size() == 1);
        CHECK (m.observations[0].provisional);              // ...but it IS visible, tagged, in Details
        CHECK (m.observations[0].text.startsWith ("Level step:"));
    }
    // A RATIFIED finding headlines (drift copy is live today): the shapeInfoNote line becomes the fix.
    eb::ShapeScalars d; d.driftMaxDb = -9.0f;
    const auto md = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedClean, 30, 56, 0.5f),
                                              eb::ShapeFlag::kDrift, d, false, false);
    CHECK (md.fixLead == "Worth checking.");
    CHECK (md.fixBody == eb::shapeInfoNote (eb::ShapeFlag::kDrift, -9.0f, 0, 0, 0, 0));
    CHECK (md.fixTone == eb::StatusTone::Dim);              // INFO-only: a shape note never warns
}

TEST_CASE("VerdictCard model: the seven chips cover the detector families") {
    eb::ShapeScalars s; s.resonanceHz = 2400.0f; s.effHiHz = 15200.0f;
    const auto m = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedClean, 30, 56, 0.5f),
                                             eb::ShapeFlag::kResonance | eb::ShapeFlag::kTruncHi, s, false, false);
    // kResonance flags the D5 noise-floor chip (labeled Hum, frame naming); kTruncHi flags Band.
    int humIdx = -1, bandIdx = -1;
    for (int i = 0; i < eb::kVerdictChipCount; ++i) {
        if (juce::String (eb::kVerdictChips[i].label) == "Hum")  humIdx = i;
        if (juce::String (eb::kVerdictChips[i].label) == "Band") bandIdx = i;
    }
    REQUIRE (humIdx >= 0); REQUIRE (bandIdx >= 0);
    CHECK (m.chips[humIdx].flagged);
    CHECK (m.chips[humIdx].value.contains ("res"));
    CHECK (m.chips[bandIdx].flagged);
    CHECK (m.tally == "5 pass - 2 flagged");
    // NEGATIVE: the other five stay un-flagged.
    int flagged = 0; for (auto& c : m.chips) if (c.flagged) ++flagged;
    CHECK (flagged == 2);
}

TEST_CASE("VerdictCard model: hardware-Dirac ungraded variant + stale passthrough") {
    const auto hw = eb::verdictCardModelAuto ("LEFT EAR", ear (RefMonState::GradingOffHardware, 0, 0, 0), 0u, {}, false, true);
    CHECK (hw.hwDirac);
    CHECK (hw.badge == "Grading off");
    CHECK (hw.gradeWord == "Ungraded");
    CHECK (hw.fixBody.contains ("per-ear calibration still works"));
    const auto st = eb::verdictCardModelAuto ("L", ear (RefMonState::GradedClean, 30, 56, 0.5f), 0u, {}, true, false);
    CHECK (st.stale);
}

TEST_CASE("VerdictCard component: details toggle displaces (preferredHeight grows) and fires onLayoutChanged") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::VerdictCard card;
    card.setSize (273, 300);
    eb::ShapeScalars s; s.driftMaxDb = -9.0f;
    card.setModel (eb::verdictCardModelAuto ("RIGHT EAR",
        [] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedMarginal;
             e.sweepSnrDb = 18; e.irSnrDb = 52; e.thdPercent = 0.3f; e.peakDb = -8; return e; }(),
        eb::ShapeFlag::kDrift, s, false, false));
    const int closed = card.preferredHeight (273);
    int fired = 0; card.onLayoutChanged = [&] { ++fired; };
    card.setDetailsOpen (true);
    const int open = card.preferredHeight (273);
    CHECK (open > closed + 24);                       // chips + observation actually claim space
    CHECK (fired >= 1);
    card.setDetailsOpen (false);
    CHECK (card.preferredHeight (273) == closed);     // collapse restores exactly (one geometry source)
}

TEST_CASE("VerdictCard render scenes: clean / marginal-open / stale / hw score clean at card width") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    const bool wasDark = eb::Theme::dark();
    eb::Theme theme;                                   // palette owner for setDarkForTest

    struct Rig : juce::Component { eb::VerdictCard card; Rig() { addAndMakeVisible (card); }
                                   void resized() override { card.setBounds (getLocalBounds().reduced (8)); } };
    Rig rig;
    // Install the themed LookAndFeel on the standalone rig, exactly as MainComponent does on itself
    // (the CalSlot standalone precedent in test_hig_layout.cpp): the probe resolves a transparent
    // label's background by walking up to the nearest ancestor's ResizableWindow::backgroundColourId,
    // which is REGISTERED on the LookAndFeel - without this the light-mode scenes would be
    // contrast-scored against the process-default DARK LnF background (a harness artifact).
    rig.setLookAndFeel (&theme);
    eb::ShapeScalars s; s.driftMaxDb = -9.0f; s.hfShelfDb = -4.4f;
    auto marg = [&] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedMarginal;
                      e.sweepSnrDb = 18; e.irSnrDb = 52; e.thdPercent = 0.3f; e.peakDb = -8; return e; }();
    auto clean = [&] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedClean;
                       e.sweepSnrDb = 30; e.irSnrDb = 56; e.thdPercent = 0.8f; e.peakDb = -8; return e; }();
    struct Scene { const char* name; eb::VerdictCardModel m; bool open; };
    const Scene scenes[] = {
        { "clean",    eb::verdictCardModelAuto ("LEFT EAR", clean, 0u, {}, false, false), false },
        { "marg-open",eb::verdictCardModelAuto ("RIGHT EAR", marg, eb::ShapeFlag::kDrift, s, false, false), true },
        { "stale",    eb::verdictCardModelAuto ("LEFT EAR", clean, 0u, {}, true, false), false },
        { "hw",       eb::verdictCardModelAuto ("LEFT EAR",
                          [] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradingOffHardware; return e; }(),
                          0u, {}, false, true), false },
    };
    juce::StringArray bad;
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        for (auto& sc : scenes) {
            rig.card.applyTheme();
            rig.card.setModel (sc.m);
            rig.card.setDetailsOpen (sc.open);
            rig.setSize (289, rig.card.preferredHeight (273) + 16);   // 273 card + 8px rig margins
            rig.resized();
            const auto jf = tmp.getChildFile ("v.json"), pf = tmp.getChildFile ("v.png");
            hig::writeDesignProbe (rig, jf, pf);
            for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                if (f.category == "overlap" || f.category == "clip" || f.category == "contrast"
                    || f.category == "duplicate")
                    bad.add (juce::String (sc.name) + "/" + (dark ? "dark" : "light") + ": "
                             + f.category + " on " + f.element + " - " + f.message);
            // #68 re-derivation: the promoted fix line must FIT the card (wrap, never squish/overflow).
            // The probe's textOverflows flag on the fixBody label is the budget now (spec 6 refresh).
            // The label-text match is pinned (sawFixBody) so the budget check can never go vacuously
            // green if the card ever stopped rendering the model's fixBody verbatim.
            const auto tree = juce::JSON::parse (jf);
            bool sawFixBody = false;
            if (const auto* els = tree.getProperty ("elements", {}).getArray())
                for (auto& e2 : *els)
                    if (e2.getProperty ("label", {}).toString() == sc.m.fixBody) {
                        sawFixBody = true;
                        if ((bool) e2.getProperty ("textOverflows", false))
                            bad.add (juce::String (sc.name) + ": fix body overflows the card");
                    }
            if (! sawFixBody)
                bad.add (juce::String (sc.name) + ": fix body label missing from the descriptor (vacuous budget check)");
        }
    }
    rig.setLookAndFeel (nullptr);                      // detach before the theme destructs (JUCE rule)
    theme.setDarkForTest (wasDark);
    INFO ("findings:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}
