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
#include "diag/DiagnosticLog.h"     // eb::DiagnosticLog (Task 1 of the match-detection + diagnostic-logging build)
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
    void updateDiracMicGainHint();   // refresh the "add ~+N dB on Dirac's Mic gain" caption from the live headroom
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
    juce::Label levelsHint;              // inline caption: aim Dirac's Master output at the meter target band
    juce::Label diracMicGainHint;        // "add ~+N dB on Dirac's Mic gain" - N = engine.headroomAttenuationDb()
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
    void loadStoredReference();      // startup: reload a previously-learned reference.f32 so it survives a restart
    void pollReferenceGrade();       // timer (~2 s): snapshot the ring, referenceMatches it (detector), grade off-thread, publish
    juce::String referenceStatePath_;                 // stored reference file (empty until learned this session)
    // Reference-Based Measurement Monitor (Plan 5): the learned reference held IN MEMORY for grading. The
    // GUI worker (pollReferenceGrade) needs both halves — this reference and the engine's response buffer —
    // to run gradeMeasurement OFFLINE. Populated on a successful learn; the rate drives the Farina offsets.
    std::vector<float> loadedReference_;              // the learned ESS reference samples (empty until learned)
    double             loadedReferenceRate_ = 48000.0;
    std::atomic<bool>  gradeInFlight_ { false };      // guards against re-posting while a grade job runs
    // Task 4 match poll: the 30 Hz timer calls pollReferenceGrade, but the MATCH (a cross-correlation FFT) is
    // throttled to ~every kGradePollTicks ticks (~2 s) while Running + referenceLoaded. Grade on a STABLE
    // MATCH, not on silence: gradedThisSession_ is the one-grade-per-match DEBOUNCE, set when a match session
    // grades and CLEARED when the match drops (so a fresh sweep can grade again). lastPollMatched_ holds the
    // previous throttled poll's match result so two consecutive matched polls (~4 s of sustained match) confirm
    // the full sweep landed before grading. There is NO absolute level gate, and no silence/settled gate, here.
    int                gradePollTick_       = 0;      // 30 Hz tick counter for the ~2 s match-poll throttle
    static constexpr int kGradePollTicks    = 60;     // ~2 s at the 30 Hz GUI timer
    bool               lastPollMatched_     = false;  // previous throttled poll's match result (stable-match edge)
    bool               gradedThisSession_   = false;  // debounce latch: this match session already graded
    // Cancellable + restartable learn: the button doubles as Learn / Cancel. learnCancelRequested_ is the
    // message-thread -> firPool hand-off (set on a cancel click, polled inside captureLoopback). learning_ is
    // message-thread-only state guarding against a double-start during the brief cancel window.
    std::atomic<bool>  learnCancelRequested_ { false };
    bool               learning_ = false;
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
    // Diagnostic-log export affordances (Task 3). "Open log folder" reveals %TEMP%/EarsBridge/logs in the
    // OS file browser; "Export log..." zips the whole logs dir to a user-chosen path via ZipFile::Builder.
    juce::TextButton openLogButton    { "Open log folder" };
    juce::TextButton exportLogButton  { "Export log..." };
    std::unique_ptr<juce::FileChooser> logChooser;   // kept alive across the async launchAsync

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

    // ---- Diagnostic log (Task 3) ----
    // The GUI owns one rotating log under %TEMP%/EarsBridge/logs and writes the app lifecycle, the
    // audio-device formats, the Dirac config, and the reference-monitor transitions through it across a
    // measurement run, so a failed run leaves a readable trail. MESSAGE-THREAD ONLY (the timer + the event
    // handlers) — the audio callback NEVER writes the log (DiagnosticLog does blocking file I/O).
    std::unique_ptr<eb::DiagnosticLog> log_;
    // The serial that logLine() scrubs out of EVERY message as a backstop, so the EARS serial can never
    // leak even if a call site forgets. Set whenever a cal loads (the cal carries the serial), cleared on
    // remove. Call sites already avoid the value; this is defense in depth.
    juce::String loggedSerial_;
    // Every log line goes through here: it runs the message through DiagnosticLog::redactSerial against
    // loggedSerial_ first, then writes it. A no-op when log_ is null.
    void logLine (eb::DiagnosticLog::Level level, const juce::String& msg);
    // Snapshot the selected input/output device + endpoint format, and the Dirac config, into the log.
    // Called at launch and whenever the device selection / format changes. Pure reads of the existing
    // device-layer + Dirac reads (no new probing).
    void logDeviceSnapshot (const juce::String& reason);
    void logDiracSnapshot  (const juce::String& reason);
    // Transition tracking for timerCallback: the last-logged reference-monitor snapshot, so we log the
    // state/peak/coherence/verdict ON CHANGE (not every 30 Hz tick), plus a ~30 s heartbeat tick counter.
    // Start-gate enabled-state, last value written to the log (-1 = never logged). updateStartGate runs every
    // 30 Hz tick; we log a Debug "Start gate: enabled/disabled (...reason...)" line ONLY when this flips, so
    // the single most useful line — WHY Start is greyed out — appears once per change, not on every tick.
    int    lastStartGateLogged_ = -1;            // 0 = disabled, 1 = enabled; -1 = never logged
    int    lastRefMonStateLogged_ = -1;          // RefMonState int; -1 = never logged
    bool   lastRefLoadedLogged_   = false;
    juce::String lastListenTextLogged_;          // last Listening<->Sweep-in-progress status text logged (Task 4)
    int    lastCoherBucketLogged_     = -1000;   // bucketed match coherence (0.05 buckets); input peak is NOT a trigger (it floods)
    int    heartbeatTick_ = 0;                    // 30 Hz tick counter; a heartbeat candidate every ~900 (~30 s)
    static constexpr int kHeartbeatTicks = 900;   // ~30 s at the 30 Hz GUI timer
    juce::String lastHeartbeatContent_;           // last heartbeat line logged; identical beats are suppressed
    int    heartbeatsSuppressed_ = 0;             // consecutive suppressed identical beats (idle de-dup)
    static constexpr int kHeartbeatKeepalive = 20; // force a beat after this many suppressed (~10 min alive pulse)

    // Hosts hover tooltips (e.g. the full 32-bit-float explanation on the neutral info line). A single
    // window owned by the component is enough for the whole app; declared last so it is destroyed
    // first (before the labels it serves).
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace eb
