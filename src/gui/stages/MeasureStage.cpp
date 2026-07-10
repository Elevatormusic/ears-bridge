#include "gui/stages/MeasureStage.h"
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)
#include "gui/Theme.h"

namespace eb {
namespace {
constexpr int kGutter = 16, kContentMaxW = 560;
constexpr int kCardPadX = 16, kCardPadY = 12, kCardGap = 8;
// P3 Task 5 (measured, ledgered deviation): the frozen §2 subs need 39px (3 lines of 13px) in the
// 294px sub box - StageHeader::kHeight's 34px slot clips the 3rd line. This stage reserves the
// extra sub row; the shared header lays its (flexible) sub into it. Other stages stay at kHeight.
// P3 Task 6 (routed ruling, Task 5's review): + kTitleExtraRow for the 2-line armed title (scale is
// pinned 1.0 - the hero instruction never squishes). A CONSTANT reserve, not per-state: the fold
// budget must not depend on which title is showing (state-invariant viewport for the fit gate).
constexpr int kHeaderH = StageHeader::kHeight + StageHeader::kSubExtraRow + StageHeader::kTitleExtraRow;
} // namespace

// ---- pure copy rules ------------------------------------------------------------------------------

MeasureStage::HeadCopy MeasureStage::measureHeadCopy (Lead lead, bool overrideOn, bool verdictShowing,
                                                      CombineMode mode) {
    if (verdictShowing) {
        // P3 Task 7 ruling (user): under a Combined mode (Average/Sum) the capture is the two earcups
        // mixed to ONE channel - "each earcup is graded" would be a per-ear claim the evidence can't
        // back. Two-pass/Auto keep the per-ear head (each earcup does get its own graded capture).
        if (mode == CombineMode::Average || mode == CombineMode::Sum)
            return { "Your measurement result",
                     "The grade reflects the combined capture" + kDash + "both earcups mixed into one channel" + kDash + "expand Details to see the shape checks behind it." };
        return { "Your per-ear result",
                 "Each earcup is graded against the matched reference" + kDash + "expand Details on any ear to see the shape checks behind the grade." };
    }
    switch (lead) {
        case Lead::HwDirac:
            return { "Measurement grading is off",
                     "Dirac runs on a hardware processor" + kDash + "there is no PC loopback to grade against. The per-ear calibration still works." };
        case Lead::Reference:
            return { "Learn your reference",
                     "One time only: EARS Bridge captures Dirac's own sweep as the grading reference. Run a Dirac measurement while it listens." };
        case Lead::Waiting:
        default:
            if (overrideOn)                                          // §7: the Dirac-shaped copy would be wrong
                return { "Run your measurement sweep",
                         "Play a measurement sweep from your measurement software" + kDash + "EARS Bridge listens and grades each earcup as it lands." };
            return { "Now run the measurement in Dirac Live",
                     "In Dirac Live, choose CABLE Input (VB-Audio) as the recording device and click Start measurement. EARS Bridge listens and grades each earcup as it lands." };
    }
}

juce::String MeasureStage::measureRunNote (bool blocked, const juce::String& machineReason,
                                           bool running, bool measureAgainShowing) {
    if (blocked) return machineReason;
    if (measureAgainShowing) return "Then start the measurement again in Dirac Live.";   // §2 correction
    if (! running) return "Arms the bridge" + kDash + "the sweep still runs in Dirac";             // the contract, stated
    return {};
}

juce::String MeasureStage::waitingHint (int armedSeconds, bool refEndpointMismatch, bool chainMismatch,
                                        const juce::String& chainSummary, bool silentInput) {
    if (armedSeconds < kArmedNoSweepHintSeconds) return {};          // passive INFO, never premature
    if (refEndpointMismatch)                                         // #34: the most specific knowledge
        return "Still listening" + kDash + "Dirac's output isn't the device the reference was learned from. "
               "Switch it back, or re-learn the reference.";
    if (chainMismatch && chainSummary.isNotEmpty())
        return "Still listening" + kDash + chainSummary;
    if (silentInput)   // spec §2 fragment kept lowercase mid-sentence (the brief's test pins it so)
        return "Still listening" + kDash + "no signal at all" + kDash + "is Dirac playing? Check Dirac's recording device.";
    return "No sweep detected yet" + kDash + "in Dirac Live, click Start measurement. "
           "EARS Bridge can't start it for you.";
}

// ---- component ------------------------------------------------------------------------------------

MeasureStage::MeasureStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Measure");
    addAndMakeVisible (header_);
    header_.setRunNoteComponentID ("measureRunNote");      // unique per stage (the T7-defuse rule)
    header_.continueButton().onClick = [this] { if (onTransport) onTransport(); };

