#include "gui/CalSlotComponent.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

static juce::String typeName (eb::CalType t) {
    switch (t) {
        case eb::CalType::Hpn: return "HPN";
        case eb::CalType::Heq: return "HEQ";
        case eb::CalType::Raw: return "RAW";
        case eb::CalType::Idf: return "IDF";
        default:               return "?";
    }
}

// Draw a small rounded "chip" badge (used for the type / Required tags).
static void drawChip (juce::Graphics& g, juce::Rectangle<int> area, juce::Justification just,
                      const juce::String& text, juce::Colour bg, juce::Colour fg) {
    juce::Font font (juce::FontOptions (11.0f).withStyle ("Bold"));
    const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, text)) + 18;
    auto chip = area.withWidth (juce::jmin (w, area.getWidth()));
    if (just.testFlags (juce::Justification::right))
        chip = area.removeFromRight (juce::jmin (w, area.getWidth()));
    g.setColour (bg);
    g.fillRoundedRectangle (chip.toFloat(), 6.0f);
    g.setColour (fg);
    g.setFont (font);
    g.drawText (text, chip, juce::Justification::centred);
}

namespace {
// One source of truth for BOTH resized() and preferredHeight() - they must never disagree.
constexpr int kPadX = 16, kPadY = 12, kHeaderH = 26, kHeadGap = 10;
constexpr int kProblemH = 32, kProblemGap = 8;
constexpr int kDzIconH = 36, kDzMainH = 18, kDzBrowseH = 28, kDzBrowseW = 150, kDzReqH = 16;
constexpr int kDzGap = 10, kDzGapSm = 8, kDzMinBodyH = 150;
constexpr int kErrStripH = 44, kErrStripGap = 8;
constexpr int kThumbH = 84, kFileH = 18, kSerialH = 16, kNoteH = 34, kNoteGap = 6;
constexpr int kBtnRowH = 28, kBtnGap = 8;
} // namespace

CalSlotComponent::CalSlotComponent (juce::String name) : earName (std::move (name)) {
    setTitle (earName + " calibration");   // accessible name (VoiceOver)
    addAndMakeVisible (thumbnail);
    thumbnail.setTitle (earName + " calibration curve");   // M5/L1: per-ear image name
    thumbnail.setVisible (false);

    fileLabel.setColour (juce::Label::textColourId, Theme::textDim());
    fileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    fileLabel.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (fileLabel);

    replaceBtn.setButtonText ("Replace...");                  // opens a chooser -> ellipsis verb
    serialLabel.setColour (juce::Label::textColourId, Theme::textDim());
    serialLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    serialLabel.setJustificationType (juce::Justification::centredLeft);
    serialLabel.setComponentID ("calSerial");
    addChildComponent (serialLabel);

    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    errorLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    errorLabel.setJustificationType (juce::Justification::topLeft);   // wrap warnings to 2 lines, top-aligned
    errorLabel.setMinimumHorizontalScale (1.0f);                      // wrap rather than squish the text
    fileLabel.setComponentID ("calFile");                             // findable in the layout regression test
    errorLabel.setComponentID ("calWarn");
    addChildComponent (errorLabel);

    // Swap banner: bold red, wraps, sits directly under the title. Distinct from errorLabel so a
    // gate-detected L/R swap is always visible and never buried under a type/parse warning.
    problemLabel.setColour (juce::Label::textColourId, Theme::danger());
    problemLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
    problemLabel.setJustificationType (juce::Justification::topLeft);
    problemLabel.setMinimumHorizontalScale (1.0f);                    // wrap rather than squish
    problemLabel.setComponentID ("calProblem");                       // findable in tests
    addChildComponent (problemLabel);

    replaceBtn.onClick = [this] { browseForCal(); };
    addChildComponent (replaceBtn);

    removeBtn.onClick = [this] { clearCal(); };
    addChildComponent (removeBtn);

    // P2 empty state (spec 5.2 / firstrun frame): real child controls, not painted text, so the
    // design gate can measure them. Clicks fall through the labels to the whole-body browse.
    dzMain.setText ("Drop the " + earName.toLowerCase() + " file here", juce::dontSendNotification);
    dzMain.setColour (juce::Label::textColourId, Theme::text());
    dzMain.setFont (juce::Font (juce::FontOptions (13.0f)));
    dzMain.setJustificationType (juce::Justification::centred);
    dzMain.setComponentID ("calDzMain");
    dzMain.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (dzMain);

    browseBtn.setButtonText ("Browse " + earName.toLowerCase() + "...");   // per-ear: a11y + dup gate
    browseBtn.onClick = [this] { browseForCal(); };
    addAndMakeVisible (browseBtn);

    dzReq.setText ("Required - load both ears to measure", juce::dontSendNotification);
    dzReq.setColour (juce::Label::textColourId, Theme::warn());
    dzReq.setFont (juce::Font (juce::FontOptions (11.5f)));
    dzReq.setJustificationType (juce::Justification::centred);
    dzReq.setComponentID ("calDzReq");
    dzReq.setInterceptsMouseClicks (false, false);
    addChildComponent (dzReq);

    refreshStateVisibility();
}

