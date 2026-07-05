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
// it shows the parsed FR thumbnail, the filename and the serial (each on its own line), a
// type badge (HPN/HEQ), and a "Replace" button. Per-type HEQ/HPN guidance lives off-card now
// (the stage caption hosts it); RAW surfaces as an amber badge (mic-only, miniDSP-unsupported).
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

    // P2 (spec 5.2): the empty-state "Required" honesty line. With the OTHER ear loaded, Start
    // is genuinely blocked until this ear loads too - only then does the card claim "Required".
    void setSiblingLoaded (bool otherEarHasCal);
    // State-dependent card height (empty drop zone vs loaded card, plus problem/caution/error
    // rows). CalibrateStage sizes both grid cells to the taller card so the row stays uniform.
    int preferredHeight() const;
    // Fired when the card's height inputs change (load/clear/problem/caution/error) so the host
    // stage re-runs its grid. Distinct from onCalLoaded/onCalCleared (MainComponent's wiring).
    std::function<void()> onLayoutChanged;

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;

    std::function<void (const juce::File&)> onCalLoaded;
    std::function<void ()> onCalCleared;   // fired when the slot is emptied via Remove
    // Fired with a loaded file's parse warnings (side-conflict, skipped/non-monotonic rows) so the owner can
    // LOG them through its redacting logger - the card shows the first inline (#54: they were write-only before).
    std::function<void (const juce::StringArray&)> onParseWarnings;

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
    void refreshStateVisibility();       // empty-state children visible iff no cal is loaded

    juce::String earName;
    juce::Label  fileLabel;     // filename only, e.g. "L_HPN.txt" (serial is on serialLabel)
    juce::Label  serialLabel;   // "Serial 000-0000" (own line under the filename)
    juce::Label  errorLabel;    // parse error (red), hidden unless set
    juce::Label  problemLabel;  // loud red swap banner under the title (setProblem); hidden unless set
    juce::TextButton replaceBtn { "Replace" };
    juce::TextButton removeBtn  { "Remove" };
    CurveThumbnail thumbnail;
    juce::Label      dzMain;             // "Drop the left ear file here"
    juce::TextButton browseBtn;          // visible Browse CTA (H1)
    juce::Label      dzReq;              // "Required - load both ears to measure"
    juce::Rectangle<int> dzIconArea;     // where paint() draws the drop glyph (set by resized)
    bool siblingLoaded = false;
    bool mouseHover = false;
    std::optional<eb::CalFile> cal;
    juce::String typeTag;       // "HPN" / "HEQ" / ...
    bool rawCaution = false;
    bool dragHover = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalSlotComponent)
};

} // namespace eb
