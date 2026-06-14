#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "cal/CalFile.h"
#include "gui/CurveThumbnail.h"
#include <functional>
#include <optional>

namespace eb {

// One calibration slot ("Left ear cal" / "Right ear cal"). Accepts a dropped .txt/.frd
// file, parses it via eb::CalFile, shows the filename + parsed serial + type tag, draws
// the static FR thumbnail, and raises a non-blocking warning badge if an HEQ file is
// loaded as a mic-cal (double-target risk, design-spec §3.7). Fires onCalLoaded with the
// absolute file path when a valid cal is parsed.
class CalSlotComponent : public juce::Component,
                         public juce::FileDragAndDropTarget {
public:
    explicit CalSlotComponent (juce::String title);

    // Programmatically load a path (used to restore from Settings). Returns false and
    // shows an error line on parse failure (keeps any previously valid curve).
    bool loadFromFile (const juce::File&);

    std::optional<eb::CalFile> calFile() const { return cal; }

    std::function<void (const juce::File&)> onCalLoaded;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit  (const juce::StringArray&) override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyParsed (const eb::CalFile&, const juce::File&);

    juce::String slotTitle;
    juce::Label  fileLabel;     // filename
    juce::Label  detailLabel;   // "Serial 000-0000 - HPN"
    juce::Label  errorLabel;    // parse error line (red), hidden unless set
    CurveThumbnail thumbnail;
    std::optional<eb::CalFile> cal;
    bool heqWarning = false;    // HEQ loaded as mic-cal
    bool dragHover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalSlotComponent)
};

} // namespace eb
