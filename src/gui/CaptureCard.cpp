#include "gui/CaptureCard.h"
#include "gui/Theme.h"

namespace eb {

CaptureCardModel CaptureCardModel::waiting (const char* ear) {
    return { State::Waiting, ear, "Waiting", "Next in the routine",
             "Auto per-ear runs one earcup at a time - this sweep follows automatically.", "Queued", -1.0f };
}
CaptureCardModel CaptureCardModel::capturing (const char* ear, float fraction, int sweepSeconds) {
    return { State::Capturing, ear, "Capturing", "Sweeping now",
             "Dirac is playing the sweep through this earcup. It grades the moment the capture ends.",
             "~" + juce::String (sweepSeconds) + " s sweep", juce::jlimit (0.0f, 1.0f, fraction) };
}
CaptureCardModel CaptureCardModel::failed (const char* ear) {
    return { State::Failed, ear, "Failed", "Capture interrupted",
             "EARS disconnected mid-sweep. This sweep can't be graded - reconnect, then measure again.",
             {}, -1.0f };
}

float CaptureCard::captureFraction (int elapsedTicks, double sweepSeconds) {
    if (sweepSeconds <= 0.0) return 0.0f;                    // unknown duration: claim nothing
    return juce::jlimit (0.0f, 1.0f, (float) ((double) elapsedTicks / (30.0 * sweepSeconds)));
}

CaptureCard::CaptureCard() {
    ear_.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")).withExtraKerningFactor (0.04f));
    badge_.setFont (juce::Font (juce::FontOptions (11.5f).withStyle ("Bold")));
    badge_.setJustificationType (juce::Justification::centredRight);
    title_.setFont (juce::Font (juce::FontOptions (19.0f).withStyle ("Bold")));
    sub_.setFont (juce::Font (juce::FontOptions (12.0f)));
    sub_.setJustificationType (juce::Justification::topLeft);
    sub_.setMinimumHorizontalScale (1.0f);
    foot_.setFont (juce::Font (juce::FontOptions (11.0f)));
    for (auto* l : { &ear_, &badge_, &title_, &sub_, &foot_ }) addAndMakeVisible (*l);
    applyTheme();
}

void CaptureCard::applyTheme() {
    ear_.setColour (juce::Label::textColourId, Theme::textDim());
    sub_.setColour (juce::Label::textColourId, Theme::textDim());
    foot_.setColour (juce::Label::textColourId, Theme::textFaint());
    applyModelTone();                                        // re-map the state-toned badge/title colours
    repaint();                                               // the border/track colours are paint-time reads
}

void CaptureCard::applyModelTone() {
    badge_.setColour (juce::Label::textColourId,
                      model_.state == CaptureCardModel::State::Capturing ? Theme::accentText()
                    : model_.state == CaptureCardModel::State::Failed    ? Theme::danger()
                                                                         : Theme::textFaint());
    title_.setColour (juce::Label::textColourId,
                      model_.state == CaptureCardModel::State::Failed ? Theme::danger() : Theme::text());
}

void CaptureCard::setModel (const CaptureCardModel& m) {
    if (m == model_) return;                                 // 30 Hz feed: repaint only on a real change
    model_ = m;
    ear_.setText (m.ear, juce::dontSendNotification);
    badge_.setText (m.badge, juce::dontSendNotification);
    title_.setText (m.title, juce::dontSendNotification);
    sub_.setText (m.sub, juce::dontSendNotification);
    foot_.setText (m.foot, juce::dontSendNotification);
    applyModelTone();
    setAlpha (m.state == CaptureCardModel::State::Waiting ? 0.55f : 1.0f);
    setDescription (m.ear + ", " + m.badge + ": " + m.title);
    repaint();
}

void CaptureCard::resized() {
    auto rr = getLocalBounds().reduced (16, 12);
    auto head = rr.removeFromTop (18);
    badge_.setBounds (head.removeFromRight (90));
    ear_.setBounds (head);
    rr.removeFromTop (10);
    title_.setBounds (rr.removeFromTop (24));
    rr.removeFromTop (4);
    // 48, not the plan's 32 (render-measured, ledgered deviation): the Capturing and Failed subs both
    // lay out at 3 lines of 12px in the 241px column - the probe flagged textOverflows and the render
    // showed the ellipsis at 32. Wrap-never-squish (scale 1.0) needs the third line.
    sub_.setBounds (rr.removeFromTop (48));
    rr.removeFromTop (10);
    progressArea_ = rr.removeFromTop (6);
    rr.removeFromTop (6);
    foot_.setBounds (rr.removeFromTop (14));
}

void CaptureCard::paint (juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    Theme::paintCardSurface (g, r);
    if (model_.state == CaptureCardModel::State::Capturing) {
        g.setColour (Theme::accent().withAlpha (0.45f));
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
    } else if (model_.state == CaptureCardModel::State::Failed) {
        g.setColour (Theme::dangerFill().withAlpha (0.38f));
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
    }
    if (! progressArea_.isEmpty() && model_.state != CaptureCardModel::State::Failed) {
        g.setColour (Theme::track());
        g.fillRoundedRectangle (progressArea_.toFloat(), 3.0f);
        if (model_.progress >= 0.0f) {
            auto fill = progressArea_.toFloat().withWidth (progressArea_.getWidth() * model_.progress);
            g.setColour (Theme::accent());
            g.fillRoundedRectangle (fill, 3.0f);
        }
    }
}

} // namespace eb
