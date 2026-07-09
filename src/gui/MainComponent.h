#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/AudioEngine.h"      // eb::AudioEngine (Plan 2 SPINE)
#include "audio/ReferenceGradePoller.h"  // eb::ReferenceGradePoller (the headless grade-poll DECISION)
#include "cal/FirDesigner.h"        // eb::FirDesigner (Plan 1)
#include "state/Settings.h"
#include "gui/Theme.h"
#include "gui/DevicePicker.h"
#include "gui/CalSlotComponent.h"
#include "gui/FormatCluster.h"
#include "gui/LevelMeter.h"
#include "gui/RateMenu.h"
#include "gui/GradeMetricDotsView.h"   // eb::GradeMetricDotsView (3-color per-metric quality dots)
#include "gui/GradeBandSmoother.h"     // eb::GradeBandSmoother (per-ear anti-flicker band smoothing)
#include "net/UpdateChecker.h"
#include "gui/WizardState.h"        // eb::WizardStep / WizardInputs / computeWizardState (Task 1)
#include "gui/WizardSpine.h"        // eb::WizardSpine (Task 2, the persistent step rail)
#include "gui/stages/StageHost.h"  // eb::StageHost (Task 3, the single-child stage switcher)
#include "gui/stages/ConnectStage.h"
#include "gui/stages/CalibrateStage.h"
#include "gui/stages/LevelStage.h"
#include "gui/stages/MeasureStage.h"
#include "diag/DiagnosticLog.h"     // eb::DiagnosticLog (Task 1 of the match-detection + diagnostic-logging build)
#include "audio/DeviceConfigCheck.h"  // eb::checkChainConfig (48k-everywhere chain-config check)
#include <atomic>
#include <memory>
#include <optional>
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
    // appDataDir/logDir (#24): a headless test must also redirect the REFERENCE store (%APPDATA%/EarsBridge)
    // and the diagnostic-log dir, or the scored layout depends on the machine's real reference files and the
    // gate writes into the real log folder (non-hermetic). Empty = the real locations (the app itself).
    struct TestConfig { juce::File settingsDir; bool disableNetwork = true;
                        juce::File appDataDir;  juce::File logDir; };
    MainComponent();
    explicit MainComponent (const TestConfig& cfg);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void timerCallback() override;   // polls levels()/health()

    // Design-QA gate seam: force light/dark through the live theme-apply path (re-register palette + per-label
    // colours + relayout) so the headless gate can score contrast in both modes deterministically.
    void forceThemeForTest (bool dark);

    // #68 render-ratification seam: drive the title-bar header (both status lines + the per-ear quality
    // dots) with GIVEN text so the headless gate can score the CAPTURED two-line state at the minimum
    // window - those strings only appear mid-run, which a construct-and-probe test never reaches. Sets
    // label text + representative dot metrics and relays out; display only, no engine/grading state.
    void driveHeaderForTest (const juce::String& line1, const juce::String& line2, bool showDots);

    // Wizard step-forcing seam (beside forceThemeForTest): pin a step deterministically + relayout, so
    // the headless gate can iterate the WizardStep axis (the driven Measure labels now live in MeasureStage).
    void forceWizardStepForTest (WizardStep step);

    // Keyboard step navigation: Ctrl/Cmd + 1..4 jumps to a step when it is navigable (pin, then refresh).
    bool keyPressed (const juce::KeyPress& key) override;

    // Pure view decision (Task 4): which stage to SHOW. Normally ws.active. Exception — the TRANSIENT-only
    // pin illegality (recorded Task-3 carryover): when the pinned step is Blocked in `ws` ONLY because a FIR
    // rebuild is in flight, HOLD it (masked == the state recomputed with calBuilding off). If the pin is
    // still Blocked with the rebuild masked out, the illegality is real -> ws.active (first-unmet) wins.
    // Static + pure so the hold rule is unit-testable without a peer (test_wizardnav.cpp).
    static WizardStep resolveShownStage (const WizardState& ws, std::optional<WizardStep> pinned,
                                         const WizardState& masked);

    // ---- Task 4 navigation/a11y test seams (documented; used by tests/test_wizardnav.cpp) ----
    // These expose the live navigation path + the wired surfaces so the headless suite can assert the
    // tick-wiring outcomes without a peer. Unlike forceWizardStepForTest (which OVERRIDES pin legality
    // for the gate), pinStepForTest goes through the SAME live path a spine/Ctrl-N click uses, so an
    // illegal pin resolves to first-unmet exactly as it would in the app.
    WizardStep     shownStageForTest() const { return stageHost_.shown(); }
    void           pinStepForTest (WizardStep s) { pinnedStep_ = s; refreshWizardView(); }
    juce::Component* firstFocusTargetForTest() { return firstFocusTarget (stageHost_.shown()); }
    CalibrateStage&  calibrateStageForTest() { return calibrateStage_; }
    ConnectStage&    connectStageForTest()  { return connectStage_; }
    juce::ToggleButton& overrideToggleForTest() { return overrideToggle; }
    juce::TextButton& connectContinueForTest() { return connectStage_.continueButton(); }
    juce::TextButton& startButtonForTest() { return startStop; }
    FormatCluster&   fmtClusterForTest() { return fmtCluster_; }
    DevicePicker&    inputPickerForTest()  { return inputPicker; }
    DevicePicker&    outputPickerForTest() { return outputPicker; }
    // T10: force the Connect warning surfaces with PRODUCTION copy (ConnectHints.h) so the
    // no-scroll/displacement gates + harness measure real strings; (false,false) restores
    // model truth (updateDiracCableHint re-derives; hermetic rateWarn is empty).
    void driveConnectWarningsForTest (bool stdCableHintWithFix, bool rateResampleWarn);
    juce::Label& outputHintForTest() { return outputHint; }
    // T10: the Connect preflight warning label (warn-toned), for the no-scroll/displacement
    // BOUNDARY case that forces the post-Start preflight stack in the safety-net state.
    juce::Label& preflightLabelForTest() { return preflightLabel; }
    // P2.9 T6: the Advanced-FIR OUTPUT TRIM control, now a parameter row (label left, compact slider).
    juce::Slider& trimSliderForTest() { return trimSlider; }
    juce::Label&  trimLabelForTest()  { return trimLabel; }
    CalSlotComponent& leftCalForTest()  { return leftCal; }
    CalSlotComponent& rightCalForTest() { return rightCal; }
    // P2.9 T9: the combine-mode combo, so a test can prove the "(recommended)"/"+6 dB" badges ride
    // PopupMenu::Item::shortcutKeyDescription (a dim right-aligned suffix) rather than triple-space item text.
    juce::ComboBox& combineBoxForTest() { return combineBox; }
    // M-2 seams: drive the Level green-band latch and route an input through the SAME apply path both the
    // user pick (onInputChosen) and the hot-plug lambda use, so a test can prove a changed input key clears
    // the latch (the spine must not keep reading "In green band" on old-gain evidence after a replug).
    void setLevelLatchedForTest (bool v) { levelLatched_ = v; }
    bool levelLatchedForTest() const { return levelLatched_; }
    void applyResolvedInputForTest (const DeviceId& d) { applyResolvedInput (d); }
    // P3 Task 4 (Level rebuild) seams:
    LevelStage& levelStageForTest() { return levelStage_; }
    juce::Label& inputClipHintForTest() { return inputClipHint; }
    // T10-style production-copy forcing for the Level fit gate (mirrors driveConnectWarningsForTest).
    void driveLevelClipForTest (bool on);
    // P3: force/clear the device-error state (statusErrorMsg_) so gates/scenes can drive Connect-Error
    // + the regression banner headlessly. Empty = clear. Display/state only - no engine call.
    void driveDeviceErrorForTest (const juce::String& msg);
    // §5.2 mask seam: force a stale unity acceptance so a test can prove the `&& noCalsLoaded` mask
    // (snapshotWizardInputs) keeps the flag INERT once a cal loads - no public route leaves it set.
    void setUnityAcceptedForTest (bool v) { unityAcceptedSession_ = v; }
    // Reads the LIVE-snapshot `unityAccepted` (post-mask) so a test can pin the mask expression itself
    // (`unityAcceptedSession_ && gate.noCalsLoaded`) directly - the cold-verifier MAJOR was that this
    // AND-term is uncovered because every public route resets the flag before a cal loads (it goes RED
    // if the `&& gate.noCalsLoaded` mask is deleted). Deterministic: no dependency on the async FIR build.
    bool snapshotUnityAcceptedForTest() const { return snapshotWizardInputs().unityAccepted; }

    // P3 verdict-generation seams: publish a grade through the SAME engine-publish + stamp path the
    // live grade continuation uses, and bump the config generation, so a headless test can drive
    // Measure done-ness + staleness end-to-end (real grading needs hardware). Stamp-only, never gates.
    // sweepSnrDb mirrors the live publishCompletedSweepSnrDb so band-driven copy is drivable (<=0 skips).
    void publishGradeForTest (int ear, int refMonState, float sweepSnrDb = 30.0f);
    void bumpConfigGenForTest() { calGenCounter_.fetch_add (1, std::memory_order_relaxed); }
    int  verdictGenForTest (int ear) const { return ear == 1 ? verdictGenR_ : verdictGenL_; }
    WizardInputs snapshotWizardInputsForTest() const { return snapshotWizardInputs(); }
    // P3 Task 1 review seams. bitBoxForTest/chooseOutputForTest drive the REAL output bit-depth /
    // output-device change handlers (onBitDepthChosen via the combo's own onChange; onOutputChosen is
    // the exact member the picker callback invokes) so the user-ratified "an output format/device
    // change stales the verdicts" rule is pinned on the production handlers, not a replica.
    juce::ComboBox& bitBoxForTest() { return bitBox; }
    void chooseOutputForTest (const DeviceId& d) { onOutputChosen (d); }
    // Fix 4a (guard-before-stamp): the live publish continuation is unreachable headless (it needs a
    // Running engine, a learned reference and the two-stage worker chain), so its guard+publish+stamp
    // HEAD is the extracted production member publishGradeIfRunCurrent(); these drive THAT member with
    // an explicit run generation. Returns whether the publish+stamp happened.
    bool publishGradeGuardedForTest (int ear, int refMonState, uint32_t runGen)
        { return publishGradeIfRunCurrent (ear, refMonState, 50.0f, 0.5f, false, false, runGen); }
    uint32_t gradeRunGenForTest() const { return gradeRunGen_.load (std::memory_order_relaxed); }
    void bumpGradeRunGenForTest() { gradeRunGen_.fetch_add (1, std::memory_order_relaxed); }

