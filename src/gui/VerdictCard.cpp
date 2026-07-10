#include "gui/VerdictCard.h"
#include "gui/Theme.h"
#include "gui/Glyphs.h"
#include <cmath>

namespace eb {
namespace {
constexpr int kPadX = 16, kPadY = 12;             // house card pad (frame's 18 yields to the fold budget)
constexpr int kHeadH = 24, kGradeH = 30, kMetricsH = 34, kFixLeadH = 16, kFixBodyH = 32;
constexpr int kDetailsH = 24, kChipH = 24, kChipGap = 6, kGap = 8;
// P3 Task 7 ruling (controller): the VIEW shows at most this many logical observation lines; beyond
// it the label reads "+N more - hover Details for the full list". A VIEW cap, never a model cap: the
// model keeps FULL evidence (operator== 30 Hz guard, the model pins, the tooltip/a11y mirrors below).
// 4 is MEASURED against the pathological all-findings card at the 508px Measure viewport - the fit is
// a HARD gate (test_hig_layout "RULE2a pathological open"), not an estimate.
constexpr int kObsCap = 4;

juce::Colour toneText (StatusTone t) {
    switch (t) {
        case StatusTone::Ok:     return Theme::ok();
        case StatusTone::Warn:   return Theme::warn();
        case StatusTone::Danger: return Theme::danger();
        default:                 return Theme::textDim();
    }
}

// Measured text width, house idiom (Font::getStringWidth is [[deprecated]] in the vendored JUCE 8.0.4).
int textW (const juce::Label& l) {
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (l.getFont(), l.getText()));
}

// Measured WRAPPED text height for a Label, mirroring the probe's textOverflows math (same usable
// width: label width minus the Label border insets) so a label sized by this can never be flagged
// as clipped by the design gate. Returns 0 for empty text / degenerate width.
int wrappedTextHeight (const juce::Label& l, int width) {
    if (l.getText().isEmpty()) return 0;
    const auto  border  = l.getBorderSize();
    const float usableW = (float) width - (float) (border.getLeft() + border.getRight());
    if (usableW <= 1.0f) return 0;
    juce::AttributedString as; as.append (l.getText(), l.getFont());
    juce::TextLayout tl; tl.createLayout (as, usableW);
    return (int) std::ceil (tl.getHeight()) + border.getTop() + border.getBottom() + 2;
}

// The metrics row is hidden when NO metric was measured (all three read the em dash: the hardware-
// Dirac and Re-learn variants) - three identical dashes inform nobody and two equal-width twins
// would trip the design gate's duplicate-label rule.
bool metricsShown (const VerdictCardModel& m) {
    const auto dash = verdictDash();
    return m.snrVal.isNotEmpty()
        && ! (m.snrVal == dash && m.irVal == dash && m.thdVal == dash);
}
} // namespace

VerdictCard::VerdictCard() {
    setFocusContainerType (juce::Component::FocusContainerType::none);
    earName_.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")).withExtraKerningFactor (0.04f));
    badge_.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    badge_.setJustificationType (juce::Justification::centred);
    grade_.setFont (juce::Font (juce::FontOptions (26.0f).withStyle ("Bold")));
    qualifier_.setFont (juce::Font (juce::FontOptions (13.0f)));
    for (int i = 0; i < 3; ++i) {
        mLabel_[i].setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")).withExtraKerningFactor (0.04f));
        mVal_[i].setFont (juce::Font (juce::FontOptions (15.0f)));
    }
    mLabel_[0].setText ("SNR", juce::dontSendNotification);
    mLabel_[1].setText ("IR-SNR", juce::dontSendNotification);
    mLabel_[2].setText ("THD", juce::dontSendNotification);
    fixLead_.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
    fixBody_.setFont (juce::Font (juce::FontOptions (12.0f)));
    fixBody_.setJustificationType (juce::Justification::topLeft);
    fixBody_.setMinimumHorizontalScale (1.0f);                 // wrap, never squish (probe catches overflow)
    for (auto& c : chip_) c.setFont (juce::Font (juce::FontOptions (11.5f)));
    observations_.setFont (juce::Font (juce::FontOptions (12.0f)));
    observations_.setJustificationType (juce::Justification::topLeft);
    observations_.setMinimumHorizontalScale (1.0f);            // same wrap-never-squish rule as the fix body
    staleTag_.setFont (juce::Font (juce::FontOptions (11.0f)));
    for (auto* l : { &earName_, &badge_, &grade_, &qualifier_, &fixLead_, &fixBody_ }) addAndMakeVisible (*l);
    for (auto& l : mLabel_) addAndMakeVisible (l);
    for (auto& l : mVal_)   addAndMakeVisible (l);
    addAndMakeVisible (details_);
    for (auto& c : chip_) addChildComponent (c);               // visible only while details are open
    addChildComponent (observations_);
    addChildComponent (staleTag_);
    details_.onOpenChanged = [this] (bool nowOpen) {
        resized(); if (onLayoutChanged) onLayoutChanged();
        // P4: the disclosed content fades in AT FINAL LAYOUT (the T2-upheld pattern - geometry lands
        // instantly, only paint eases). Open only; closing snaps (frozen decision 2).
        if (nowOpen) detailsRamp_.start(); else detailsRamp_.snapToEnd();
    };
    detailsRamp_.onTick = [this] {
        const float v = detailsRamp_.value();
        // HONESTY EXCLUSION (P4 Task 3 ruling): a warn-FLAGGED chip carries a finding + its value -
        // it must be readable the MOMENT the layout lands, so it never rides the fade. Only the
        // passing chips and the neutral-toned observations ease in.
        for (int i = 0; i < kVerdictChipCount; ++i)
            chip_[i].setAlpha (model_.chips[i].flagged ? 1.0f : v);
        observations_.setAlpha (v);
        repaint();                                   // chip BACKGROUNDS are painted surfaces; they ride v too
    };
    applyTheme();
}

