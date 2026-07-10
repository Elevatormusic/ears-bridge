#include "gui/stages/CalibrateStage.h"
#include "gui/Theme.h"
#include "gui/Glyphs.h"
#include "gui/Copy.h"   // P4 T6: kDash typography
#include <cmath>   // P2.9 Task 7: std::abs (double) for the trim near-zero compare

namespace eb {

namespace {
constexpr int kGutter = 16;
constexpr int kContentMaxW = 760, kGridGap = 14;
constexpr int kDiscH = 24, kDiscIndent = 24;
constexpr int kSectionGap = 12;   // T8: slot grid -> caption/unity/disclosure column gap (value unchanged)
constexpr int kCaptionH = 44;
constexpr int kEyebrowH = 14, kComboH = 28;
} // namespace

CalibrateStage::CalibrateStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Calibrate");

    addAndMakeVisible (header_);
    header_.setRunNoteComponentID ("calRunNote");      // this stage owns the "cal"-prefixed run-note id
    header_.continueButton().onClick = [this] { if (onContinue) onContinue(); };

    viewport_.setViewedComponent (&content_, false);   // false: content_ is a member
    viewport_.setScrollBarsShown (true, false);
    viewport_.setScrollBarThickness (10);
    addAndMakeVisible (viewport_);

    content_.addChildComponent (caption_);             // shown while the caption text is nonempty
    content_.addAndMakeVisible (advancedFir_);
    advancedFir_.onOpenChanged = [this] (bool open) {
        resized();
        // P4 motion (frozen decision 2): the revealed section fades in AT its final layout; the
        // push-down itself is instant (the fit/displacement gates score end-states). Close is
        // instant — snapToEnd restores every alpha to 1 for the next open.
        if (open) revealRamp_.start(); else revealRamp_.snapToEnd();
    };
    revealRamp_.onTick = [this] {
        const float v = revealRamp_.value();
        for (juce::Component* c : { (juce::Component*) complexPhaseToggle_, (juce::Component*) firLenLabel_,
                                    (juce::Component*) firLenBox_, (juce::Component*) trimLabel_,
                                    (juce::Component*) trimSlider_ })
            if (c != nullptr) c->setAlpha (v);
    };

    // Unity path (spec 5.2): a warn-toned hint + the secondary "Continue without calibration"
    // button, both shown only while BOTH slots are empty (setUnityState drives visibility).
    unityHint_.setJustificationType (juce::Justification::centredLeft);
    unityHint_.setMinimumHorizontalScale (1.0f);
    unityHint_.setComponentID ("calUnityHint");
    content_.addChildComponent (unityHint_);
    unityBtn_.onClick = [this] { if (onContinueWithoutCal) onContinueWithoutCal(); };
    content_.addChildComponent (unityBtn_);
}

void CalibrateStage::setStageCaption (const juce::String& s) {
    if (caption_.text.getText() == s && caption_.isVisible() == s.isNotEmpty())
        return;
    caption_.text.setText (s, juce::dontSendNotification);
    caption_.setVisible (s.isNotEmpty());
    resized();
}

void CalibrateStage::setAdvancedOpen (bool open) {
    advancedFir_.setOpen (open);                       // fires onOpenChanged -> resized()
}

juce::String CalibrateStage::unityHintText (bool accepted) {
    return accepted ? "Continuing without calibration" + kDash + "the measurement won't be corrected for the EARS mic."
                    : "Calibration is recommended" + kDash + "without it the measurement isn't corrected for the EARS mic.";
}

void CalibrateStage::setUnityState (bool bothEmpty, bool accepted) {
    const juce::String msg = ! bothEmpty ? juce::String() : unityHintText (accepted);
    const bool showHint = bothEmpty;
    const bool showBtn  = bothEmpty && ! accepted;
    if (unityHint_.getText() == msg && unityHint_.isVisible() == showHint
        && unityBtn_.isVisible() == showBtn)
        return;                                               // per-tick no-op guard
    unityHint_.setText (msg, juce::dontSendNotification);
    unityHint_.setVisible (showHint);
    unityBtn_.setVisible (showBtn);
    resized();
}

