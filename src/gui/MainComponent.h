#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/AudioEngine.h"      // eb::AudioEngine (Plan 2 SPINE)
#include "audio/ReferenceGradePoller.h"  // eb::ReferenceGradePoller (the headless grade-poll DECISION)
#include "cal/FirDesigner.h"        // eb::FirDesigner (Plan 1)
#include "state/Settings.h"
#include "gui/Theme.h"
#include "gui/DevicePicker.h"
#include "gui/CalSlotComponent.h"
#include "gui/LevelMeter.h"
#include "gui/RateMenu.h"
#include "gui/GradeMetricDotsView.h"   // eb::GradeMetricDotsView (3-color per-metric quality dots)
#include "gui/GradeBandSmoother.h"     // eb::GradeBandSmoother (per-ear anti-flicker band smoothing)
#include "net/UpdateChecker.h"
#include "diag/DiagnosticLog.h"     // eb::DiagnosticLog (Task 1 of the match-detection + diagnostic-logging build)
#include "audio/DeviceConfigCheck.h"  // eb::checkChainConfig (48k-everywhere chain-config check)
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
    // Headless/test construction: inject a temp-dir Settings (hermetic) and suppress the launch-time update
    // network call. The default ctor is the real app (per-user Settings, network on).
    struct TestConfig { juce::File settingsDir; bool disableNetwork = true; };
    MainComponent();
    explicit MainComponent (const TestConfig& cfg);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void timerCallback() override;   // polls levels()/health()

    // Design-QA gate seam: force light/dark through the live theme-apply path (re-register palette + per-label
    // colours + relayout) so the headless gate can score contrast in both modes deterministically.
    void forceThemeForTest (bool dark);