bool CalSlotComponent::isInterestedInFileDrag (const juce::StringArray& files) {
    for (auto& f : files) {
        auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") return true;
    }
    return false;
}

void CalSlotComponent::fileDragEnter (const juce::StringArray&, int, int) { dragHover = true;  repaint(); }
void CalSlotComponent::fileDragExit  (const juce::StringArray&)          { dragHover = false; repaint(); }

void CalSlotComponent::filesDropped (const juce::StringArray& files, int, int) {
    dragHover = false;
    for (auto& f : files) {
        juce::File file (f);
        auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") { loadFromFile (file); break; }
    }
    repaint();
}

void CalSlotComponent::mouseUp (const juce::MouseEvent& e) {
    if (! cal && e.mouseWasClicked()) browseForCal();   // empty card: whole body browses
}

void CalSlotComponent::browseForCal() {
    chooser = std::make_unique<juce::FileChooser> ("Choose a calibration file (.txt / .frd)",
                                                   juce::File(), "*.txt;*.frd");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f.existsAsFile()) loadFromFile (f);
        });
}

bool CalSlotComponent::loadFromFile (const juce::File& file) {
    if (! file.existsAsFile()) {
        errorLabel.setColour (juce::Label::textColourId, Theme::danger());   // real error, not a warning
        errorLabel.setText ("File not found: " + file.getFileName(), juce::dontSendNotification);
        errorLabel.setVisible (true);
        // #38: a PERSISTED path can go stale (file moved/deleted). Without a visible Remove the user had no
        // way to clear it from the UI - the error card re-appeared every launch with Start locked closed.
        removeBtn.setVisible (true);
        refreshStateVisibility();
        resized(); repaint();
        if (onLayoutChanged) onLayoutChanged();
        return false;
    }
    // #9: cap the accepted size BEFORE reading. Real EARS/FRD cals are a few KB; a multi-hundred-MB file at a
    // persisted path (a renamed recording, a runaway export) would read the whole thing into one String on the
    // message thread, copy it again in parse(), and could throw a std::bad_alloc that isn't a CalParseError ->
    // terminate. This path is reloaded from Settings on EVERY launch, so it must fail gracefully, not freeze.
    if (file.getSize() > 10 * 1024 * 1024) {
        errorLabel.setColour (juce::Label::textColourId, Theme::danger());
        errorLabel.setText ("File too large (" + juce::File::descriptionOfSizeInBytes (file.getSize())
                            + ") - not an EARS calibration file.", juce::dontSendNotification);
        errorLabel.setVisible (true);
        removeBtn.setVisible (true);
        refreshStateVisibility();
        resized(); repaint();
        if (onLayoutChanged) onLayoutChanged();
        return false;
    }
    try {
        auto parsed = eb::CalFile::parse (file.loadFileAsString());
        // Second ear-side signal: the FILENAME. The content side (parse()) is authoritative; the
        // filename only FILLS an Unknown content side, and a definite disagreement is recorded but
        // does NOT override the content (a file whose body says RIGHT stays RIGHT even if misnamed).
        const auto fnSide = eb::sideFromFilename (file.getFileName());
        if (parsed.side == eb::CalSide::Unknown && fnSide != eb::CalSide::Unknown) {
            parsed.side = fnSide;
        } else if (parsed.side != eb::CalSide::Unknown && fnSide != eb::CalSide::Unknown
                   && parsed.side != fnSide) {
            auto sideWord = [] (eb::CalSide s) { return s == eb::CalSide::Left ? "LEFT" : "RIGHT"; };
            parsed.parseWarnings.add (juce::String ("Filename says ") + sideWord (fnSide)
                                      + " but the file content says " + sideWord (parsed.side)
                                      + " - keeping the content side.");
        }
        applyParsed (parsed, file);
        if (onCalLoaded) onCalLoaded (file);
        return true;
    } catch (const eb::CalParseError& e) {
        errorLabel.setColour (juce::Label::textColourId, Theme::danger());   // real error, not a warning
        errorLabel.setText (juce::String ("Parse error: ") + e.what(), juce::dontSendNotification);
        errorLabel.setVisible (true);
        removeBtn.setVisible (true);   // #38: same clear affordance for a corrupt persisted file
        refreshStateVisibility();
        resized(); repaint();
        if (onLayoutChanged) onLayoutChanged();
        return false;
    }
}