juce::String CalibrateStage::stageCaptionFor (std::optional<CalType> l, std::optional<CalType> r) {
    if (! l.has_value() || ! r.has_value())
        return "Use HEQ files for headphones, IDF for IEMs" + kDash + "files are matched to left and right automatically.";
    const bool heq = (*l == CalType::Heq) || (*r == CalType::Heq);
    const bool hpn = (*l == CalType::Hpn) || (*r == CalType::Hpn);
    if (heq) return "HEQ curves include a mild bass boost" + kDash + "in Dirac, start from a flat bass target.";
    if (hpn) return "HPN is miniDSP's older curve" + kDash + "it works, but HEQ is now recommended for headphone EQ.";
    return {};   // IDF/RAW/Unknown pairs: the cards carry their own cautions
}

juce::String CalibrateStage::advancedFirSummary (bool complexPhase, int firLength, double trimDb) {
    juce::String s = complexPhase ? "Complex phase" : "Min phase";
    s += (firLength > 0) ? (kDash + juce::String (firLength) + " taps") : (kDash + "Auto length");
    s += kDash + juce::String (trimDb, 1) + " dB trim";
    return s;
}

bool CalibrateStage::advancedFirNonDefault (bool complexPhase, int firLength, double trimDb) {
    return complexPhase || firLength > 0 || std::abs (trimDb) >= 0.05;
}

void CalibrateStage::adopt (CalSlotComponent& leftCal, CalSlotComponent& rightCal,
                            juce::ToggleButton& complexPhaseToggle,
                            juce::Label& firLenLabel, juce::ComboBox& firLenBox,
                            juce::Label& trimLabel, juce::Slider& trimSlider) {
    leftCal_ = &leftCal;   content_.addAndMakeVisible (leftCal);
    rightCal_ = &rightCal; content_.addAndMakeVisible (rightCal);
    complexPhaseToggle_ = &complexPhaseToggle; content_.addChildComponent (complexPhaseToggle);
    firLenLabel_ = &firLenLabel;               content_.addChildComponent (firLenLabel);
    firLenBox_ = &firLenBox;                   content_.addChildComponent (firLenBox);
    trimLabel_ = &trimLabel;                   content_.addChildComponent (trimLabel);
    trimSlider_ = &trimSlider;                 content_.addChildComponent (trimSlider);

    // A card that grows (problem banner / caution / state flip) re-runs the grid so the two
    // cells stay uniform and nothing below overlaps (the P1 fixed-206 rows die here).
    leftCal.onLayoutChanged  = [this] { resized(); };
    rightCal.onLayoutChanged = [this] { resized(); };

    int fo = 1;
    leftCal.setExplicitFocusOrder            (fo++);
    rightCal.setExplicitFocusOrder           (fo++);
    unityBtn_.setExplicitFocusOrder          (fo++);
    advancedFir_.setExplicitFocusOrder       (fo++);
    complexPhaseToggle.setExplicitFocusOrder (fo++);
    firLenBox.setExplicitFocusOrder          (fo++);
    trimSlider.setExplicitFocusOrder         (fo++);
    header_.continueButton().setExplicitFocusOrder (fo++);
}

void CalibrateStage::applyTheme() {
    header_.applyTheme();
    caption_.applyTheme();
    unityHint_.setColour (juce::Label::textColourId, Theme::warn());
    unityHint_.setFont (juce::Font (juce::FontOptions (12.0f)));
}

void CalibrateStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
}