private:
    // The reference/schedule store dir: the TestConfig override (#24, hermetic tests), else %APPDATA%/EarsBridge.
    juce::File appDataDir() const {
        return appDataOverride_.getFullPathName().isNotEmpty()
             ? appDataOverride_
             : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("EarsBridge");
    }
    juce::File appDataOverride_;   // empty = real location
    void refreshDeviceLists();
    void autoSelectDefaults();       // first run / empty slot: pick a recognised EARS + a standard VB-CABLE
    // Apply an input to the engine and, when the applied device KEY changed since the last apply, invalidate
    // the Level green-band latch AND the verdict freshness (§3.2: a different device/gain is different
    // evidence — level and measurement verdicts alike). BOTH the user pick (onInputChosen) and the hot-plug
    // re-resolution (engine.onDevicesChanged, incl. the EARS gain-DIP rename fallback) route through here,
    // so the spine can never keep claiming "In green band" — or presenting old-jig verdicts fresh — on
    // old-device evidence. Message-thread only (lastAppliedInputKey_ has no cross-thread reader). Returns
    // nothing; the caller owns persistence/menu-rebuild (those differ between the two paths).
    void applyResolvedInput (const DeviceId&);
    juce::String lastAppliedInputKey_;   // the key() of the input last pushed to the engine (latch memo)
    void onInputChosen  (const DeviceId&);
    void onOutputChosen (const DeviceId&);
    void updateDiracCableHint();     // standard-VB-CABLE-vs-Dirac warning + one-click shared-mode fix
    void rebuildRateMenu();
    void rebuildBitDepthMenu();
    void onRateChosen();
    void onBitDepthChosen();
    void onCombineChosen();
    void rebuildFirsAsync();         // design both FIRs at the active rate off-thread
    // Advance the config generation WITHOUT redesigning the FIRs (measurement-context change only:
    // input identity / output device / output bit depth). See the definition for the in-flight-build
    // fallback that keeps calBuilding from wedging.
    void bumpConfigGeneration();
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
    void syncTransport();            // §3.3 one action, per-stage faces (Level now; Measure joins in Task 5)
    void updateDiracMicGainHint();   // refresh the "add ~+N dB on Dirac's Mic gain" caption from the live headroom
    void updateCalProblems();        // surface a rejected swap/serial/type loudly ON the offending cal card
    // Pure shared computation of the per-card problem strings (per-slot side check + pair diagnostic).
    // updateCalProblems() sets the card text from it; anyCalProblem() collapses it to a bool for the
    // wizard snapshot so BOTH branches drive the Calibrate step's Error (#M3).
    void computeCalProblems (juce::String& leftProblem, juce::String& rightProblem) const;
    bool anyCalProblem() const;
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

    // Recompute the wizard view from live truth: snapshot the inputs, run the pure state machine, feed
    // the spine (always) + show the resolved stage. P1 calls this from the same places updateStartGate is
    // called (Task 4 folds it into the tick proper). pinnedStep_ carries the user's navigation pin.
    void refreshWizardView();
    // Render a resolved WizardState into the view (spine + stage host). Split out of refreshWizardView so
    // the test seam forceWizardStepForTest can feed a state with `.active` overridden (see the seam's note).
    void renderWizardView (const WizardState& ws);
    // Fill a WizardInputs from the existing members (GateSnapshot fields + engine state). Pure reads.
    WizardInputs snapshotWizardInputs() const;
    std::optional<WizardStep> pinnedStep_;   // user navigation pin (empty = launch/first-unmet resolution)
    // Which stage the view is CURRENTLY showing (mirror of stageHost_.shown()), so the tick wiring can
    // detect a real switch (post the a11y announcement + place focus) vs a same-stage re-render.
    WizardStep shownStage_ = WizardStep::Connect;
    // Level SOFT-gate latch (§3.2): set once L and R meter dB both reached the green band [-18,-12] this
    // session while Running; survives Stop (the knob didn't move); reset on an input-device change.
    bool levelLatched_ = false;
    // §3.2 staleness stamps: the config generation each ear's LAST verdict was measured under (-1 =
    // never). Config is frozen while Running (updateControlsEnabled), so the generation at publish
    // time IS the generation the sweep ran under. Never reset — staleness is COMPUTED
    // (verdictGen < configGen); evidence is downgraded, not deleted.
    int  verdictGenL_ = -1, verdictGenR_ = -1;
    void stampVerdictGeneration (int ear);
    // The run-generation-guarded HEAD of the live publish continuation (guard FIRST, then publish, then
    // stamp) — extracted so the ORDER is test-pinned. Returns false (nothing published, nothing stamped)
    // when runGen is stale. Message-thread only.
    bool publishGradeIfRunCurrent (int ear, int state, float irSnrDb, float thdPercent,
                                   bool mismatch, bool lowQuality, uint32_t runGen);
    bool unityAcceptedSession_ = false;   // §5.2 explicit unity choice; session-scoped, never persisted
    // The first enabled, focusable leaf inside a stage's subtree (top-down), for explicit focus placement
    // on a stage switch. Returns the stage itself when it has no focusable child (never null once built).
    juce::Component* firstFocusTarget (WizardStep step);
    // Post an AccessibilityHandler announcement ("<Step name>, step N of 4") on a real stage switch.
    void postStageAnnouncement (WizardStep step);

    Theme theme;
    AudioEngine engine;
    Settings settings;

    // P2.9 chrome ruling: the NATIVE title bar owns the app name (taskbar/snap identity); the in-app
    // bar keeps only the brand GLYPH + this live format cluster (W2 .fmt). The word-mark label died -
    // "EARS Bridge" twice in vertical adjacency was the double-branding tell.
    FormatCluster fmtCluster_;
    // Subtle "vX.Y.Z" footnote in the bottom-right corner (current installed version).
    juce::Label versionLabel;
    // Right-pane section eyebrows. (calEyebrow died in P2 Task 5 - the CalibrateStage header replaced it.)
    juce::Label levelsEyebrow;
    juce::Label levelsHint;              // inline caption: aim Dirac's Master output at the meter target band
    juce::Label diracMicGainHint;        // "add ~+N dB on Dirac's Mic gain" - N = engine.headroomAttenuationDb()

    // Input row.
    DevicePicker inputPicker { "INPUT" };
    juce::Label  inputGainHint;   // "keep the EARS gain at its factory 18 dB" recommendation
    // Cal slots.
    CalSlotComponent leftCal  { "Left ear" };
    CalSlotComponent rightCal { "Right ear" };
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
    // #34: the Dirac output endpoint (stable UID, else display name) the reference was learned FROM -
    // from the sidecar on reload, or resolved at learn time. "" = no binding (legacy sidecar / default
    // render target). Start's preflight warns when the current Dirac output differs.
    juce::String       loadedReferenceEndpoint_;
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
    // SP3 cross-ear polarity (D4): the 2048-sample peak segment of each ear's LAST graded IR this session,
    // plus its freshness. crossEarPolarity runs only when BOTH ears carry a fresh, matched segment from the
    // same run. Message-thread-only (written in the grade publish callAsync, which is serialized per ear by
    // gradeInFlight_). The run generation stamps the segments so a Stop/Start invalidates a stale pair.
    static constexpr int kShapePolaritySeg = 2048;
    std::vector<float>    shapePeakSegPerEar_[2];             // the 2048-sample peak segment (empty until fresh)
    uint32_t              shapePeakSegGenPerEar_[2] { 0u, 0u }; // run gen the segment was captured in
    bool                  shapePeakSegFresh_[2] { false, false };
    // The per-ear shape-detector result the WORKER produces (everything but cross-ear polarity, which needs
    // both ears). Scalars are the worst-offender magnitudes; peakSeg is this ear's 2048-sample IR peak segment
    // for the message-thread cross-ear polarity pass.
    struct ShapeResult {
        unsigned flags = 0u;
        float driftMaxDb = 0.0f, hfShelfDb = 0.0f, combDepthDb = 0.0f, combDelayMs = 0.0f;
        float effLoHz = 0.0f, effHiHz = 0.0f, lobeWidth = 0.0f, stepDb = 0.0f, resonanceHz = 0.0f;
        int   humBaseHz = 0;
        std::vector<float> peakSeg;    // 2048-sample IR peak segment (for D4 cross-ear), empty when not extracted
        eb::BandCurve newBaseline;     // the curve to LEARN as this ear's baseline (valid only when it should set)
        bool setBaseline = false;      // true == this is the FIRST GradedClean -> the message thread learns newBaseline
    };
    // WORKER-THREAD (heavy FFT detectors, off the message thread): run D1/D2/D3/D5a/D5b/D6/D7 for one graded
    // ear against a COPY of that ear's current baseline (nullptr when none learned yet). PURE re the engine —
    // it reads no engine state; the message thread owns the single-writer baseline set + the publish. Called
    // only on a MATCHED grade (a graded state); non-graded outcomes never reach it (honesty gate).
    static ShapeResult runShapeDetectors (int ear, bool gradedClean, const eb::BandCurve* baseline,
                                          const std::vector<float>& ir, double bandLoHz, double bandHiHz,
                                          const std::vector<float>& window, int windowStart, int gradedLen,
                                          const std::vector<float>& reference, double rate);
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
    // #50: the RUNNING wording/precedence lives in the PURE eb::runningStatus (gui/StatusLadder.h,
    // headlessly tested); updateStatusLine only builds its snapshot and commits the result.
    juce::Label statusLine;
    juce::Label statusLineR;
    // 3-color per-metric quality dots (SNR/IR/THD) under each ear's status line, fed by a per-ear band smoother.
    eb::GradeMetricDotsView gradeDotsL_, gradeDotsR_;
    eb::GradeBandSmoother    smootherL_, smootherR_;
    // " - <advisory>" when the chain-config advisory should decorate a calm status line; empty otherwise.
    juce::String chainAdvisoryTail() const;
    // Non-modal "Update available" link shown in the title bar when a newer release exists.
    juce::HyperlinkButton updateLink;
    UpdateChecker         updateChecker;
    // FIR / options controls (the Advanced disclosure died in the wizard cutover; its CHILDREN live on,
    // re-homed into stages — complexPhase/firLen/trim to Calibrate, override to Connect, hwDirac to Measure).
    juce::ToggleButton complexPhaseToggle { "Complex (with-phase) FIR" };
    juce::ToggleButton autoUpdateToggle { "Automatically check for updates" };
    // #3: opt-in escape hatch from the Dirac-path Start gates (combine-mode + virtual output).
    // Nested inside Advanced so it takes a deliberate expand-and-tick; never bypasses cal/devices.
    juce::ToggleButton overrideToggle { "Allow non-Dirac use (skip combine-mode / output checks)" };
    // Hardware-Dirac detect-and-degrade: the toggle commits "grading off"; the trackers feed the auto-detect.
    juce::ToggleButton hwDiracToggle  { "Dirac runs on a hardware processor (DDRC-24 / SHD / Flex)" };
    float maxOutputRenderPeak_ = 0.0f;   // max Dirac-output render peak seen this run (auto-detect)
    // #16: run-scoped UNCONDITIONAL max of the live input peaks (fed every GUI tick from engine.levels()).
    // The old micHeard source (maxSweepPeakL/R) only accumulates inside session.sweepActive(), whose +12 dB
    // rise-arm is proven dead on gradual Dirac log-sweeps - so the hardware-Dirac auto-detect could never
    // fire on a real measurement. This peak sees the sweep regardless of the session arm.
    float hwMicRunPeak_        = 0.0f;
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
    // #53: flush settings and surface a silent save failure ONCE per session (read-only profile / AV lock -
    // the user's selections would otherwise vanish on restart with zero indication).
    void flushSettings();
    bool settingsSaveWarned_ = false;
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

    // --- Wizard view layer (P1 Task 3 cutover) ---
    // The persistent step spine + the single-child stage host, plus the four stages that re-home every
    // former rail/right-pane control. Declared BEFORE tooltips (§4: destroyed after it) and AFTER the
    // leaf controls above, so on teardown the stages detach their adopted children safely. Construct-once,
    // reparent-once: the stages adopt() the existing members in the ctor; navigation is setVisible only.
    WizardSpine    spine_;
    StageHost      stageHost_;
    ConnectStage   connectStage_;
    CalibrateStage calibrateStage_;
    LevelStage     levelStage_;
    MeasureStage   measureStage_;

    // Hosts hover tooltips (e.g. the full 32-bit-float explanation on the neutral info line). A single
    // window owned by the component is enough for the whole app; declared last so it is destroyed
    // first (before the labels it serves).
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace eb