void CalSlotComponent::applyParsed (const eb::CalFile& parsed, const juce::File& file) {
    cal = parsed;
    typeTag = typeName (parsed.type);
    rawCaution = (parsed.type == eb::CalType::Raw);
    fileLabel.setText (file.getFileName(), juce::dontSendNotification);
    serialLabel.setText ("Serial " + (parsed.serial.isNotEmpty() ? parsed.serial
                                                                 : juce::String ("not found")),
                         juce::dontSendNotification);
    // Per-file CAUTIONS stay on the card (they are about THIS file). Per-TYPE guidance (HEQ /
    // HPN) moved to the ONE stage-level caption - spec 5.2: it used to render once per card,
    // twice for a pair (the "duplicated warning"). CalibrateStage::stageCaptionFor owns it now.
    if (parsed.type == eb::CalType::Raw) {
        errorLabel.setColour (juce::Label::textColourId, Theme::warn());
        errorLabel.setText ("RAW is mic-capsule-only with no compensation (miniDSP marks it unsupported). "
                            "Use HEQ for Dirac unless you specifically need RAW.", juce::dontSendNotification);
        errorLabel.setVisible (true);
    } else if (parsed.type == eb::CalType::Unknown) {
        errorLabel.setColour (juce::Label::textColourId, Theme::warn());
        errorLabel.setText ("Couldn't identify this calibration type - confirm it's an EARS HEQ file.",
                            juce::dontSendNotification);
        errorLabel.setVisible (true);
    } else {   // Heq / Hpn / Idf - no on-card note (the stage caption carries the pair guidance)
        errorLabel.setText ({}, juce::dontSendNotification);
        errorLabel.setVisible (false);
    }
    // #54: surface parse warnings that were previously WRITE-ONLY (the filename/content side-conflict message
    // + skipped/non-monotonic-row notes). Show the first on the card when no type note already occupies the
    // label (amber); always hand them to the owner to LOG (skipped-row text can embed header content, so the
    // owner logs through its redacting logLine). '+N more' when several.
    if (! parsed.parseWarnings.isEmpty()) {
        if (! errorLabel.isVisible()) {
            const auto more = parsed.parseWarnings.size() > 1
                            ? " (+" + juce::String (parsed.parseWarnings.size() - 1) + " more)" : juce::String();
            errorLabel.setColour (juce::Label::textColourId, Theme::warn());
            errorLabel.setText (parsed.parseWarnings[0] + more, juce::dontSendNotification);
            errorLabel.setVisible (true);
        } else {
            // A type note (HEQ/HPN/RAW - the common cases) already owns the label: append a short pointer so
            // the warnings are still VISIBLE on the card, not only in the log (C+D verifier MINOR on #54).
            errorLabel.setText (errorLabel.getText() + " (" + juce::String (parsed.parseWarnings.size())
                                + " parse warning" + (parsed.parseWarnings.size() > 1 ? "s" : "") + " - see log.)",
                                juce::dontSendNotification);
        }
        if (onParseWarnings) onParseWarnings (parsed.parseWarnings);
    }
    thumbnail.setCalFile (parsed);
    thumbnail.setVisible (true);
    fileLabel.setVisible (true);
    serialLabel.setVisible (true);
    replaceBtn.setVisible (true);
    removeBtn.setVisible (true);
    refreshStateVisibility();
    resized();
    repaint();
    if (onLayoutChanged) onLayoutChanged();
}

