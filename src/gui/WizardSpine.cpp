#include "gui/WizardSpine.h"
#include "gui/Theme.h"
#include "gui/Glyphs.h"

namespace eb {

// ---- geometry (mirrors the .spine block; all in logical px) -------------------------------------
namespace {
constexpr int kPadX       = 16;   // .spine padding-left/right
constexpr int kPadTop     = 22;
constexpr int kNode       = 24;   // node circle diameter
constexpr int kGap        = 12;   // node <-> body gap
constexpr int kRowPadX    = 8;    // .step padding
constexpr int kRowPadY    = 10;
constexpr int kRowGap     = 2;    // .step + .step margin-top
constexpr int kRowHeight  = 84;   // node + title + 2-line meta + "You are here" tag + padding
constexpr int kEyebrowH   = 24;
constexpr int kFootGap    = 22;   // .spine-foot margin-top
constexpr int kFootRowH   = 18;

const juce::String kStepNames[kWizardStepCount] = { "Connect", "Calibrate", "Level", "Measure" };

juce::String stateWord (StepState s) {
    switch (s) {
        case StepState::Done:    return "done";
        case StepState::Active:  return "current step";
        case StepState::Error:   return "needs attention";
        case StepState::Blocked: return "blocked";
        case StepState::Todo:    return "to do";
    }
    return "to do";
}

// The clickable-affordance clause appended to a navigable row's a11y title.
juce::String clickClause (WizardStep step) {
    switch (step) {
        case WizardStep::Connect:   return "click to change devices";
        case WizardStep::Calibrate: return "click to swap calibration files";
        case WizardStep::Level:     return "click to re-adjust the level";
        case WizardStep::Measure:   return "click to run the measurement";
    }
    return "click to open";
}

// Set a label's text/colour only if it actually changed (the repo's set-if-changed discipline;
// see MainComponent::setLabelIfChanged). Avoids needless repaints on every setState.
bool setLabelIfChanged (juce::Label& label, const juce::String& text, juce::Colour colour) {
    const bool textChanged   = label.getText() != text;
    const bool colourChanged = label.findColour (juce::Label::textColourId) != colour;
    if (! textChanged && ! colourChanged) return false;
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, colour);
    return true;
}
} // namespace

// ================================================================================================
// StepRow — one navigable step: a node circle (painted), a title + meta label, keyboard-focusable.
// The node fill/glyph is drawn here from Theme tokens; the two labels carry the text so the design
// probe can measure/contrast them. A Blocked row is disabled (setEnabled(false)) and inert.
// ================================================================================================
struct WizardSpine::StepRow : public juce::Component {
    WizardSpine& owner;
    const int    index;          // 0..3
    StepState    state = StepState::Todo;
    bool         isActive = false;
    bool         isLastRow = false;

    // Repaint discipline (M-1): the spine is ALWAYS visible and setState runs at 30 Hz from
    // refreshWizardView, so an unconditional row->repaint() there re-invalidates all four rows forever
    // (see the set-if-changed comment at :44-46 — the same discipline, now extended to the paint call).
    // Every repaint this row issues funnels through markDirty() so setState can repaint a row ONLY when
    // something it actually renders changed, and tests can observe the count.
    int repaintCount = 0;
    void markDirty() { ++repaintCount; repaint(); }

    juce::Label  title, meta, tag;   // tag = the "You are here" pill text on the active row

    StepRow (WizardSpine& o, int i) : owner (o), index (i) {
        setWantsKeyboardFocus (true);         // Return/Space activation (a11y: button-like)
        setFocusContainerType (juce::Component::FocusContainerType::none);

        for (auto* l : { &title, &meta, &tag }) {
            l->setInterceptsMouseClicks (false, false);
            l->setMinimumHorizontalScale (1.0f);   // never squash — let the probe catch true overflow
        }
        title.setJustificationType (juce::Justification::centredLeft);
        meta .setJustificationType (juce::Justification::topLeft);
        tag  .setJustificationType (juce::Justification::topLeft);
        title.setFont (juce::Font (juce::FontOptions (13.5f)));
        meta .setFont (juce::Font (juce::FontOptions (11.5f)));
        tag  .setFont (juce::Font (juce::FontOptions (11.0f)));
        addAndMakeVisible (title);
        addAndMakeVisible (meta);
        addChildComponent (tag);              // shown only on the active row
    }

    bool navigable() const {
        return state != StepState::Blocked;   // Done/Error/Todo/Active are navigable; Blocked is not
    }