    content_.owner = this;
    viewport_.setViewedComponent (&content_, false);
    viewport_.setScrollBarsShown (true, false);
    viewport_.setScrollBarThickness (10);
    addAndMakeVisible (viewport_);

    refLead_.setJustificationType (juce::Justification::topLeft);
    refLead_.setMinimumHorizontalScale (1.0f);
    waitHint_.setJustificationType (juce::Justification::topLeft);
    waitHint_.setMinimumHorizontalScale (1.0f);
    hwLead_.setJustificationType (juce::Justification::topLeft);
    hwLead_.setMinimumHorizontalScale (1.0f);
    meterLegend_.setJustificationType (juce::Justification::centredRight);
    for (auto* c : { (juce::Component*) &refLead_, (juce::Component*) &waitHint_, (juce::Component*) &hwLead_,
                     (juce::Component*) &meterTitle_, (juce::Component*) &meterLegend_,
                     (juce::Component*) &liveL_, (juce::Component*) &liveR_, (juce::Component*) &liveOut_ })
        content_.addChildComponent (*c);                    // visibility is lead-state-driven (layoutContent)
    content_.addChildComponent (capL_);                     // P3 Task 6: the capture grid (Waiting lead only)
    content_.addChildComponent (capR_);
    content_.addChildComponent (verdL_);                    // P3 Task 7: the per-ear verdict cards + strip
    content_.addChildComponent (verdR_);
    content_.addChildComponent (staleStrip_);
    // A details toggle / model-height change relayouts the column (push-down displacement, spec 4).
    verdL_.onLayoutChanged = verdR_.onLayoutChanged = [this] { resized(); };
    // T2-review reconciliation (double-stating): in THIS stage the §5.4 stale strip is the single
    // voice for the staleness clause - the cards' inline tags stay off (dimming + a11y remain).
    verdL_.setInlineStaleTagEnabled (false);
    verdR_.setInlineStaleTagEnabled (false);
    liveL_.setShowTargetBand (true);
    liveR_.setShowTargetBand (true);
    applyTheme();
}

void MeasureStage::applyTheme() {
    header_.applyTheme();
    capL_.applyTheme();
    capR_.applyTheme();
    verdL_.applyTheme();
    verdR_.applyTheme();
    staleStrip_.setColour (juce::Label::textColourId, Theme::textDim());
    staleStrip_.setFont (juce::Font (juce::FontOptions (12.0f)));
    refLead_.setColour (juce::Label::textColourId, Theme::textDim());
    refLead_.setFont (juce::Font (juce::FontOptions (12.0f)));
    refLead_.setText ("The reference is the yardstick every sweep is graded against. Learn it once; it's stored for next time.",
                      juce::dontSendNotification);
    waitHint_.setColour (juce::Label::textColourId, Theme::infoText());   // passive INFO tone, never warn
    waitHint_.setFont (juce::Font (juce::FontOptions (12.0f)));
    hwLead_.setColour (juce::Label::textColourId, Theme::textDim());
    hwLead_.setFont (juce::Font (juce::FontOptions (12.0f)));
    hwLead_.setText ("Measurement grading isn't available when Dirac runs on a hardware processor" + kDash + "the per-ear calibration still works.", juce::dontSendNotification);
    meterTitle_.setColour (juce::Label::textColourId, Theme::textDim());
    meterTitle_.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")).withExtraKerningFactor (0.04f));
    meterTitle_.setText ("LIVE LEVEL", juce::dontSendNotification);
    // textDim, not the plan's textFaint: 11pt is normal-size text under WCAG 1.4.3 and textFaint
    // measures ~2.4:1 - the 64-cell contrast gate fails it (the LevelStage legend precedent).
    meterLegend_.setColour (juce::Label::textColourId, Theme::textDim());
    meterLegend_.setFont (juce::Font (juce::FontOptions (11.0f)));
    meterLegend_.setText ("green target -18 to -12 dBFS", juce::dontSendNotification);
}

void MeasureStage::adopt (juce::Label& statusLine, juce::TextButton& learnRefButton,
                          juce::Label& learnRefResultLabel, juce::ToggleButton& hwDiracToggle) {
    statusLine_ = &statusLine;                   content_.addAndMakeVisible (statusLine);
    learnRefButton_ = &learnRefButton;           content_.addChildComponent (learnRefButton);
    learnRefResultLabel_ = &learnRefResultLabel; content_.addChildComponent (learnRefResultLabel);
    hwDiracToggle_ = &hwDiracToggle;             content_.addAndMakeVisible (hwDiracToggle);
    statusLine.setJustificationType (juce::Justification::centredLeft);

    int fo = 1;                                            // learn action, hw toggle, transport last
    learnRefButton.setExplicitFocusOrder (fo++);
    hwDiracToggle.setExplicitFocusOrder  (fo++);
    header_.continueButton().setExplicitFocusOrder (fo++);
}

