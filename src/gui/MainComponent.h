#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/AudioEngine.h"      // eb::AudioEngine (Plan 2 SPINE)
#include "cal/FirDesigner.h"        // eb::FirDesigner (Plan 1)
#include "state/Settings.h"
#include "gui/Theme.h"
#include "gui/DevicePicker.h"
#include "gui/CalSlotComponent.h"
#include "gui/LevelMeter.h"
#include "gui/RateMenu.h"
#include "net/UpdateChecker.h"
#include <atomic>
#include <memory>
#include <vector>

namespace eb {

// Top-level window content (design-spec §8). Owns the AudioEngine and Settings,
// builds the full layout, polls engine telemetry on a timer, and reconfigures the
// engine on user actions. FIRs are (re)designed on a worker thread at the active rate.
class MainComponent : public juce::Component,
                      public juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void timerCallback() override;   // polls levels()/health()

private:
    void refreshDeviceLists();
    void autoSelectDefaults();       // first run / empty slot: pick a recognised EARS + a standard VB-CABLE
    void onInputChosen  (const DeviceId&);
    void onOutputChosen (const DeviceId&);
    void updateDiracCableHint();     // standard-VB-CABLE-vs-Dirac warning + one-click shared-mode fix
    void rebuildRateMenu();
    void rebuildBitDepthMenu();
    void onRateChosen();
    void onBitDepthChosen();
    void onCombineChosen();
    void rebuildFirsAsync();         // design both FIRs at the active rate off-thread
    void onLeftCalLoaded  (const juce::File&);
    void onRightCalLoaded (const juce::File&);
    void onStartStop();
    void updateStatusLine();
    void updateActiveEarIndicator (bool silent);   // AutoPerEar "capturing Left/Right" caption + meter accent
    void updateStartGate();          // enable Start only when a valid calibration generation is applied
    void updateCalProblems();        // surface a rejected swap/serial/type loudly ON the offending cal card
    void updateControlsEnabled();    // freeze config (cals/rate/mode/FIR) while capturing; re-enable when stopped
    // Returns true when the selected input is a detected EARS / EARS Pro (the Dirac path).
    // Shared by isRealEarsWithCable and the physical-output gate (P1-09).
    bool isRealEarsInput() const noexcept;
    // Returns true when a detected EARS input and a virtual-sink output are both
    // selected. Used by updateStartGate to enforce the combine-mode gate (D7 / R17).
    bool isRealEarsWithCable() const noexcept;
    void syncPlotScales();           // lock both ear plots to one shared dB scale
    void applyTextColours();         // (re)apply theme-dependent label colours (live light/dark)
    void applyTitleBarTheme();       // match the OS title bar to the active mode (Windows)
    double activeRate() const;

    // Lays out every left-rail child inside railContent's local coordinates (a single
    // top-down removeFromTop pass over `width` reduced by the 16px gutter), and returns the
    // TOTAL content height consumed. resized() calls this to size railContent so the Viewport
    // can scroll the whole stack — including the tall expanded Advanced section. Pure layout:
    // it never calls resized(), so it can't trigger a resized() recursion through the Viewport.
    int layoutRail (int width);

    Theme theme;
    AudioEngine engine;
    Settings settings;

    // Left configuration rail, wrapped in a Viewport so the (growing) Advanced section is always
    // reachable by scrolling at any window size — including the default 900x700. railContent holds
    // every rail child and is sized to the FULL content height by layoutRail(); the Viewport scrolls
    // it. railContent paints the graphite rail backdrop + the right divider so the rail looks the
    // same as the old fixed rect (the Viewport itself is transparent and scrolls with the content).
    struct RailContent : juce::Component {
        void paint (juce::Graphics&) override;
    };
    juce::Viewport railViewport;
    RailContent    railContent;

    // Title bar brand.
    juce::Label brandLabel;
    // Subtle "vX.Y.Z" footnote in the bottom-right corner (current installed version).
    juce::Label versionLabel;
    // Right-pane section eyebrows.
    juce::Label calEyebrow;
    juce::Label levelsEyebrow;
    juce::Label levelsHint;              // inline caption: aim the amp at the meter target band
    juce::Rectangle<int> levelsBounds;   // Levels card backdrop (drawn in paint)

