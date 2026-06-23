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
// "Replace" button. HEQ is miniDSP's recommended Dirac cal (calm note); RAW surfaces as an amber
// badge (mic-only, miniDSP-unsupported).
// Fires onCalLoaded with the absolute path when a valid cal is parsed.
class CalSlotComponent : public juce::Component,
                         public juce::FileDragAndDropTarget {
public:
    explicit CalSlotComponent (juce::String earName);

    bool loadFromFile (const juce::File&);
    std::optional<eb::CalFile> calFile() const { return cal; }
    bool hasCal() const { return cal.has_value(); }
    void setPlotRange (float topDb);   // lock the FR plot to a shared dB scale
    void applyTheme();                 // re-apply theme-dependent colours (live light/dark switch)
    void clearCal();                   // empty the slot (back to the drop zone) + notify
    // Loud, un-truncated red problem banner under the card title (e.g. a detected L/R swap). Empty
    // string clears it. Independent of the parse warning/error label so a swap can't be buried.
    void setProblem (const juce::String& message);

    std::function<void (const juce::File&)> onCalLoaded;
    std::function<void ()> onCalCleared;   // fired when the slot is emptied via Remove

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
    juce::Label  problemLabel;  // loud red swap banner under the title (setProblem); hidden unless set
    juce::TextButton replaceBtn { "Replace" };
    juce::TextButton removeBtn  { "Remove" };
    CurveThumbnail thumbnail;
    std::optional<eb::CalFile> cal;
    juce::String typeTag;       // "HPN" / "HEQ" / ...
    bool rawCaution = false;
    bool dragHover = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalSlotComponent)
};

} // namespace eb