    void resized() override {
        const int bodyX = kRowPadX + kNode + kGap;
        const int bodyW = juce::jmax (0, getWidth() - bodyX - kRowPadX);
        // meta is a TWO-LINE box so a long device name / reason wraps instead of clipping (mirrors
        // the prototype's .step-meta line-height:1.35, no nowrap). The probe measures the wrap.
        title.setBounds (bodyX, kRowPadY,      bodyW, 18);
        meta .setBounds (bodyX, kRowPadY + 20, bodyW, 32);
        tag  .setBounds (bodyX, kRowPadY + 54, bodyW, 15);
    }

    void paint (juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();

        // Active row: a filled ctrl() pill behind the whole row (the "You are here" surface).
        if (isActive) {
            g.setColour (Theme::ctrl());
            g.fillRoundedRectangle (r.reduced (1.0f), 10.0f);
        }

        // Keyboard-focus ring (HIG M4) — visible only when this row holds focus.
        if (hasKeyboardFocus (false)) {
            g.setColour (Theme::accent());
            g.drawRoundedRectangle (r.reduced (1.0f), 10.0f, 2.0f);
        }

        // Node circle.
        const float nodeX = (float) kRowPadX;
        const float nodeY = (float) kRowPadY;
        const juce::Rectangle<float> node (nodeX, nodeY, (float) kNode, (float) kNode);

        juce::Colour fill, border, glyphCol;
        const bool dimmed = (state == StepState::Blocked);
        switch (state) {
            case StepState::Done:
                fill = Theme::okFill(); border = fill; glyphCol = Theme::okFill().darker (0.85f); break;
            case StepState::Active:
                // W2 .step.active .node = --accent-fill (white glyph 4.81:1)
                fill = Theme::primaryFill(); border = fill; glyphCol = Theme::onAccentText(); break;
            case StepState::Error:
                fill = Theme::dangerFill(); border = fill; glyphCol = Theme::onAccentText(); break;
            case StepState::Blocked:
            case StepState::Todo:
            default:
                fill = Theme::ctrl(); border = Theme::sep2();
                glyphCol = dimmed ? Theme::textFaint() : Theme::textDim(); break;
        }

        g.setColour (fill);   g.fillEllipse (node);
        g.setColour (border); g.drawEllipse (node.reduced (0.5f), 1.0f);

        g.setColour (glyphCol);
        if (state == StepState::Done) {
            eb::glyph::drawTick (g, node.reduced (5.5f), glyphCol);   // 13px tick in the 24px node (W2 node tick)
        } else if (state == StepState::Error) {
            g.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            g.drawText ("!", node, juce::Justification::centred, false);
        } else {
            g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            g.drawText (juce::String (index + 1), node, juce::Justification::centred, false);
        }

        // Connector below the node (2px), ok-tinted below a Done node. Not drawn under the last row.
        if (! isLastRow) {
            const float connX = nodeX + (float) kNode * 0.5f - 1.0f;
            const float connY = nodeY + (float) kNode + 2.0f;
            const float connB = (float) getHeight() + (float) kRowGap;   // reach into the inter-row gap
            g.setColour (state == StepState::Done ? Theme::okFill().withAlpha (0.45f) : Theme::sep());
            g.fillRect (connX, connY, 2.0f, juce::jmax (0.0f, connB - connY));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override {
        if (navigable() && getLocalBounds().contains (e.getPosition()))
            owner.rowActivated (index);
    }

    bool keyPressed (const juce::KeyPress& key) override {
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
            if (navigable()) { owner.rowActivated (index); return true; }
        }
        return false;
    }

    void focusGained (juce::Component::FocusChangeType) override { markDirty(); }
    void focusLost   (juce::Component::FocusChangeType) override { markDirty(); }
};

// ================================================================================================
WizardSpine::WizardSpine() {
    // A11y: the spine is a focus container grouping the step rows.
    setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
    setTitle ("Setup steps");

    for (int i = 0; i < kWizardStepCount; ++i) {
        auto* row = new StepRow (*this, i);
        row->isLastRow = (i == kWizardStepCount - 1);
        rows.add (row);
        addAndMakeVisible (row);
    }

    auto setupFootLabel = [] (juce::Label& l, juce::Justification j, float pt) {
        l.setInterceptsMouseClicks (false, false);
        l.setJustificationType (j);
        l.setMinimumHorizontalScale (1.0f);
        l.setFont (juce::Font (juce::FontOptions (pt)));
    };
    setupFootLabel (refLabel,  juce::Justification::centredLeft,  11.5f);
    setupFootLabel (refValue,  juce::Justification::centredRight, 11.5f);
    setupFootLabel (refLabel2, juce::Justification::centredLeft,  11.5f);
    setupFootLabel (refValue2, juce::Justification::centredRight, 11.5f);
    refLabel.setText ("Reference",    juce::dontSendNotification);
    refLabel2.setText ("Learned from", juce::dontSendNotification);
    for (auto* l : { &refLabel, &refValue, &refLabel2, &refValue2 })
        addAndMakeVisible (*l);
}

WizardSpine::~WizardSpine() = default;

void WizardSpine::resized() {
    const int contentX = kPadX;
    const int contentW = juce::jmax (0, getWidth() - 2 * kPadX);
    int y = kPadTop + kEyebrowH;

    for (auto* row : rows) {
        row->setBounds (contentX, y, contentW, kRowHeight);
        y += kRowHeight + kRowGap;
    }

    // Footer: two rows pinned below the last step (with the .spine-foot gap + hairline above).
    int fy = y + kFootGap;
    const int footX = contentX + kRowPadX;
    const int footW = juce::jmax (0, contentW - 2 * kRowPadX);
    refLabel .setBounds (footX, fy, footW, kFootRowH);
    refValue .setBounds (footX, fy, footW, kFootRowH);
    fy += kFootRowH + 4;
    refLabel2.setBounds (footX, fy, footW, kFootRowH);
    refValue2.setBounds (footX, fy, footW, kFootRowH);
}

void WizardSpine::paint (juce::Graphics& g) {
    g.fillAll (Theme::rail());

    // Right-edge hairline (.spine border-right).
    g.setColour (Theme::sep());
    g.fillRect ((float) getWidth() - 1.0f, 0.0f, 1.0f, (float) getHeight());

    // Eyebrow.
    g.setColour (Theme::textFaint());
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    g.drawText ("CALIBRATION FLOW", kPadX + kRowPadX, kPadTop, getWidth() - 2 * kPadX, kEyebrowH,
                juce::Justification::centredLeft, false);

    // Footer hairline (above the two ref rows).
    if (! rows.isEmpty()) {
        const int lastBottom = rows.getLast()->getBottom();
        const float hy = (float) (lastBottom + kFootGap - 8);
        g.setColour (Theme::sep());
        g.fillRect ((float) (kPadX + kRowPadX), hy,
                    (float) juce::jmax (0, getWidth() - 2 * (kPadX + kRowPadX)), 1.0f);
    }

    // P2.9: a "matched" tick before the reference value, only when the ref status is positive (W2 .matched).
    if (refPositive_ && refValue.isVisible() && refValue.getText().isNotEmpty()) {
        const juce::Font f (juce::FontOptions (11.5f));
        const float tw = juce::GlyphArrangement::getStringWidth (f, refValue.getText());
        const auto  rb = refValue.getBounds().toFloat();
        eb::glyph::drawTick (g, { rb.getRight() - tw - 17.0f, rb.getCentreY() - 6.0f, 12.0f, 12.0f }, Theme::ok());
    }
}

void WizardSpine::setState (const WizardState& ws, const juce::String viewMetas[kWizardStepCount],
                            const juce::String& refLine1, const juce::String& refLine2) {
    for (int i = 0; i < kWizardStepCount; ++i) {
        auto* row = rows[i];
        const auto& st = ws.steps[i];
        const bool isActiveStep = ((int) ws.active == i);

        // M-1: repaint a row only when something it PAINTS changed. The node fill/glyph is a function of
        // `state`; the active pill is a function of `isActive`; the label text/colours are tracked by the
        // set-if-changed writes below. Anything else (a11y title, tag VISIBILITY) is not painted by this
        // component, so it never forces a repaint. Track the state/activeness deltas before overwriting.
        const bool stateChanged  = (row->state != st.state);
        const bool activeChanged = (row->isActive != isActiveStep);

        row->state    = st.state;
        row->isActive = isActiveStep;

        const bool navigable = (st.state != StepState::Blocked);
        row->setEnabled (navigable);                     // Blocked -> dimmed + not clickable
        row->setWantsKeyboardFocus (navigable);

        // Title colour: dim upcoming/blocked rows.
        const bool upcoming = (st.state == StepState::Todo || st.state == StepState::Blocked)
                           && ! isActiveStep;
        bool labelChanged = setLabelIfChanged (row->title, kStepNames[i],
                                               upcoming ? Theme::textDim() : Theme::text());

        // Meta line: a non-empty viewMeta overrides the machine reason (DONE summaries the view owns).
        // The active step's "You are here" affordance renders as a SEPARATE tag label (mirrors the
        // prototype's .step-meta vs .step-tag split), so the meta text stays the pure summary/reason.
        const juce::String metaText = viewMetas[i].isNotEmpty() ? viewMetas[i] : st.reason;
        const juce::Colour metaCol  = (st.state == StepState::Done) ? Theme::textDim() : Theme::textFaint();
        labelChanged |= setLabelIfChanged (row->meta, metaText, metaCol);

        row->tag.setVisible (isActiveStep);
        // P2.9: the tag is the accent anchor - accentText() is 4.5:1-safe in both themes (was infoText during the frozen-Theme era).
        labelChanged |= setLabelIfChanged (row->tag, isActiveStep ? juce::String ("You are here") : juce::String(),
                                           Theme::accentText());

        // A11y title: "Step N of 4, <name>, <state word>[, <click clause>]".
        juce::String t;
        t << "Step " << (i + 1) << " of " << kWizardStepCount << ", " << kStepNames[i]
          << ", " << stateWord (st.state);
        if (navigable)
            t << ", " << clickClause ((WizardStep) i);
        if (row->getTitle() != t) row->setTitle (t);

        // Repaint ONLY on an actual visual delta (M-1) — a Label's own text/colour setter already
        // invalidated its child bounds, so the row-level repaint is needed only for what THIS paint()
        // draws (node + active pill). Without this guard the always-visible spine repainted at 30 Hz.
        if (stateChanged || activeChanged || labelChanged)
            row->markDirty();
    }

    // Reference footer. Tone the value by CONTENT: ok() (green) only for a POSITIVE reference status
    // ("matched"/"learned"/"loaded"); textDim() for a neutral/negative one ("not learned", "n/a ...").
    // (The old unconditional ok() painted "not learned" green, reading like a success — a lie.) "not
    // learned" contains "learned", so the negative "not " prefix must veto the positive match.
    const bool refNegative = refLine1.containsIgnoreCase ("not ") || refLine1.startsWithIgnoreCase ("n/a");
    const bool refPositive = ! refNegative
                          && (refLine1.containsIgnoreCase ("matched")
                              || refLine1.containsIgnoreCase ("learned")
                              || refLine1.containsIgnoreCase ("loaded"));
    if (refPositive != refPositive_) { refPositive_ = refPositive; repaint(); }   // repaint-discipline: only on flip
    setLabelIfChanged (refValue,  refLine1, refPositive ? Theme::ok() : Theme::textDim());
    setLabelIfChanged (refValue2, refLine2, Theme::textFaint());
    setLabelIfChanged (refLabel2, refLine2.isNotEmpty() ? juce::String ("Learned from") : juce::String(),
                       Theme::textFaint());
    refLabel .setColour (juce::Label::textColourId, Theme::textDim());
}

void WizardSpine::rowActivated (int step) {
    if (step < 0 || step >= kWizardStepCount) return;
    if (rows[step]->state == StepState::Blocked) return;   // inert
    if (onStepClicked) onStepClicked ((WizardStep) step);
}

// ---- test-only seams ---------------------------------------------------------------------------
int WizardSpine::rowCountForTest() const { return rows.size(); }

bool WizardSpine::rowEnabledForTest (int step) const {
    return step >= 0 && step < rows.size() && rows[step]->isEnabled();
}

juce::String WizardSpine::rowTitleForTest (int step) const {
    return (step >= 0 && step < rows.size()) ? rows[step]->getTitle() : juce::String();
}

juce::String WizardSpine::rowMetaForTest (int step) const {
    return (step >= 0 && step < rows.size()) ? rows[step]->meta.getText() : juce::String();
}

void WizardSpine::clickStepForTest (int step) { rowActivated (step); }

juce::Colour WizardSpine::tagColourForTest() const {
    for (auto* r : rows) if (r->isActive) return r->tag.findColour (juce::Label::textColourId);
    return {};
}

// The number of repaints this row has issued through markDirty() (M-1). A second setState with the
// SAME inputs must NOT increment it — the always-visible spine used to repaint unconditionally at 30 Hz.
int WizardSpine::rowRepaintCountForTest (int step) const {
    return (step >= 0 && step < rows.size()) ? rows[step]->repaintCount : -1;
}

} // namespace eb
