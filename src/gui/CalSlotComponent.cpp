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
    addChildComponent (errorLabel);

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
        errorLabel.setText ("File not found: " + file.getFileName(), juce::dontSendNotification);
        errorLabel.setVisible (true); repaint();
        return false;
    }
    try {
        auto parsed = eb::CalFile::parse (file.loadFileAsString());
        applyParsed (parsed, file);
        if (onCalLoaded) onCalLoaded (file);
        return true;
    } catch (const eb::CalParseError& e) {
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
    errorLabel.setText ({}, juce::dontSendNotification);
    errorLabel.setVisible (false);
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
    thumbnail.clear();
    thumbnail.setVisible (false);
    fileLabel.setVisible (false);
    errorLabel.setVisible (false);
    replaceBtn.setVisible (false);
    removeBtn.setVisible (false);
    resized();
    repaint();
    if (onCalCleared) onCalCleared();
}

void CalSlotComponent::setPlotRange (float topDb) { thumbnail.setRange (topDb); }

void CalSlotComponent::applyTheme() {
    fileLabel.setColour (juce::Label::textColourId, Theme::textDim());
    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    thumbnail.repaint();
    repaint();
}

void CalSlotComponent::resized() {
    auto r = getLocalBounds().reduced (14, 12);
    r.removeFromTop (26);   // header strip (painted)
    r.removeFromTop (10);

    if (cal) {
        auto meta = r.removeFromBottom (28);
        replaceBtn.setBounds (meta.removeFromRight (96));
        meta.removeFromRight (8);
        removeBtn.setBounds (meta.removeFromRight (84));
        meta.removeFromRight (10);
        fileLabel.setBounds (meta);
        errorLabel.setBounds (meta);
        r.removeFromBottom (10);
        thumbnail.setBounds (r);
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
        drawChip (g, header, juce::Justification::right, "Required",
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
