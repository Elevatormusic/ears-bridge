#include "gui/stages/ConnectStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
constexpr int kGutter = 16, kPadX = 30;
constexpr int kContentMaxW = 560;              // the frames' single-column rhythm
constexpr int kCardPadX = 16, kCardPadY = 12;  // T10: horizontal air kept, vertical on the 4-grid
constexpr int kCardGap = 8, kDiscH = 24, kDiscIndent = 24;
constexpr int kGapIntra = 4, kGapInter = 8;    // T10 spacing rhythm
constexpr int kEyebrowH = 14, kComboH = 28, kHintH = 16, kHint2H = 32, kBtnH = 28;
} // namespace

ConnectStage::ConnectStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Connect");

    addAndMakeVisible (header_);
    // SHARED StageHeader: name this stage's run-note element uniquely (StageHeader mandates a unique
    // id per instance - a hard-coded id would collide with Calibrate's "calRunNote"). #T7 defuse.
    header_.setRunNoteComponentID ("connectRunNote");
    header_.continueButton().onClick = [this] { if (onContinue) onContinue(); };

    content_.owner = this;
    viewport_.setViewedComponent (&content_, false);   // false: content_ is a member
    viewport_.setScrollBarsShown (true, false);         // vertical only
    viewport_.setScrollBarThickness (10);
    addAndMakeVisible (viewport_);

    content_.addAndMakeVisible (notUsingDirac_);
    notUsingDirac_.onOpenChanged = [this] (bool) { resized(); };
}

void ConnectStage::Content::paint (juce::Graphics& g) {
    if (owner == nullptr) return;
    for (const auto& r : owner->groupRects_)
        Theme::paintCardSurface (g, r.toFloat());          // W2 card: surface + 1px sep hairline, r12
}

void ConnectStage::applyTheme() {
    header_.applyTheme();
}

void ConnectStage::syncOverrideDisclosure() {
    // Honesty: an ENABLED override may never be collapsed out of sight. The status line's
    // "Advanced override on - not the standard Dirac path." advisory remains the second surface.
    const bool on = overrideToggle_ != nullptr && overrideToggle_->getToggleState();
    notUsingDirac_.setLocked (on);                     // locking force-opens
    notUsingDirac_.setSummary (on ? "Override on - non-Dirac use" : juce::String());
}

void ConnectStage::adopt (DevicePicker& inputPicker, juce::Label& inputGainHint,
                          juce::Label& combineLabel, juce::ComboBox& combineBox, juce::Label& combineHint,
                          DevicePicker& outputPicker, juce::Label& outputHint,
                          juce::Label& preflightLabel, juce::Label& preflightInfo,
                          juce::Label& diracCableHint, juce::TextButton& diracFixButton,
                          juce::Label& rateLabel, juce::ComboBox& rateBox, juce::Label& rateWarn,
                          juce::Label& bitLabel, juce::ComboBox& bitBox,
                          juce::TextButton& verifyButton, juce::Label& verifyResultLabel,
                          juce::ToggleButton& overrideToggle) {
    inputPicker_ = &inputPicker;             content_.addAndMakeVisible (inputPicker);
    inputGainHint_ = &inputGainHint;         content_.addAndMakeVisible (inputGainHint);
    combineLabel_ = &combineLabel;           content_.addAndMakeVisible (combineLabel);
    combineBox_ = &combineBox;               content_.addAndMakeVisible (combineBox);
    combineHint_ = &combineHint;             content_.addAndMakeVisible (combineHint);
    outputPicker_ = &outputPicker;           content_.addAndMakeVisible (outputPicker);
    outputHint_ = &outputHint;               content_.addAndMakeVisible (outputHint);
    preflightLabel_ = &preflightLabel;       content_.addAndMakeVisible (preflightLabel);
    preflightInfo_ = &preflightInfo;         content_.addAndMakeVisible (preflightInfo);
    diracCableHint_ = &diracCableHint;       content_.addChildComponent (diracCableHint);   // shown only for a standard VB-CABLE
    diracFixButton_ = &diracFixButton;       content_.addChildComponent (diracFixButton);
    rateLabel_ = &rateLabel;                 content_.addAndMakeVisible (rateLabel);
    rateBox_ = &rateBox;                     content_.addAndMakeVisible (rateBox);
    rateWarn_ = &rateWarn;                   content_.addAndMakeVisible (rateWarn);
    bitLabel_ = &bitLabel;                   content_.addAndMakeVisible (bitLabel);
    bitBox_ = &bitBox;                       content_.addAndMakeVisible (bitBox);
    verifyButton_ = &verifyButton;           content_.addAndMakeVisible (verifyButton);       // a connection concern (spec 5.1)
    verifyResultLabel_ = &verifyResultLabel; content_.addAndMakeVisible (verifyResultLabel);
    overrideToggle_ = &overrideToggle;       content_.addChildComponent (overrideToggle);     // disclosed content, hidden while collapsed

    // 5.1 visual order = focus order: input, output(+fix), combine, rate, bit, verify,
    // disclosure, override, CTA last.
    int fo = 1;
    inputPicker.setExplicitFocusOrder     (fo++);
    outputPicker.setExplicitFocusOrder    (fo++);
    diracFixButton.setExplicitFocusOrder  (fo++);
    combineBox.setExplicitFocusOrder      (fo++);
    rateBox.setExplicitFocusOrder         (fo++);
    bitBox.setExplicitFocusOrder          (fo++);
    verifyButton.setExplicitFocusOrder    (fo++);
    notUsingDirac_.setExplicitFocusOrder  (fo++);
    overrideToggle.setExplicitFocusOrder  (fo++);
    header_.continueButton().setExplicitFocusOrder (fo++);
}

void ConnectStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
}

int ConnectStage::layoutContent (int width) {
    // Content::paint draws the card fills straight from groupRects_, but content_ only auto-repaints
    // when setSize CHANGES its size. On a tall window the jmax(contentH, viewport) clamp pins the size,
    // so a relayout that grows a card (diracCableHint/fix appear, preflightInfo gains text) would leave
    // a STALE fill painted behind the moved controls - a background stripe across the grown card. So we
    // repaint content_ whenever the rects actually change. This mirrors the P1 WizardSpine markDirty
    // discipline (repaint only on a real visual delta), not an unconditional per-resized repaint.
    const auto prevRects = groupRects_;
    groupRects_.clear();
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, width - 2 * kGutter));
    const int x0   = (width - colW) / 2;
    auto rr = juce::Rectangle<int> (x0, 0, colW, 100000);
    // T10: no top pad - the fixed header supplies the whitespace; every px belongs to the fold budget.

    int cardTop = 0;
    const auto beginCard = [&] { cardTop = rr.getY(); rr.removeFromTop (kCardPadY); };
    const auto endCard   = [&] {
        rr.removeFromTop (kCardPadY);
        groupRects_.push_back ({ x0, cardTop, colW, rr.getY() - cardTop });
        rr.removeFromTop (kCardGap);
    };
    const auto row = [&] (juce::Component* c, int h) {
        auto b = rr.removeFromTop (h).reduced (kCardPadX, 0);
        if (c != nullptr) c->setBounds (b);
        return b;
    };

    beginCard();                                       // ---- SIGNAL PATH (5.1: input -> output)
    row (inputPicker_, 46);            rr.removeFromTop (kGapIntra);
    row (inputGainHint_, kHint2H);     rr.removeFromTop (kGapInter);
    row (outputPicker_, 46);           rr.removeFromTop (kGapIntra);
    // Output MESSAGE SLOT (T10-D7): the passive Dirac-capture hint OR the cable warning
    // (+ fix) - never both. Both guide the same control; the warning supersedes the guidance
    // while active and the guidance returns the instant the hint clears.
    const bool diracHintOn = diracCableHint_ != nullptr && diracCableHint_->isVisible();
    if (outputHint_ != nullptr) {
        outputHint_->setVisible (! diracHintOn);
        if (! diracHintOn) row (outputHint_, kHintH);
        else               outputHint_->setBounds ({});
    }
    if (diracHintOn) {
        row (diracCableHint_, kHint2H);
        if (diracFixButton_ != nullptr && diracFixButton_->isVisible()) {
            rr.removeFromTop (kGapIntra);
            auto b = rr.removeFromTop (kBtnH).reduced (kCardPadX, 0);
            diracFixButton_->setBounds (b.removeFromLeft (200));
        }
    }
    // Post-Start preflight lines claim rows only when non-empty. These are the ONLY Connect
    // rows allowed to push past the min-window fold (post-failure states, not workflow states;
    // the Task-5 boundary gate proves the warnings still sit above the fold then).
    if (preflightLabel_ != nullptr) {
        if (preflightLabel_->getText().isNotEmpty()) { rr.removeFromTop (kGapIntra); row (preflightLabel_, kHint2H); }
        else preflightLabel_->setBounds ({});
    }
    if (preflightInfo_ != nullptr) {
        if (preflightInfo_->getText().isNotEmpty()) { rr.removeFromTop (kGapIntra); row (preflightInfo_, kHintH); }
        else preflightInfo_->setBounds ({});
    }
    endCard();

    beginCard();                                       // ---- FORMAT
    row (combineLabel_, kEyebrowH);  rr.removeFromTop (kGapIntra);
    row (combineBox_, kComboH);      rr.removeFromTop (kGapIntra);
    row (combineHint_, kHint2H);     rr.removeFromTop (kGapInter);
    {
        auto rb = rr.removeFromTop (kEyebrowH + kGapIntra + kComboH).reduced (kCardPadX, 0);
        auto rcol = rb.removeFromLeft (rb.getWidth() / 2 - 8);
        rb.removeFromLeft (16);
        if (rateLabel_ != nullptr) rateLabel_->setBounds (rcol.removeFromTop (kEyebrowH));
        rcol.removeFromTop (kGapIntra);
        if (rateBox_ != nullptr)   rateBox_->setBounds (rcol.removeFromTop (kComboH));
        if (bitLabel_ != nullptr)  bitLabel_->setBounds (rb.removeFromTop (kEyebrowH));
        rb.removeFromTop (kGapIntra);
        if (bitBox_ != nullptr)    bitBox_->setBounds (rb.removeFromTop (kComboH));
    }
    if (rateWarn_ != nullptr) {   // warning row claims space only while live (home preserved: same card, same spot)
        if (rateWarn_->getText().isNotEmpty()) { rr.removeFromTop (kGapIntra); row (rateWarn_, kHintH); }
        else rateWarn_->setBounds ({});
    }
    endCard();

    {                                                  // ---- WIRING CHECK: a plain row, demoted off card (P2.9)
        auto b = rr.removeFromTop (32).reduced (kCardPadX, 0);
        if (verifyButton_ != nullptr) {
            auto cell = b.removeFromLeft (juce::jmin (200, b.getWidth()));
            verifyButton_->setBounds (cell.withSizeKeepingCentre (cell.getWidth(), kBtnH));
        }
        b.removeFromLeft (12);
        if (verifyResultLabel_ != nullptr) verifyResultLabel_->setBounds (b);
        rr.removeFromTop (kCardGap);
    }

    // ---- "Not using Dirac?" escape hatch ----
    notUsingDirac_.setBounds (rr.removeFromTop (kDiscH));
    if (overrideToggle_ != nullptr) {
        overrideToggle_->setVisible (notUsingDirac_.isOpen());
        if (notUsingDirac_.isOpen()) {
            rr.removeFromTop (kGapIntra);
            overrideToggle_->setBounds (rr.removeFromTop (24).withTrimmedLeft (kDiscIndent));
        } else {
            overrideToggle_->setBounds ({});
        }
    }

    // Repaint the card fills iff the geometry the paint reads actually moved (delta-only, per the P1
    // repaint-discipline). On a tall window setSize is a no-op, so this is the ONLY thing that refreshes
    // the stale fill behind a grown/shrunk card.
    if (groupRects_ != prevRects)
        content_.repaint();
    return rr.getY() + 4;                              // 4px bottom breathing (fold ledger)
}

void ConnectStage::resized() {
    auto area = getLocalBounds();
    header_.setBounds (area.removeFromTop (StageHeader::kHeight).reduced (kPadX, 0));
    viewport_.setBounds (area);
    const int contentW = viewport_.getMaximumVisibleWidth();
    const int contentH = layoutContent (contentW);
    content_.setSize (contentW, juce::jmax (contentH, viewport_.getHeight()));
    const int finalW = viewport_.getMaximumVisibleWidth();   // scrollbar may have toggled
    if (finalW != contentW) {
        const int h2 = layoutContent (finalW);
        content_.setSize (finalW, juce::jmax (h2, viewport_.getHeight()));
    }
}

} // namespace eb