void VerdictCard::applyTheme() {
    earName_.setColour (juce::Label::textColourId, Theme::textDim());
    grade_.setColour (juce::Label::textColourId, Theme::text());
    qualifier_.setColour (juce::Label::textColourId, Theme::textDim());
    for (auto& l : mLabel_) l.setColour (juce::Label::textColourId, Theme::textFaint());
    observations_.setColour (juce::Label::textColourId, Theme::textDim());
    staleTag_.setColour (juce::Label::textColourId, Theme::textDim());
    setModel (model_);   // re-derive the tone-mapped colours (badge/fix/metric flags) for the new palette
}

void VerdictCard::setModel (const VerdictCardModel& m) {
    const int before = layoutRows (getWidth(), false);   // height under the OLD model (no bounds applied)
    model_ = m;
    earName_.setText (m.earName, juce::dontSendNotification);
    badge_.setText (m.badge, juce::dontSendNotification);
    badge_.setColour (juce::Label::textColourId, toneText (m.badgeTone));
    grade_.setText (m.gradeWord, juce::dontSendNotification);
    qualifier_.setText (m.qualifier, juce::dontSendNotification);
    const juce::String* vals[3] = { &m.snrVal, &m.irVal, &m.thdVal };
    const bool flags[3] = { m.snrFlag, m.irFlag, m.thdFlag };
    for (int i = 0; i < 3; ++i) {
        mVal_[i].setText (*vals[i], juce::dontSendNotification);
        mVal_[i].setColour (juce::Label::textColourId, flags[i] ? Theme::warn() : Theme::text());
    }
    fixLead_.setText (m.fixLead, juce::dontSendNotification);
    fixLead_.setColour (juce::Label::textColourId, m.fixTone == StatusTone::Dim ? Theme::text() : toneText (m.fixTone));
    fixBody_.setText (m.fixBody, juce::dontSendNotification);
    fixBody_.setColour (juce::Label::textColourId, Theme::textDim());
    details_.setSummary (m.tally);
    details_.setVisible (! m.hwDirac);
    for (int i = 0; i < kVerdictChipCount; ++i) {
        chip_[i].setText (m.chips[i].flagged && m.chips[i].value.isNotEmpty()
                              ? m.chips[i].label + juce::String (" ") + m.chips[i].value
                              : juce::String (m.chips[i].label),
                          juce::dontSendNotification);
        chip_[i].setColour (juce::Label::textColourId, m.chips[i].flagged ? Theme::warn() : Theme::textDim());
        // A chip whose flagged-ness changes MID-FADE must obey the exclusion immediately (warn tone
        // changes snap - never wait for the next 60 Hz tick). At rest value() == 1.0: a no-op.
        chip_[i].setAlpha (m.chips[i].flagged ? 1.0f : detailsRamp_.value());
    }
    // Observation cap (kObsCap, ruling above). ORDER IS LOAD-BEARING: compose the FULL join FIRST,
    // hand it to the tooltip + accessibility help, THEN set the capped label - reading the label back
    // into the tooltip (the pre-cap idiom) would silently break tooltip-carries-all the day the cap
    // landed. anyProv scans the FULL list: a provisional finding beyond the cap must still raise the
    // footer. "+N more" counts logical observations only (the footer is not an observation).
    juce::StringArray full;
    bool anyProv = false;
    for (const auto& o : m.observations) {
        full.add (o.provisional ? o.text + "  [provisional]" : o.text);
        anyProv = anyProv || o.provisional;
    }
    juce::StringArray shown = full;
    if (full.size() > kObsCap) {
        shown.removeRange (kObsCap, shown.size() - kObsCap);
        shown.add ("+" + juce::String (full.size() - kObsCap) + " more - hover Details for the full list");
    }
    if (anyProv) {
        const juce::String footer = "Provisional findings aren't ratified on hardware yet - shown for information only.";
        full.add (footer);
        shown.add (footer);
    }
    const juce::String fullText = full.joinIntoString ("\n");
    // Collapsed-state tooltip parity (spec 6): the FULL observation text rides the details row's hover
    // tooltip (discoverable while collapsed AND past the cap) and its accessibility help (capped
    // findings must never be hover-only for a screen reader). Pinned in test_verdictcard.cpp.
    details_.setTooltip (fullText);
    details_.setHelpText (fullText);
    observations_.setText (shown.joinIntoString ("\n"), juce::dontSendNotification);
    staleTag_.setText (m.stale ? "from your previous configuration" : juce::String(), juce::dontSendNotification);
    staleTag_.setVisible (m.stale && inlineStaleTag_);   // in-context the stage strip speaks (see header)
    setAlpha (m.stale ? 0.55f : 1.0f);              // 5.4: evidence dimmed, never deleted (static, never animated)
    setDescription (m.earName + ", " + m.badge + (m.stale ? ", from your previous configuration" : ""));
    resized();
    repaint();
    // Any height delta (growth OR shrink: observations, chips wrap, stale tag, hw variant) must
    // reach the owner - one exact source (layoutRows) instead of a change-flag heuristic.
    if (onLayoutChanged && layoutRows (getWidth(), false) != before) onLayoutChanged();
}

