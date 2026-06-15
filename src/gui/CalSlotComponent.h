#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "cal/CalFile.h"
#include "gui/CurveThumbnail.h"
#include <functional>
#include <optional>
#include <memory>

namespace eb {

// One per-ear calibration card ("Left ear" / "Right ear"). Empty, it shows a dashed
// drop zone (drag a .txt/.frd file or click to browse) and a "Required" badge; loaded,
// it shows the parsed FR thumbnail, the filename + serial, a type badge (HPN/HEQ), and a
// "Replace" button. HEQ files surface as an amber badge (double-target risk, spec §3.7).
// Fires onCalLoaded with the absolute path when a valid cal is parsed.
class CalSlotComponent : public juce::Component,
                         public juce::FileDragAndDropTarget {
public:
    explicit CalSlotComponent (juce::String earName);

    bool loadFromFile (const juce::File&);
    std::optional<eb::CalFile> calFile() const { return cal; }
    bool hasCal() const { return cal.has_value(); }

    std::function<void (const juce::File&)> onCalLoaded;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit  (const juce::StringArray&) override;
    void mouseUp (const juce::MouseEvent&) override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyParsed (const eb::CalFile&, const juce::File&);
    void browseForCal();

    juce::String earName;
    juce::Label  fileLabel;     // "L_HPN.txt - serial 000-0000"
    juce::Label  errorLabel;    // parse error (red), hidden unless set
    juce::TextButton replaceBtn { "Replace" };
    CurveThumbnail thumbnail;
    std::optional<eb::CalFile> cal;
    juce::String typeTag;       // "HPN" / "HEQ" / ...
    bool heqWarning = false;
    bool dragHover = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalSlotComponent)
};

} // namespace eb
