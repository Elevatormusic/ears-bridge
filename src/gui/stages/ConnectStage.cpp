#include "gui/stages/ConnectStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
constexpr int kGutter = 16, kPadX = 30;
constexpr int kContentMaxW = 560;     // the frames' single-column rhythm
constexpr int kCardPad = 14, kCardGap = 12, kDiscH = 28, kDiscIndent = 24;
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
    for (const auto& r : owner->groupRects_) {
        g.setColour (Theme::surface());
        g.fillRoundedRectangle (r.toFloat(), 12.0f);   // elevation fill-step, no outline
    }
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
    groupRects_.clear();
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, width - 2 * kGutter));
    const int x0   = (width - colW) / 2;
    auto rr = juce::Rectangle<int> (x0, 0, colW, 100000);
    rr.removeFromTop (4);

    int cardTop = 0;
    const auto beginCard = [&] { cardTop = rr.getY(); rr.removeFromTop (kCardPad); };
    const auto endCard   = [&] {
        rr.removeFromTop (kCardPad);
        groupRects_.push_back ({ x0, cardTop, colW, rr.getY() - cardTop });
        rr.removeFromTop (kCardGap);
    };
    const auto row = [&] (juce::Component* c, int h) {
        auto b = rr.removeFromTop (h).reduced (kCardPad, 0);
        if (c != nullptr) c->setBounds (b);
        return b;
    };

    beginCard();                                       // ---- SIGNAL PATH (5.1: input -> output)
    row (inputPicker_, 62);            rr.removeFromTop (4);
    row (inputGainHint_, 60);          rr.removeFromTop (12);
    row (outputPicker_, 62);           rr.removeFromTop (4);
    row (outputHint_, 30);
    row (preflightLabel_, 14);
    if (preflightInfo_ != nullptr) {
        if (preflightInfo_->getText().isNotEmpty()) row (preflightInfo_, 14);
        else                                        preflightInfo_->setBounds ({});
    }
    if (diracCableHint_ != nullptr && diracCableHint_->isVisible()) {
        rr.removeFromTop (6);
        row (diracCableHint_, 48);
        if (diracFixButton_ != nullptr && diracFixButton_->isVisible()) {
            rr.removeFromTop (4);
            auto b = rr.removeFromTop (30).reduced (kCardPad, 0);
            diracFixButton_->setBounds (b.removeFromLeft (200));
        }
    }
    endCard();

    beginCard();                                       // ---- FORMAT
    row (combineLabel_, 16);  rr.removeFromTop (6);
    row (combineBox_, 40);    rr.removeFromTop (6);
    row (combineHint_, 80);   rr.removeFromTop (12);
    {
        auto rb = rr.removeFromTop (62).reduced (kCardPad, 0);
        auto rcol = rb.removeFromLeft (rb.getWidth() / 2 - 8);
        rb.removeFromLeft (16);
        if (rateLabel_ != nullptr) rateLabel_->setBounds (rcol.removeFromTop (16));
        rcol.removeFromTop (6);
        if (rateBox_ != nullptr)   rateBox_->setBounds (rcol.removeFromTop (40));
        if (bitLabel_ != nullptr)  bitLabel_->setBounds (rb.removeFromTop (16));
        rb.removeFromTop (6);
        if (bitBox_ != nullptr)    bitBox_->setBounds (rb.removeFromTop (40));
    }
    row (rateWarn_, 14);
    endCard();

    beginCard();                                       // ---- WIRING CHECK (5.1: a connect concern)
    {
        auto b = rr.removeFromTop (30).reduced (kCardPad, 0);
        if (verifyButton_ != nullptr)
            verifyButton_->setBounds (b.removeFromLeft (juce::jmin (220, b.getWidth())));
    }
    rr.removeFromTop (4);
    row (verifyResultLabel_, 16);
    endCard();

    // ---- "Not using Dirac?" escape hatch (flat row; contents only while open) ----
    notUsingDirac_.setBounds (rr.removeFromTop (kDiscH));
    if (overrideToggle_ != nullptr) {
        overrideToggle_->setVisible (notUsingDirac_.isOpen());
        if (notUsingDirac_.isOpen()) {
            rr.removeFromTop (6);
            overrideToggle_->setBounds (rr.removeFromTop (26).withTrimmedLeft (kDiscIndent));
        } else {
            overrideToggle_->setBounds ({});
        }
    }
    return rr.getY() + kGutter;
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