    // Input row.
    DevicePicker inputPicker { "INPUT" };
    juce::Label  inputGainHint;   // "keep the EARS gain at its factory 18 dB" recommendation
    // Cal slots.
    CalSlotComponent leftCal  { "LEFT EAR CAL" };
    CalSlotComponent rightCal { "RIGHT EAR CAL" };
    // Combine.
    juce::Label    combineLabel;
    juce::ComboBox combineBox;
    juce::Label    combineHint;
    // Output row.
    DevicePicker outputPicker { "OUTPUT (VIRTUAL CABLE)" };
    juce::Label  outputHint;     // "set this device's capture side as Dirac's Recording device"
    juce::Label  preflightLabel; // warnings (yellow)
    juce::Label  preflightInfo;  // calm, neutral fact line (e.g. the 32-bit-float / shared-mode note)
    // Shown only when the chosen output is a STANDARD VB-CABLE: Dirac records it in exclusive mode,
    // which that cable can't do (error 600007). Warns to use Hi-Fi Cable + a one-click shared-mode fix.
    juce::Label      diracCableHint;
    juce::TextButton diracFixButton { "Set Dirac to shared mode" };
    // Rate + bit depth.
    juce::Label    rateLabel;    juce::ComboBox rateBox;    juce::Label rateWarn;
    juce::Label    bitLabel;     juce::ComboBox bitBox;
    // Meters.
    LevelMeter meterL { "L" }, meterR { "R" }, meterOut { "Out" };
    // Shown when the RAW EARS capture clips (pre-cal, so it survives the inverse-cal cut at the
    // coupler resonance where clipping is most likely). Held long enough to outlast a full sweep.
    juce::Label inputClipHint;
    int         inputClipHold_ = 0;
    static constexpr int kInputClipHoldTicks = 240;   // ~8 s at the 30 Hz GUI timer
    int          silentTicks_ = 0;                    // consecutive below-floor input ticks (debounce)
    static constexpr int kSilentHoldTicks = 60;       // ~2 s of sustained silence before "no input signal"
    int          lowLevelTicks_ = 0;                  // consecutive present-but-too-quiet ticks (debounce)
    static constexpr int kLowLevelHoldTicks = 45;     // ~1.5 s below a healthy capture level before warning
    int          lowSnrTicks_ = 0;                    // consecutive ticks with the LowSnr flag set (debounce)
    static constexpr int kLowSnrHoldTicks = 6;        // ~0.2 s: steady the per-sweep verdict without lag
    juce::String statusErrorMsg_;                     // specific Error-state message (survives re-renders)
    // Reference-Based Measurement Monitor (Plan 5): drives the GUI worker that grades a completed sweep
    // OFFLINE against the learned reference, then surfaces the verdict in the status ladder.
    void onLearnReference();         // Advanced affordance: capture + validate + store a loopback reference (on-device)
    void pollReferenceGrade();       // timer: drain engine.consumePendingGrade(), run gradeMeasurement off-thread, publish
    juce::String referenceStatePath_;                 // stored reference file (empty until learned this session)
    // Reference-Based Measurement Monitor (Plan 5): the learned reference held IN MEMORY for grading. The
    // GUI worker (pollReferenceGrade) needs both halves — this reference and the engine's response buffer —
    // to run gradeMeasurement OFFLINE. Populated on a successful learn; the rate drives the Farina offsets.
    std::vector<float> loadedReference_;              // the learned ESS reference samples (empty until learned)
    double             loadedReferenceRate_ = 48000.0;
    std::atomic<bool>  gradeInFlight_ { false };      // guards against re-posting while a grade job runs
    // Transport.
    juce::TextButton startStop { "Start" };
    juce::Label statusLine;
    // Non-modal "Update available" link shown in the title bar when a newer release exists.
    juce::HyperlinkButton updateLink;
    UpdateChecker         updateChecker;
    // Advanced disclosure.
    juce::ToggleButton advancedToggle { "Advanced" };
    juce::ToggleButton complexPhaseToggle { "Complex (with-phase) FIR" };
    juce::ToggleButton autoUpdateToggle { "Automatically check for updates" };
    // #3: opt-in escape hatch from the Dirac-path Start gates (combine-mode + virtual output).
    // Nested inside Advanced so it takes a deliberate expand-and-tick; never bypasses cal/devices.
    juce::ToggleButton overrideToggle { "Allow non-Dirac use (skip combine-mode / output checks)" };
    juce::Label    firLenLabel; juce::ComboBox firLenBox;
    juce::Label    trimLabel;   juce::Slider trimSlider;
    // L/R check: drive a tone into the LEFT earcup; the engine reports whether L/R are wired right.
    juce::TextButton verifyButton { "Check L/R wiring" };
    juce::Label      verifyResultLabel;
    int verifyTicks = 0;        // GUI-tick timeout counter while a verify is running (0 = idle)
    // Reference-Based Measurement Monitor (Plan 5): learn the loopback reference (Dirac in Windows Audio).
    juce::TextButton learnRefButton { "Learn reference (Windows Audio)" };
    juce::Label      learnRefResultLabel;

    // FIR-length combo item id for the "Auto (scales with rate)" choice. Distinct from every
    // real tap count (4096/8192/16384/32768) so it never collides with an explicit override.
    static constexpr int kFirLenAutoId = 1;

    std::vector<RateMenuItem> rateModel;       // index -> combo id (i+1)
    std::vector<int> bitModel;                 // index -> combo id (i+1)
    std::vector<CombineMenuItem> combineModel; // index -> combo id (i+1)

    std::unique_ptr<juce::ThreadPool> firPool;  // off-thread FIR design
    // Monotonic calibration-generation request id. rebuildFirsAsync bumps this and stamps the
    // posted build job; on the message thread a finished job discards itself when its id no longer
    // equals this counter (a newer request superseded it). The engine mirrors it via
    // setRequestedGeneration(); together they form the stale-guard for the Start gate (P0-02).
    std::atomic<int> calGenCounter_ { 0 };
    int themeTick = 0;                          // throttles the live light/dark poll

    // Hosts hover tooltips (e.g. the full 32-bit-float explanation on the neutral info line). A single
    // window owned by the component is enough for the whole app; declared last so it is destroyed
    // first (before the labels it serves).
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace eb
