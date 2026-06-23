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

CalSlotComponent::CalSlotComponent (juce::String name) : earName (std::move (name)) {
    setTitle (earName + " calibration");   // accessible name (VoiceOver)
    addAndMakeVisible (thumbnail);
    thumbnail.setVisible (false);

    fileLabel.setColour (juce::Label::textColourId, Theme::textDim());
    fileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    fileLabel.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (fileLabel);

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
        errorLabel.setVisible (true); repaint();
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
        errorLabel.setVisible (true); repaint();
        return false;
    }
}

void CalSlotComponent::applyParsed (const eb::CalFile& parsed, const juce::File& file) {
    cal = parsed;
    typeTag = typeName (parsed.type);
    heqWarning = (parsed.type == eb::CalType::Heq);
    fileLabel.setText (file.getFileName()
                       + "  -  serial " + (parsed.serial.isNotEmpty() ? parsed.serial : juce::String ("?")),
                       juce::dontSendNotification);
    // Explain the two cal-type hazards that otherwise load silently behind only a tiny chip.
    if (parsed.type == eb::CalType::Heq) {
        errorLabel.setColour (juce::Label::textColourId, Theme::warn());
        errorLabel.setText ("HEQ bakes in a headphone target - for Dirac load the HPN (or RAW) file, "
                            "or you'll double-correct.", juce::dontSendNotification);
        errorLabel.setVisible (true);
    } else if (parsed.type == eb::CalType::Unknown) {
        errorLabel.setColour (juce::Label::textColourId, Theme::warn());
        errorLabel.setText ("Couldn't identify this calibration type - confirm it's the right HPN file.",
                            juce::dontSendNotification);
        errorLabel.setVisible (true);
    } else {
        errorLabel.setText ({}, juce::dontSendNotification);
        errorLabel.setVisible (false);
    }
    thumbnail.setCalFile (parsed);
    thumbnail.setVisible (true);
    fileLabel.setVisible (true);
    replaceBtn.setVisible (true);
    removeBtn.setVisible (true);
    resized();
    repaint();
}

void CalSlotComponent::clearCal() {
    cal = std::nullopt;
    typeTag = {};
    heqWarning = false;
    fileLabel.setText ({}, juce::dontSendNotification);
    errorLabel.setText ({}, juce::dontSendNotification);
    problemLabel.setText ({}, juce::dontSendNotification);
    thumbnail.clear();
    thumbnail.setVisible (false);
    fileLabel.setVisible (false);
    errorLabel.setVisible (false);
    problemLabel.setVisible (false);   // an emptied slot has no swap to warn about
    replaceBtn.setVisible (false);
    removeBtn.setVisible (false);
    resized();
    repaint();
    if (onCalCleared) onCalCleared();
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
}

void CalSlotComponent::setPlotRange (float topDb) { thumbnail.setRange (topDb); }

void CalSlotComponent::applyTheme() {
    fileLabel.setColour (juce::Label::textColourId, Theme::textDim());
    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    problemLabel.setColour (juce::Label::textColourId, Theme::danger());
    thumbnail.repaint();
    repaint();
}

void CalSlotComponent::resized() {
    auto r = getLocalBounds().reduced (14, 12);
    r.removeFromTop (26);   // header strip (painted)
    r.removeFromTop (10);

    // The swap banner (setProblem) sits directly under the title, ABOVE the thumbnail/empty-state,
    // so it's the first thing read on the card and never overlaps the filename or type warning.
    if (problemLabel.isVisible()) {
        problemLabel.setBounds (r.removeFromTop (32));
        r.removeFromTop (8);
    }

    if (cal) {
        auto meta = r.removeFromBottom (28);
        replaceBtn.setBounds (meta.removeFromRight (96));
        meta.removeFromRight (8);
        removeBtn.setBounds (meta.removeFromRight (84));
        meta.removeFromRight (10);
        fileLabel.setBounds (meta);
        // A type warning (HEQ / unidentified) gets its OWN line directly above the filename row, so it
        // never lands on top of the filename label. It used to share `meta`'s bounds, so a HEQ or
        // unknown-type file drew the warning and the filename in the same rectangle -> unreadable.
        // The space is only carved out when a warning is actually visible (clean cards keep the room).
        if (errorLabel.isVisible()) {
            r.removeFromBottom (8);
            errorLabel.setBounds (r.removeFromBottom (34));
        }
        r.removeFromBottom (10);
        thumbnail.setBounds (r);
    } else if (errorLabel.isVisible()) {
        // Empty slot with a load/parse error (no thumbnail): show it along the bottom of the body.
        errorLabel.setBounds (r.removeFromBottom (34));
    }
}

void CalSlotComponent::paint (juce::Graphics& g) {
    auto full = getLocalBounds().toFloat();
    g.setColour (Theme::surface());
    g.fillRoundedRectangle (full, 10.0f);

    auto inner = getLocalBounds().reduced (14, 12);
    auto header = inner.removeFromTop (26);

    // Ear name.
    g.setColour (Theme::text());
    g.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
    g.drawText (earName, header, juce::Justification::centredLeft);

    // Status badge (right).
    if (cal) {
        if (heqWarning) drawChip (g, header, juce::Justification::right, "HEQ",
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
        inner.removeFromTop (10);
        auto body = inner.toFloat();
        juce::Path dash;
        dash.addRoundedRectangle (body.reduced (0.5f), 8.0f);
        g.setColour (dragHover ? Theme::accent() : Theme::sep2());
        const float dashes[] = { 6.0f, 5.0f };
        juce::Path stroked;
        juce::PathStrokeType (1.5f).createDashedStroke (stroked, dash, dashes, 2);
        g.fillPath (stroked);

        g.setColour (Theme::text());
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        auto t = body.removeFromTop (body.getHeight() * 0.5f + 8.0f);
        g.drawText ("Drop a calibration file", t.toNearestInt(), juce::Justification::centredBottom);
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("FRD text file, or click to browse",
                    body.toNearestInt(), juce::Justification::centredTop);
    }
}

} // namespace eb