void CalSlotComponent::clearCal() {
    cal = std::nullopt;
    typeTag = {};
    rawCaution = false;
    fileLabel.setText ({}, juce::dontSendNotification);
    serialLabel.setText ({}, juce::dontSendNotification);
    errorLabel.setText ({}, juce::dontSendNotification);
    problemLabel.setText ({}, juce::dontSendNotification);
    thumbnail.clear();
    thumbnail.setVisible (false);
    fileLabel.setVisible (false);
    serialLabel.setVisible (false);
    errorLabel.setVisible (false);
    problemLabel.setVisible (false);   // an emptied slot has no swap to warn about
    replaceBtn.setVisible (false);
    removeBtn.setVisible (false);
    refreshStateVisibility();
    resized();
    repaint();
    if (onCalCleared) onCalCleared();
    if (onLayoutChanged) onLayoutChanged();
}

void CalSlotComponent::setProblem (const juce::String& message) {
    const bool show = message.isNotEmpty();
    if (show == problemLabel.isVisible() && problemLabel.getText() == message)
        return;   // no-op: avoids a relayout/repaint storm from the per-tick gate refresh
    problemLabel.setColour (juce::Label::textColourId, Theme::danger());
    problemLabel.setText (message, juce::dontSendNotification);
    problemLabel.setVisible (show);
    resized();
    repaint();
    if (onLayoutChanged) onLayoutChanged();   // problemLabel is a preferredHeight() input - the guard above already suppressed no-op repeats
}