void MeasureStage::setLead (Lead l) { if (l != lead_) { lead_ = l; resized(); } }
void MeasureStage::setHeadCopy (const juce::String& t, const juce::String& s) { header_.setTitleText (t); header_.setSubText (s); }
void MeasureStage::setWaitHint (const juce::String& s) {
    if (waitHint_.getText() != s) { waitHint_.setText (s, juce::dontSendNotification); resized(); }
}
void MeasureStage::feedLiveLevels (float l, bool cl, float r, bool cr, float out, bool co) {
    liveL_.setLevel (l, cl); liveR_.setLevel (r, cr); liveOut_.setLevel (out, co);
}
void MeasureStage::setActiveEar (int ear) { liveL_.setActive (ear == 0); liveR_.setActive (ear == 1); }
// The capture grid's geometry is model-invariant (fixed 164px rows - see layoutContent's row
// budget; the bar is paint-only) and its visibility is LEAD-driven in layoutContent, so a model
// feed never needs a relayout - each card is set-if-changed and repaints itself only on a real
// change. (Task 7 review fix: this comment said 148px while the code laid 164 - stale since the
// Task 6 render-measured sub-slot fix.)
void MeasureStage::setCaptureModels (const CaptureCardModel& l, const CaptureCardModel& r) {
    capL_.setModel (l);
    capR_.setModel (r);
}

// P3 Task 7: the verdict feed. Set-if-changed at STAGE level (VerdictCardModel::operator==) because
// this runs on the 30 Hz tick (refreshWizardView -> refreshMeasureView) - an unchanged truth must
// not relayout/repaint the column every tick (the Task-6 delta-repaint discipline). VerdictCard's
// own setModel stays unconditional so applyTheme's setModel(model_) re-tone keeps working (the
// CaptureCard applyModelTone lesson).
void MeasureStage::setVerdictModels (const VerdictCardModel& l, const VerdictCardModel& r, bool stale) {
    if (l == verdModelL_ && r == verdModelR_ && stale == verdStale_) return;
    verdModelL_ = l; verdModelR_ = r; verdStale_ = stale;
    verdL_.setModel (l);
    verdR_.setModel (r);
    staleStrip_.setText (stale ? "From your previous configuration" + kDash + "measure again to refresh."
                               : juce::String(), juce::dontSendNotification);
    resized();
}

void MeasureStage::Content::paint (juce::Graphics& g) {
    if (owner == nullptr) return;
    for (const auto& r : owner->cardRects_)
        Theme::paintCardSurface (g, r.toFloat());
}

void MeasureStage::paint (juce::Graphics& g) { g.fillAll (Theme::bg()); }

