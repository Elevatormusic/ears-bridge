#include "gui/CalSlotComponent.h"
#include "gui/Theme.h"

namespace eb {

static juce::String typeName (eb::CalType t) {
    switch (t) {
        case eb::CalType::Hpn: return "HPN";
        case eb::CalType::Heq: return "HEQ";
        case eb::CalType::Raw: return "RAW";
        case eb::CalType::Idf: return "IDF";
        default:               return "Unknown";
    }
}

CalSlotComponent::CalSlotComponent (juce::String title) : slotTitle (std::move (title)) {
    addAndMakeVisible (thumbnail);

    fileLabel.setColour (juce::Label::textColourId, Theme::text());
    fileLabel.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    fileLabel.setText ("Drop a cal file (.txt / .frd)", juce::dontSendNotification);
    addAndMakeVisible (fileLabel);

    detailLabel.setColour (juce::Label::textColourId, Theme::textDim());
    detailLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (detailLabel);

    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    errorLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (errorLabel);
}

bool CalSlotComponent::isInterestedInFileDrag (const juce::StringArray& files) {
    for (auto& f : files) {
        auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") return true;
    }
    return false;
}

void CalSlotComponent::fileDragEnter (const juce::StringArray&, int, int) {
    dragHover = true; repaint();
}
void CalSlotComponent::fileDragExit (const juce::StringArray&) {
    dragHover = false; repaint();
}

void CalSlotComponent::filesDropped (const juce::StringArray& files, int, int) {
    dragHover = false;
    for (auto& f : files) {
        juce::File file (f);
        auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") { loadFromFile (file); break; }
    }
    repaint();
}

bool CalSlotComponent::loadFromFile (const juce::File& file) {
    if (! file.existsAsFile()) {
        errorLabel.setText ("File not found: " + file.getFileName(), juce::dontSendNotification);
        repaint();
        return false;
    }
    try {
        auto parsed = eb::CalFile::parse (file.loadFileAsString());
        applyParsed (parsed, file);
        if (onCalLoaded) onCalLoaded (file);
        return true;
    } catch (const eb::CalParseError& e) {
        // Keep any previously valid curve; surface the error with context.
        errorLabel.setText (juce::String ("Parse error: ") + e.what(), juce::dontSendNotification);
        repaint();
        return false;
    }
}

void CalSlotComponent::applyParsed (const eb::CalFile& parsed, const juce::File& file) {
    cal = parsed;
    errorLabel.setText ({}, juce::dontSendNotification);
    fileLabel.setText (file.getFileName(), juce::dontSendNotification);
    juce::String detail = "Serial " + (parsed.serial.isNotEmpty() ? parsed.serial : juce::String ("?"))
                        + "  -  " + typeName (parsed.type);
    detailLabel.setText (detail, juce::dontSendNotification);
    heqWarning = (parsed.type == eb::CalType::Heq);
    thumbnail.setCalFile (parsed);
    repaint();
}

void CalSlotComponent::resized() {
    auto r = getLocalBounds().reduced (8);
    r.removeFromTop (18);                 // title strip (painted)
    auto top = r.removeFromTop (20);
    fileLabel.setBounds (top);
    detailLabel.setBounds (r.removeFromTop (16));
    errorLabel.setBounds (r.removeFromTop (14));
    r.removeFromTop (4);
    thumbnail.setBounds (r);
}

void CalSlotComponent::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (dragHover ? Theme::accent() : Theme::outline());
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, dragHover ? 2.0f : 1.0f);

    // Title.
    g.setColour (Theme::textDim());
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    g.drawText (slotTitle, getLocalBounds().reduced (10, 6).removeFromTop (16),
                juce::Justification::topLeft);

    // HEQ-as-mic-cal warning badge (non-blocking).
    if (heqWarning) {
        auto badge = getLocalBounds().reduced (8).removeFromTop (18)
                         .removeFromRight (132).toFloat();
        g.setColour (Theme::warn());
        g.fillRoundedRectangle (badge, 4.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
        g.drawText ("HEQ - double-target?", badge, juce::Justification::centred);
    }
}

} // namespace eb