void VerdictCard::setDetailsOpen (bool open) { details_.setOpen (open); }

void VerdictCard::setInlineStaleTagEnabled (bool on) {
    if (inlineStaleTag_ == on) return;
    inlineStaleTag_ = on;
    staleTag_.setVisible (model_.stale && on);
    resized();
    if (onLayoutChanged) onLayoutChanged();          // the tag row claims/releases 14px
}

int VerdictCard::layoutRows (int width, bool apply) {
    auto rr = juce::Rectangle<int> (0, 0, width, 100000).reduced (kPadX, 0);
    rr.removeFromTop (kPadY);
    const auto place = [&] (juce::Component& c, int h) { auto b = rr.removeFromTop (h); if (apply) c.setBounds (b); return b; };
    {   auto head = rr.removeFromTop (kHeadH);
        if (apply) {
            const int bw = juce::jmax (64, textW (badge_) + 24);
            badgeBg_ = head.removeFromRight (bw).withSizeKeepingCentre (bw, 20);
            badge_.setBounds (badgeBg_);
            earName_.setBounds (head);
        }
    }
    rr.removeFromTop (kGap);
    {   auto row = rr.removeFromTop (kGradeH);
        if (apply) {
            // +12 = the Label's own 5+5 border insets + margin, so the measured word can never read
            // as clipped (the probe compares the string width against width-minus-border).
            const int gw = textW (grade_) + 12;
            grade_.setBounds (row.removeFromLeft (juce::jmin (gw, row.getWidth())));
            row.removeFromLeft (8);
            qualifier_.setBounds (row);
        }
    }
    const bool metrics = metricsShown (model_);
    if (metrics) {
        rr.removeFromTop (kGap);
        auto row = rr.removeFromTop (kMetricsH);
        if (apply) {
            const int cw = row.getWidth() / 3;
            for (int i = 0; i < 3; ++i) {
                auto col = row.removeFromLeft (cw).withTrimmedLeft (i == 0 ? 0 : 12);
                mLabel_[i].setBounds (col.removeFromTop (13));
                mVal_[i].setBounds (col);
            }
        }
    }
    if (apply)
        for (int i = 0; i < 3; ++i) { mLabel_[i].setVisible (metrics); mVal_[i].setVisible (metrics); }
    rr.removeFromTop (kGap);
    if (apply) fixRuleY_ = rr.getY();
    rr.removeFromTop (1);                                       // the hairline above the note
    rr.removeFromTop (6);
    place (fixLead_, kFixLeadH);
    // MEASURED wrapped height, floored at the frozen 2-line reserve (P3 Task 7, T2-review gate):
    // the Red-SNR / clip action bodies run 3 lines at the 273px card width - a fixed slot clipped
    // the Danger card's action line (a trust bug). Never copy surgery; the geometry follows the text.
    place (fixBody_, juce::jmax (kFixBodyH, wrappedTextHeight (fixBody_, rr.getWidth())));
    if (staleTag_.isVisible()) place (staleTag_, 14);
    const bool open = details_.isVisible() && details_.isOpen();
    if (details_.isVisible()) place (details_, kDetailsH);
    if (open) {
        rr.removeFromTop (kChipGap);
        // Chip flow layout: measured widths, wrap at the column edge. Box = 18px tick gutter + text
        // + right pad; the +34 also covers the Label's 5+5 border so the text never reads clipped.
        int x = 0, rowY = rr.getY();
        for (int i = 0; i < kVerdictChipCount; ++i) {
            const int cw = textW (chip_[i]) + 34;
            if (x > 0 && x + cw > rr.getWidth()) { x = 0; rowY += kChipH + kChipGap; }
            if (apply) {
                chipBg_[i] = { rr.getX() + x, rowY, cw, kChipH };
                chip_[i].setBounds (chipBg_[i].withTrimmedLeft (18).withTrimmedRight (4));
                chip_[i].setVisible (true);
            }
            x += cw + kChipGap;
        }
        const int chipsBottom = rowY + kChipH;
        rr.removeFromTop (chipsBottom - rr.getY());
        // Observations claim their MEASURED wrapped height (a fixed per-line height would clip the
        // longer finding lines at the 273px card width - the full findings are the point of Details).
        const int obsH = wrappedTextHeight (observations_, rr.getWidth());
        if (obsH > 0) {
            rr.removeFromTop (kGap);
            const auto b = rr.removeFromTop (obsH);
            if (apply) { observations_.setBounds (b); observations_.setVisible (true); }
        } else if (apply) {
            observations_.setVisible (false); observations_.setBounds ({});
        }
    } else if (apply) {
        for (auto& c : chip_) { c.setVisible (false); c.setBounds ({}); }
        observations_.setVisible (false); observations_.setBounds ({});
    }
    rr.removeFromTop (kPadY);
    return rr.getY();
}