int MeasureStage::layoutContent (int width) {
    const auto prevRects = cardRects_;
    cardRects_.clear();
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, width - 2 * kGutter));
    const int x0   = (width - colW) / 2;
    auto rr = juce::Rectangle<int> (x0, 0, colW, 100000);
    const bool isRef = lead_ == Lead::Reference, isHw = lead_ == Lead::HwDirac, isWait = lead_ == Lead::Waiting;

    // ---- lead block (exactly one) ----
    if (learnRefButton_)      learnRefButton_->setVisible (isRef);
    if (learnRefResultLabel_) learnRefResultLabel_->setVisible (isRef);
    refLead_.setVisible (isRef);
    hwLead_.setVisible (isHw);
    if (statusLine_) statusLine_->setVisible (! isHw);      // the ladder line is meaningless w/ grading off
    waitHint_.setVisible (isWait && waitHint_.getText().isNotEmpty());

    if (isRef) {                                            // the Reference card (roadmap #29's original ask)
        const int top = rr.getY();
        rr.removeFromTop (kCardPadY);
        refLead_.setBounds (rr.removeFromTop (32).reduced (kCardPadX, 0));
        rr.removeFromTop (8);
        // Null-guarded (plan defect: the brief dereferenced unguarded - a standalone stage that is
        // setLead(Reference)-ed before adopt() would crash; the guards keep layout total either way).
        if (learnRefButton_)
            learnRefButton_->setBounds (rr.removeFromTop (28).reduced (kCardPadX, 0).removeFromLeft (260));
        else rr.removeFromTop (28);
        rr.removeFromTop (4);
        if (learnRefResultLabel_)
            learnRefResultLabel_->setBounds (rr.removeFromTop (32).reduced (kCardPadX, 0));
        else rr.removeFromTop (32);
        rr.removeFromTop (kCardPadY);
        cardRects_.push_back ({ x0, top, colW, rr.getY() - top });
        rr.removeFromTop (kCardGap);
    } else if (isHw) {
        hwLead_.setBounds (rr.removeFromTop (32));
        rr.removeFromTop (kCardGap);
    }
    if (statusLine_ && ! isHw) { statusLine_->setBounds (rr.removeFromTop (18)); rr.removeFromTop (4); }
    if (waitHint_.isVisible()) { waitHint_.setBounds (rr.removeFromTop (32)); rr.removeFromTop (4); }
    rr.removeFromTop (8);

    // ---- the per-ear grid: VerdictCard once that ear graded (the payoff), else its CaptureCard ----
    // (Waiting lead only; hidden for Reference/HwDirac leads.)
    const bool grid = isWait;
    const bool showVerdL = grid && verdModelL_.graded, showVerdR = grid && verdModelR_.graded;
    staleStrip_.setVisible (grid && verdStale_ && (showVerdL || showVerdR));
    if (staleStrip_.isVisible()) { staleStrip_.setBounds (rr.removeFromTop (16)); rr.removeFromTop (4); }
    capL_.setVisible (grid && ! showVerdL);  capR_.setVisible (grid && ! showVerdR);
    verdL_.setVisible (showVerdL);           verdR_.setVisible (showVerdR);
    if (grid) {
        const int cardW = (colW - 14) / 2;
        // CaptureCard rows are fixed 164 = 12+18+10+24+4+48+10+6+6+14+12 (48px sub = 3 lines,
        // render-measured, Task 6); a VerdictCard claims its own MEASURED preferred height.
        const int hL = showVerdL ? verdL_.preferredHeight (cardW) : 164;
        const int hR = showVerdR ? verdR_.preferredHeight (cardW) : 164;
        const int rowY = rr.getY();
        if (showVerdL) verdL_.setBounds ({ x0, rowY, cardW, hL });              else capL_.setBounds ({ x0, rowY, cardW, hL });
        if (showVerdR) verdR_.setBounds ({ x0 + cardW + 14, rowY, cardW, hR }); else capR_.setBounds ({ x0 + cardW + 14, rowY, cardW, hR });
        rr.removeFromTop (juce::jmax (hL, hR));                                 // align-start columns
        rr.removeFromTop (kCardGap);
    }

    // ---- live-meters strip ----
    {
        const int top = rr.getY();
        rr.removeFromTop (kCardPadY);
        auto head = rr.removeFromTop (14).reduced (kCardPadX, 0);
        meterTitle_.setBounds (head.removeFromLeft (90));
        meterLegend_.setBounds (head);
        rr.removeFromTop (8);
        for (auto* m : { &liveL_, &liveR_, &liveOut_ }) {
            m->setVisible (true);
            m->setBounds (rr.removeFromTop (20).reduced (kCardPadX, 0));
            if (m != &liveOut_) rr.removeFromTop (6);
        }
        rr.removeFromTop (kCardPadY);
        cardRects_.push_back ({ x0, top, colW, rr.getY() - top });
        rr.removeFromTop (kCardGap);
    }
    meterTitle_.setVisible (true); meterLegend_.setVisible (true);

    if (hwDiracToggle_) hwDiracToggle_->setBounds (rr.removeFromTop (26));

    if (cardRects_ != prevRects) content_.repaint();        // delta-repaint discipline (T7 stale-fill lesson)
    return rr.getY() + 4;
}

void MeasureStage::resized() {
    auto area = getLocalBounds();
    auto headerArea = area.removeFromTop (kHeaderH);        // kHeight + the 3rd sub line (see kHeaderH)
    viewport_.setBounds (area);
    const int contentW = viewport_.getMaximumVisibleWidth();
    const int contentH = layoutContent (contentW);
    content_.setSize (contentW, juce::jmax (contentH, viewport_.getHeight()));
    const int finalW = viewport_.getMaximumVisibleWidth();
    if (finalW != contentW) {
        const int h2 = layoutContent (finalW);
        content_.setSize (finalW, juce::jmax (h2, viewport_.getHeight()));
    }
    {   // one alignment system: the header adopts the body column (P2.9 T10 rule)
        const int cw = viewport_.getMaximumVisibleWidth();
        const int colW = juce::jmin (kContentMaxW, juce::jmax (0, cw - 2 * kGutter));
        const int x0 = (cw - colW) / 2;
        header_.setBounds (headerArea.withX (headerArea.getX() + x0).withWidth (colW));
    }
}

} // namespace eb