private:
    // Common ctor both public ctors delegate to: settingsDir empty -> per-user Settings file (real app), else a
    // test temp dir; disableNetwork suppresses the launch-time update check so a headless test never hits the net.
    MainComponent (juce::File settingsDir, bool disableNetwork);
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
    // Write a status label only when its text OR colour actually changed (JUCE dontSendNotification, compare
    // getText()/findColour first). updateStatusLine computes each line's FINAL (text,colour) ONCE per tick then
    // commits it through this — NOTHING is cleared-then-conditionally-reset — so the line is persistent and the
    // notes never flicker (HIG: reflect state persistently, never flash). Returns true iff it wrote.
    static bool setLabelIfChanged (juce::Label& label, const juce::String& text, juce::Colour colour);
    void updateActiveEarIndicator (bool silent);   // AutoPerEar "capturing Left/Right" caption + meter accent
    // The ONE Start-readiness predicate, snapshotted: updateStartGate() renders it into the button + log, and
    // onStartStop() re-checks it AT CLICK TIME (audit #3: rate/FIR/complex changes bump the cal generation
    // without a gate refresh, so the button's enabled-state can be stale - a click raced the async build and
    // started the run on a superseded FIR, then stranded the gate closed).
    struct GateSnapshot { bool haveDevs = false, haveCals = false, wrongMode = false,
                          physicalOutput = false, noCalsLoaded = false, ready = false; };
    GateSnapshot computeStartGate() const;
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
    // LIVE in-sweep input readout (the sweep PEAK in dBFS + a clip). The raw 30 Hz dB would jitter on the last
    // digit and read as distracting motion (HIG: avoid distracting motion / Reduce Motion), so we hold a TRUE
    // RUNNING MAX of the per-channel level since the sweep engaged (reset on the idle->active edge) — a stable
    // peak that climbs but never jitters down, which is what's needed to set the level and catch a clip. The
    // displayed text is rebuilt at a reduced cadence (~7 Hz) from the held value so it reads steady. Sweep-
    // active is debounced: peakMax must hold above the gate for ~0.3 s before the live line takes over the
    // per-ear verdicts, and a release hold keeps it engaged across the quiet L<->R inter-sweep gap.
    float        liveHeldLDb_ = -120.0f;              // running PEAK (dBFS) of the L input since this sweep engaged
    float        liveHeldRDb_ = -120.0f;              // running PEAK (dBFS) of the R input since this sweep engaged
    bool         liveWasActive_ = false;              // prior tick's sweep-active state -> reset the running max on the engage edge
    int          sweepActiveTicks_ = 0;               // consecutive ticks with peakMax above the live gate (attack debounce)
    int          sweepActiveReleaseTicks_ = 0;        // remaining held ticks after the level last cleared the gate (release HOLD)
    int          liveTextTick_ = 0;                   // counts down to the next live-text rebuild (~7 Hz cadence)
    // Last RENDERED live-readout text/colour, re-emitted on the in-between (non-render) ticks so the live line
    // PERSISTS instead of blinking between the ~7 Hz rebuilds (Bug B-1). Reset on Start/Stop.
    juce::String liveHeldPrimary_;
    juce::Colour liveHeldColour_;
    static constexpr float kLiveActiveGateDb   = -18.0f;  // peakMax above this = a sweep (room floor ~-30 sits below)
    static constexpr int   kSweepActiveHoldTicks = 9;     // ~0.3 s at 30 Hz before the live line engages (attack)
    static constexpr int   kSweepActiveReleaseTicks = 45; // ~1.5 s at 30 Hz the live line stays engaged after the level dips (release)
    static constexpr int   kLiveTextEveryTicks = 4;       // rebuild the live text ~every 4 ticks (~7 Hz)
    // The rendered live-readout for one tick: the (held, throttled) status text split across the two status
    // lines, the primary line's Theme colour, and whether the live readout OWNS the status this tick.
    struct LiveReadout {
        bool         owns = false;        // true iff sweep-active (the live line should take over)
        bool         render = false;      // true iff this tick is a text-rebuild tick (the ~7 Hz cadence)
        juce::String primary;             // statusLine text (the live readout; single-line, statusLineR stays empty)
        juce::Colour colour;              // statusLine colour (a semantic Theme role)
    };
    // Advance liveHeldL/RDb_ (running peak-hold) from the engine's live per-channel peaks and the sweep-active
    // debounce EVERY tick (so the held level + debounce stay continuous), and compute the rendered readout.
    // Pure of side effects on the labels — the caller writes them only when the live readout takes precedence,
    // so a co-occurring hard error keeps statusLineR blank. Throttles the text rebuild to ~7 Hz (render flag).
    LiveReadout  updateLiveInputReadout();
    // Reference-Based Measurement Monitor (Plan 5): drives the GUI worker that grades a completed sweep
    // OFFLINE against the learned reference, then surfaces the verdict in the status ladder.
    void onLearnReference();         // Advanced affordance: capture + validate + store a loopback reference (on-device)
    void loadStoredReference();      // startup: reload a previously-learned reference.f32 so it survives a restart
    void pollReferenceGrade();       // timer (~2 s): runs BOTH per-ear pollers (L vs ref_L, R vs ref_R), grades off-thread, publishes two verdicts
    // Grade ONE earcup (ear 0 = LEFT, 1 = RIGHT) against ITS OWN reference channel: snapshot that ear's ring,
    // rate-guard, decide() (match-gate), and on the stable-match edge dispatch the off-thread deconvolve+grade
    // that publishes that ear's verdict via publishReferenceGrade(ear,...). Returns true iff it posted an
    // off-thread grade this tick (so the caller can serialize the two ears under one in-flight guard). A silent
    // ring fails the match-gate inside decide() -> no grade, no publish, no LowSnr for that ear (the honesty gate).
    bool gradeOneEar (int ear, eb::ReferenceGradePoller& poller,
                      const std::vector<float>& reference, double referenceRate);
    juce::String referenceStatePath_;                 // stored reference file (empty until learned this session)
    // Reference-Based Measurement Monitor (Plan 5): the learned reference held IN MEMORY for grading. The
    // GUI worker (pollReferenceGrade) needs both halves — this reference and the engine's response buffer —
    // to run gradeMeasurement OFFLINE. Populated on a successful learn; the rate drives the Farina offsets.
    // (the old loadedReference_ LEFT alias - a full ~2 MB duplicate kept for one startup log line - was
    //  removed in the audit Phase B (#62); the log + grade path read loadedReferenceL_/R_ directly.)
    // Per-Ear Per-Channel Grading: the learned reference is PER CHANNEL — ref_L = Dirac's hard-panned LEFT
    // sweep (render ch0), ref_R = the RIGHT sweep (render ch1). They are stored separately (reference_L.f32 /
    // reference_R.f32) so each earcup is deconvolved against the channel that drove it. The grade path
    // (pollReferenceGrade -> gradeOneEar) and the startup duration log use THESE two refs exclusively.
    std::vector<float> loadedReferenceL_, loadedReferenceR_;  // per-channel learned reference samples (empty until learned)
    double             loadedReferenceRateL_ = 0.0, loadedReferenceRateR_ = 0.0;
    std::atomic<bool>  gradeInFlight_ { false };      // guards against re-posting while a grade job runs
    std::atomic<uint32_t> gradeRunGen_ { 0 };         // bumped on Stop+Start; a grade job stamps it at post and a
                                                      // stale (prior-run) continuation is dropped, so run A's verdict
                                                      // can never publish over run B's fresh state (review #4)
    // Task 4 match poll: the 30 Hz timer calls pollReferenceGrade, but the MATCH (a cross-correlation FFT) is
    // throttled to ~every kGradePollTicks ticks (~2 s) while Running + referenceLoaded. Grade on a STABLE
    // MATCH, not on silence: gradedThisSession_ is the one-grade-per-match DEBOUNCE, set when a match session
    // grades and CLEARED when the match drops (so a fresh sweep can grade again). lastPollMatched_ holds the
    // previous throttled poll's match result so two consecutive matched polls (~4 s of sustained match) confirm
    // the full sweep landed before grading. There is NO absolute level gate, and no silence/settled gate, here.
    int                gradePollTick_       = 0;      // 30 Hz tick counter for the ~2 s match-poll throttle
    int                gradeEarToggle_      = 0;      // alternates which ear pollReferenceGrade tries first (fairness)
    static constexpr int kGradePollTicks    = 60;     // ~2 s at the 30 Hz GUI timer
    // The match + stable-match-debounce DECISION lives in a headless unit (eb::ReferenceGradePoller) so the
    // grade logic can be self-tested without the GUI/hardware (the "grade never fires" regression lived here).
    // The GUI keeps the throttle (above), the off-thread firPool dispatch, and the publish; the poller owns
    // only the two-consecutive-matched-polls debounce + the grade — see decide()/gradeWindow().
    // Per-Ear Per-Channel Grading (Task 4): ONE poller PER EAR. Dirac hard-pans its sweeps, so the LEFT mic
    // ring is graded vs ref_L (gradePollerL_) and the RIGHT mic ring vs ref_R (gradePollerR_), each with its
    // OWN two-poll debounce. Both are reset() on Start/Stop. The two ears are fully independent: one can grade
    // GradedClean while the other (silent ring / no sweep) never matches and stays Learned.
    eb::ReferenceGradePoller gradePollerL_, gradePollerR_;
    // Cancellable + restartable learn: the button doubles as Learn / Cancel. learnCancelRequested_ is the
    // message-thread -> firPool hand-off (set on a cancel click, polled inside captureLoopback). learning_ is
    // message-thread-only state guarding against a double-start during the brief cancel window.
    std::atomic<bool>  learnCancelRequested_ { false };
    bool               learning_ = false;
    // Transport.
    juce::TextButton startStop { "Start" };
    // Status note in the title bar. Per-Ear Per-Channel Grading (Task 5): TWO stacked lines. statusLine is
    // the upper/LEFT line, statusLineR the lower/RIGHT line. For every GLOBAL/hard condition (device error,
    // the 48k config veto, Stopped/gate states, learning, no per-ear reference) statusLine carries the ONE
    // message and statusLineR is blanked — a hard error must never show a stale per-ear grade beneath it.
    // Only when Running with a per-ear reference loaded do BOTH lines render, one verdict per ear ("L:" /
    // "R:"). renderEarStatusLine() builds one ear's text+colour; renderPerEarStatusLines() drives both.
    juce::Label statusLine;
    juce::Label statusLineR;
    // 3-color per-metric quality dots (SNR/IR/THD) under each ear's status line, fed by a per-ear band smoother.
    eb::GradeMetricDotsView gradeDotsL_, gradeDotsR_;
    eb::GradeBandSmoother    smootherL_, smootherR_;
    // Per-Ear Per-Channel Grading (Task 5): set both per-ear lines from each ear's published state+metrics.
    // Called only from the Running + reference-loaded path in updateStatusLine (the hard/global ladder above
    // it has precedence and blanks statusLineR). Pure DISPLAY — reads the ear-indexed engine getters, never
    // touches grading logic, the match-gate, or the thresholds.
    void renderPerEarStatusLines();
    // Build ONE ear's status line (ear 0 = LEFT, 1 = RIGHT) into `label` from THAT ear's refMonState +
    // IR-SNR/THD/sweepSNR. INFO-not-warn while kIrThresholdsRatified is false (a clean ~13 dB ESS must not
    // false-warn); mismatch/stale -> warn "re-learn"; ungraded -> dim "waiting for the sweep"; per-ear low
    // SNR appends "(low SNR)". prefix is "L:" / "R:".
    // appendAdvisory: compose the chain-config advisory INTO the single commit when this line lands calm
    // (ok/dim) - the old post-commit append stripped + re-suffixed the label every tick (audit #49).
    void renderEarStatusLine (int ear, const char* prefix, juce::Label& label, bool appendAdvisory = false);
    // " - <advisory>" when the chain-config advisory should decorate a calm status line; empty otherwise.
    juce::String chainAdvisoryTail() const;
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
    // Hardware-Dirac detect-and-degrade: the toggle commits "grading off"; the trackers feed the auto-detect.
    juce::ToggleButton hwDiracToggle  { "Dirac runs on a hardware processor (DDRC-24 / SHD / Flex)" };
    float maxOutputRenderPeak_ = 0.0f;   // max Dirac-output render peak seen this run (auto-detect)
    bool  hwOutputReadable_    = false;  // whether OutputActivity read a real value this run (else can't confirm silence)
    int   hwDetectTick_        = 0;      // throttle for the (COM-heavy) output-activity poll (~2 s, like the grade poll)
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
    // Serials that logLine() scrubs out of EVERY message as a backstop, so the EARS serial can never leak
    // even if a call site forgets. #51: a StringArray, not one serial - a mismatched L/R pair carries TWO
    // distinct serials and the single-value backstop left the other one unscrubbed. Accumulate every distinct
    // loaded serial (an extra stale entry is harmless: it only scrubs a value that no longer appears).
    juce::StringArray loggedSerials_;
    // Every log line goes through here: it runs the message through DiagnosticLog::redactSerial against each
    // known serial first, then writes it. A no-op when log_ is null.
    void logLine (eb::DiagnosticLog::Level level, const juce::String& msg);
    // 48k-everywhere chain-config check (Reference-Based Measurement Monitor). On the GUI timer (throttled
    // to ~once/sec) read the three endpoints — input = the selected input, cable = the selected OUTPUT,
    // diracOutput = readDiracOutputDeviceName — run eb::checkChainConfig, store the verdict, and log it ON
    // CHANGE (with each endpoint's rate/channels/bits). updateStatusLine reads chainVerdict_ to WARN +
    // VETO the green "verified" line when the chain isn't all-48k. The live reads are Windows/on-device;
    // the verdict LOGIC is fully unit-tested headless (test_deviceconfigcheck.cpp).
    void pollChainConfig();
    eb::ConfigVerdict chainVerdict_;                  // last computed verdict (read by updateStatusLine)
    eb::ChainConfig   chainFormats_;                  // the three per-endpoint formats behind the verdict (read by the Signal-chain panel)
    int    chainPollTick_ = 0;                        // 30 Hz tick counter for the ~1 s chain-config throttle
    static constexpr int kChainPollTicks = 30;        // ~1 s at the 30 Hz GUI timer
    juce::String lastChainSummaryLogged_;             // last chain-config line logged (log on change only)
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