int CalibrateStage::layoutContent (int width) {
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, width - 2 * kGutter));
    const int x0   = (width - colW) / 2;
    auto rr = juce::Rectangle<int> (x0, 0, colW, 100000);
    // T10: no top pad (fold budget; the fixed header supplies the whitespace).

    if (leftCal_ != nullptr && rightCal_ != nullptr) {
        const int cellW = (colW - kGridGap) / 2;
        const int rowH  = juce::jmax (leftCal_->preferredHeight(), rightCal_->preferredHeight());
        auto row = rr.removeFromTop (rowH);
        leftCal_->setBounds (row.removeFromLeft (cellW));
        row.removeFromLeft (kGridGap);
        rightCal_->setBounds (row);
    }
    rr.removeFromTop (kSectionGap);

    if (caption_.isVisible()) {
        caption_.setBounds (rr.removeFromTop (kCaptionH));
        rr.removeFromTop (8);
    }

    if (unityHint_.isVisible()) {
        unityHint_.setBounds (rr.removeFromTop (16));
        rr.removeFromTop (4);
        if (unityBtn_.isVisible()) {
            unityBtn_.setBounds (rr.removeFromTop (28).removeFromLeft (240));
            rr.removeFromTop (8);
        } else {
            rr.removeFromTop (4);
        }
    } else {
        unityBtn_.setBounds ({});
    }

    advancedFir_.setBounds (rr.removeFromTop (kDiscH));
    const bool open = advancedFir_.isOpen();
    for (juce::Component* c : { (juce::Component*) complexPhaseToggle_, (juce::Component*) firLenLabel_,
                                (juce::Component*) firLenBox_, (juce::Component*) trimLabel_,
                                (juce::Component*) trimSlider_ })
        if (c != nullptr) c->setVisible (open);
    if (open) {
        rr.removeFromTop (4);
        if (complexPhaseToggle_ != nullptr) {
            complexPhaseToggle_->setBounds (rr.removeFromTop (24).withTrimmedLeft (kDiscIndent));
            rr.removeFromTop (4);
        }
        if (firLenLabel_ != nullptr) {
            firLenLabel_->setBounds (rr.removeFromTop (kEyebrowH).withTrimmedLeft (kDiscIndent));
            rr.removeFromTop (4);
        }
        if (firLenBox_ != nullptr) {
            auto row = rr.removeFromTop (kComboH).withTrimmedLeft (kDiscIndent);
            firLenBox_->setBounds (row.removeFromLeft (juce::jmin (280, row.getWidth())));
            rr.removeFromTop (8);
        }
        {   // P2.9: parameter row - "Output trim" label left, compact slider + value chip right.
            auto row = rr.removeFromTop (28).withTrimmedLeft (kDiscIndent);
            if (trimLabel_ != nullptr)  trimLabel_->setBounds (row.removeFromLeft (100));
            row.removeFromLeft (8);
            if (trimSlider_ != nullptr) trimSlider_->setBounds (row.removeFromLeft (juce::jmin (220, row.getWidth())));
        }
    } else {
        for (juce::Component* c : { (juce::Component*) complexPhaseToggle_, (juce::Component*) firLenLabel_,
                                    (juce::Component*) firLenBox_, (juce::Component*) trimLabel_,
                                    (juce::Component*) trimSlider_ })
            if (c != nullptr) c->setBounds ({});
    }
    return rr.getY() + 4;
}

void CalibrateStage::resized() {
    auto area = getLocalBounds();
    auto headerArea = area.removeFromTop (StageHeader::kHeight);
    viewport_.setBounds (area);
    const int contentW = viewport_.getMaximumVisibleWidth();
    const int contentH = layoutContent (contentW);
    content_.setSize (contentW, juce::jmax (contentH, viewport_.getHeight()));
    const int finalW = viewport_.getMaximumVisibleWidth();   // scrollbar may have toggled
    if (finalW != contentW) {
        const int h2 = layoutContent (finalW);
        content_.setSize (finalW, juce::jmax (h2, viewport_.getHeight()));
    }
    // P2.9 alignment unification: the header adopts the SAME centered content column the body just
    // laid out (one system: header text, cards, CTA share left/right edges at every width). Placed
    // AFTER the double-pass so a scrollbar toggle cannot leave the header 10px off the column.
    {
        const int cw = viewport_.getMaximumVisibleWidth();
        const int colW = juce::jmin (kContentMaxW, juce::jmax (0, cw - 2 * kGutter));
        const int x0 = (cw - colW) / 2;
        header_.setBounds (headerArea.withX (headerArea.getX() + x0).withWidth (colW));
    }
}

// ---- InfoCaption --------------------------------------------------------------------------------

CalibrateStage::InfoCaption::InfoCaption() {
    text.setJustificationType (juce::Justification::topLeft);
    text.setMinimumHorizontalScale (1.0f);                    // wrap to two lines, never squish
    text.setComponentID ("calStageCaption");
    addAndMakeVisible (text);
    applyTheme();
}

void CalibrateStage::InfoCaption::applyTheme() {
    text.setColour (juce::Label::textColourId, Theme::infoText());
    text.setFont (juce::Font (juce::FontOptions (12.0f)));
}

void CalibrateStage::InfoCaption::resized() {
    text.setBounds (getLocalBounds().reduced (12, 6).withTrimmedLeft (24));   // 32px text = 2 Callout lines
}

void CalibrateStage::InfoCaption::paint (juce::Graphics& g) {
    g.setColour (Theme::infoBg());
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    eb::glyph::drawInfo (g, { 12.0f, 10.0f, 15.0f, 15.0f }, Theme::infoText());   // shared recipe (identical geometry)
}

} // namespace eb
