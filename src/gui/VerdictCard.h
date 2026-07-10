#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/StatusLadder.h"     // VerdictCardModel / kVerdictChipCount / StatusTone
#include "gui/DisclosureRow.h"

namespace eb {

// The SP3-hybrid per-ear verdict card (spec 6 [P3-refresh 2026-07-05]) - W2's hero payoff. A pure
// VIEW: setModel() in, deterministic render out; it holds no engine state and gates nothing.
// Every tone-bearing string is a child juce::Label (RULE2 walker coverage by construction); paint()
// draws only surfaces (card fill via Theme::paintCardSurface, warn border, badge capsule, chip
// backgrounds, hairlines). Details expansion is push-down displacement (spec 4 RULE2) - the owner
// relayouts via onLayoutChanged; the card never scrolls internally.
class VerdictCard : public juce::Component {
public:
    VerdictCard();

    void setModel (const VerdictCardModel& m);      // fires onLayoutChanged when my height changed
    void setDetailsOpen (bool open);                // harness/gate seam + the user path (DisclosureRow)
    bool detailsOpen() const { return details_.isOpen(); }
    int  preferredHeight (int width) const;         // the owner's grid uses this (align-start columns)
    // P3 Task 7 (T2-review reconciliation of the stale double-stating): in the Measure stage the
    // §5.4 stale STRIP is the single voice for the staleness clause, so the stage disables the
    // card's inline tag; standalone (no strip) the tag stays the visible why. Dimming + the a11y
    // description clause are unconditional either way (spec 5.4).
    void setInlineStaleTagEnabled (bool on);

    std::function<void()> onLayoutChanged;          // details toggled / a model change changed my height

    // ---- test seams (documented; tests/test_verdictcard.cpp, test_hig_layout.cpp) ----
    DisclosureRow& detailsRowForTest() { return details_; }
    juce::Label&   fixBodyForTest()    { return fixBody_; }
    juce::Label&   badgeForTest()      { return badge_; }
    juce::Label&   staleTagForTest()   { return staleTag_; }

    void applyTheme();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int layoutRows (int width, bool apply);         // ONE geometry source for resized() + preferredHeight()
    juce::Label earName_, badge_, grade_, qualifier_;
    juce::Label mLabel_[3], mVal_[3];               // SNR / IR-SNR / THD columns
    juce::Label fixLead_, fixBody_;
    DisclosureRow details_ { "Details" };
    juce::Label chip_[kVerdictChipCount];
    juce::Label observations_, staleTag_;
    VerdictCardModel model_;
    bool inlineStaleTag_ = true;                    // false in the Measure grid (the strip speaks)
    juce::Rectangle<int> badgeBg_; juce::Rectangle<int> chipBg_[kVerdictChipCount];
    int fixRuleY_ = 0;                              // hairline above the fix note (paint reads)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VerdictCard)
};

} // namespace eb