int  VerdictCard::preferredHeight (int width) const { return const_cast<VerdictCard*> (this)->layoutRows (width, false); }
void VerdictCard::resized()                          { layoutRows (getWidth(), true); }

void VerdictCard::paint (juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    Theme::paintCardSurface (g, r);                             // the ONE card recipe
    if (model_.graded && model_.badgeTone == StatusTone::Warn)
        { g.setColour (Theme::warnFill().withAlpha (0.38f));   g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f); }
    else if (model_.graded && model_.badgeTone == StatusTone::Danger)
        { g.setColour (Theme::dangerFill().withAlpha (0.38f)); g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f); }
    if (! badgeBg_.isEmpty()) {                                 // badge capsule bg (Label carries the text)
        const auto tint = toneText (model_.badgeTone);
        g.setColour (tint.withAlpha (0.16f));
        g.fillRoundedRectangle (badgeBg_.toFloat(), badgeBg_.getHeight() * 0.5f);
    }
    g.setColour (Theme::sep());                                 // the hairline above the fix note
    if (fixRuleY_ > 0) g.fillRect (kPadX, fixRuleY_, getWidth() - 2 * kPadX, 1);
    // (metric-column separators: the 12px column trim carries the frame's separation; add 1px sep()
    //  lines at the two column edges ONLY if the T9 eyeball says the columns read as one blob)
    if (details_.isVisible() && details_.isOpen()) {
        // P4: the painted chip surfaces ride the details fade (withMultipliedAlpha - chipBg carries its
        // own intrinsic alpha). The warn-flagged fill/border do NOT: the honesty exclusion above - a
        // warn surface lands at full strength the moment the layout lands.
        const float dv = detailsRamp_.value();
        for (int i = 0; i < kVerdictChipCount; ++i)
            if (! chipBg_[i].isEmpty()) {
                const bool f = model_.chips[i].flagged;
                g.setColour (f ? Theme::warnFill().withAlpha (0.14f) : Theme::chipBg().withMultipliedAlpha (dv));
                g.fillRoundedRectangle (chipBg_[i].toFloat(), 8.0f);
                if (f) { g.setColour (Theme::warnFill().withAlpha (0.32f));
                         g.drawRoundedRectangle (chipBg_[i].toFloat().reduced (0.5f), 8.0f, 1.0f); }
                else eb::glyph::drawTick (g, juce::Rectangle<float> ((float) chipBg_[i].getX() + 5.0f,
                                              (float) chipBg_[i].getCentreY() - 5.5f, 11.0f, 11.0f),
                                          Theme::okFill().withMultipliedAlpha (dv));
            }
    }
}

} // namespace eb