void CalSlotComponent::refreshStateVisibility() {
    const bool empty = ! cal.has_value();
    dzMain.setVisible (empty);
    browseBtn.setVisible (empty);
    dzReq.setVisible (empty && siblingLoaded);
    setMouseCursor (empty ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void CalSlotComponent::setSiblingLoaded (bool other) {
    if (siblingLoaded == other) return;
    siblingLoaded = other;
    refreshStateVisibility();
    resized(); repaint();
    if (onLayoutChanged) onLayoutChanged();
}

int CalSlotComponent::preferredHeight() const {
    if (cal) {
        int h = kPadY + kHeaderH + kHeadGap;
        if (problemLabel.isVisible()) h += kProblemH + kProblemGap;
        h += kThumbH + 6 + kFileH + 2 + kSerialH;
        if (errorLabel.isVisible()) h += kNoteGap + kNoteH;
        h += 8 + kBtnRowH;
        return h + kPadY;
    }
    int h = kPadY + kHeaderH + kHeadGap;
    if (problemLabel.isVisible()) h += kProblemH + kProblemGap;
    int body = kDzGap + kDzIconH + kDzGapSm + kDzMainH + kDzGap + kDzBrowseH;
    if (dzReq.isVisible()) body += kDzGapSm + kDzReqH;
    h += juce::jmax (kDzMinBodyH, body);
    if (errorLabel.isVisible()) h += kErrStripGap + kErrStripH;   // #38 stale-path error strip
    return h + kPadY;
}

// L6: the drop zone answers HOVER like it answers a file-drag (accent dashes). Keyboard focus
// rides the Browse button (a TextButton paints the LnF focus treatment).
void CalSlotComponent::mouseEnter (const juce::MouseEvent&) { if (! cal) { mouseHover = true; repaint(); } }
void CalSlotComponent::mouseExit  (const juce::MouseEvent&) { mouseHover = false; repaint(); }

void CalSlotComponent::setPlotRange (float topDb) { thumbnail.setRange (topDb); }

void CalSlotComponent::applyTheme() {
    fileLabel.setColour (juce::Label::textColourId, Theme::textDim());
    serialLabel.setColour (juce::Label::textColourId, Theme::textDim());
    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    problemLabel.setColour (juce::Label::textColourId, Theme::danger());
    dzMain.setColour (juce::Label::textColourId, Theme::text());
    dzReq.setColour  (juce::Label::textColourId, Theme::warn());
    thumbnail.repaint();
    repaint();
}

void CalSlotComponent::resized() {
    auto r = getLocalBounds().reduced (kPadX, kPadY);
    r.removeFromTop (kHeaderH);   // header strip (painted)
    r.removeFromTop (kHeadGap);

    // The swap banner (setProblem) sits directly under the title, ABOVE the thumbnail/empty-state,
    // so it's the first thing read on the card and never overlaps the filename or type warning.
    if (problemLabel.isVisible()) {
        problemLabel.setBounds (r.removeFromTop (kProblemH));
        r.removeFromTop (kProblemGap);
    }

    if (cal) {
        thumbnail.setBounds (r.removeFromTop (kThumbH));
        r.removeFromTop (6);
        fileLabel.setBounds (r.removeFromTop (kFileH));
        r.removeFromTop (2);
        serialLabel.setBounds (r.removeFromTop (kSerialH));
        if (errorLabel.isVisible()) {
            r.removeFromTop (kNoteGap);
            errorLabel.setBounds (r.removeFromTop (kNoteH));
        }
        r.removeFromTop (8);
        auto btns = r.removeFromTop (kBtnRowH);
        replaceBtn.setBounds (btns.removeFromRight (100));
        btns.removeFromRight (kBtnGap);
        removeBtn.setBounds (btns.removeFromRight (88));
    } else {
        // EMPTY: #38 error strip first (bottom), then the centred drop-zone stack. dzIconArea is
        // recorded for paint() so the glyph and the labels can never drift apart.
        if (errorLabel.isVisible()) {
            auto strip = r.removeFromBottom (kErrStripH);
            if (removeBtn.isVisible()) {
                removeBtn.setBounds (strip.removeFromRight (84).withSizeKeepingCentre (84, 28));
                strip.removeFromRight (10);
            }
            errorLabel.setBounds (strip);
            r.removeFromBottom (kErrStripGap);
        }
        int stackH = kDzIconH + kDzGapSm + kDzMainH + kDzGap + kDzBrowseH;
        if (dzReq.isVisible()) stackH += kDzGapSm + kDzReqH;
        auto stack = r.withSizeKeepingCentre (r.getWidth(), stackH);
        dzIconArea = stack.removeFromTop (kDzIconH);
        stack.removeFromTop (kDzGapSm);
        dzMain.setBounds (stack.removeFromTop (kDzMainH));
        stack.removeFromTop (kDzGap);
        browseBtn.setBounds (stack.removeFromTop (kDzBrowseH).withSizeKeepingCentre (kDzBrowseW, kDzBrowseH));
        if (dzReq.isVisible()) {
            stack.removeFromTop (kDzGapSm);
            dzReq.setBounds (stack.removeFromTop (kDzReqH));
        }
    }
}

void CalSlotComponent::paint (juce::Graphics& g) {
    auto full = getLocalBounds().toFloat();
    g.setColour (Theme::surface());
    g.fillRoundedRectangle (full, 10.0f);

    auto inner = getLocalBounds().reduced (kPadX, kPadY);
    auto header = inner.removeFromTop (kHeaderH);

    // Ear name: the frame's 12px tracked uppercase ear label (the old 15px bold read as a second
    // stage title at card scale).
    g.setColour (Theme::text());
    g.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")).withExtraKerningFactor (0.04f));
    g.drawText (earName.toUpperCase(), header, juce::Justification::centredLeft);

    // Status badge (right).
    if (cal) {
        if (rawCaution) drawChip (g, header, juce::Justification::right, "RAW",
                                  Theme::warn().withAlpha (0.18f), Theme::warn());
        else            drawChip (g, header, juce::Justification::right, typeTag,
                                  Theme::infoBg(), Theme::infoText());
    } else {
        // Start is allowed with no cal (unity passthrough), so the cal is RECOMMENDED, not required. Keep the
        // amber warn tint as a visible nudge: an uncalibrated measurement is not corrected for the EARS mic.
        drawChip (g, header, juce::Justification::right, "Recommended",
                  Theme::warn().withAlpha (0.16f), Theme::warn());
    }

    // Empty state: a dashed drop zone filling the body.
    if (! cal) {
        inner.removeFromTop (kHeadGap);
        auto body = inner.toFloat();
        // Error state (#38 follow-up): the bottom strip holds the error text + the Remove button - carve it
        // out of the drop zone so the dashes/hint never paint underneath them (verifier MINOR).
        if (errorLabel.isVisible())
            body.removeFromBottom ((float) kErrStripH);
        if (dragHover) {   // a file is over the zone: tint the fill before stroking the accent dashes
            g.setColour (Theme::accent().withAlpha (0.06f));
            g.fillRoundedRectangle (body.reduced (0.5f), 8.0f);
        }
        juce::Path dash;
        dash.addRoundedRectangle (body.reduced (0.5f), 8.0f);
        g.setColour ((dragHover || mouseHover) ? Theme::accent() : Theme::sep2());   // L6: hover == drag
        const float dashes[] = { 6.0f, 5.0f };
        juce::Path stroked;
        juce::PathStrokeType (1.5f).createDashedStroke (stroked, dash, dashes, 2);
        g.fillPath (stroked);

        // The drop glyph (a down-arrow into a tray) - centred in dzIconArea, which resized() records
        // so the glyph and the child labels below it can never drift apart.
        if (! dzIconArea.isEmpty()) {
            auto ib = dzIconArea.toFloat().withSizeKeepingCentre (36.0f, 36.0f);
            juce::Path p;                                            // down-arrow into a tray
            p.startNewSubPath (ib.getCentreX(), ib.getY() + 4.0f);
            p.lineTo          (ib.getCentreX(), ib.getY() + 22.0f);
            p.startNewSubPath (ib.getCentreX() - 7.0f, ib.getY() + 15.0f);
            p.lineTo          (ib.getCentreX(),        ib.getY() + 22.0f);
            p.lineTo          (ib.getCentreX() + 7.0f, ib.getY() + 15.0f);
            p.startNewSubPath (ib.getX() + 5.0f,  ib.getY() + 26.0f);
            p.lineTo          (ib.getX() + 5.0f,  ib.getY() + 30.0f);
            p.lineTo          (ib.getRight() - 5.0f, ib.getY() + 30.0f);
            p.lineTo          (ib.getRight() - 5.0f, ib.getY() + 26.0f);
            g.setColour (Theme::textDim());                          // promoted off tertiary (HIG)
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }
    }
}

} // namespace eb
