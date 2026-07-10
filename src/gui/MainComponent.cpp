#include "gui/MainComponent.h"
#include "gui/ClipStatus.h"
#include "gui/RawRailStatus.h"
#include "gui/ConnectHints.h" // eb::hints:: production Connect warning copy (T10 - shared with tests + harness)
#include "gui/StartGate.h"   // eb::startReady (Task 3 / #3 advanced override)
#include "gui/StartNotes.h"  // eb::buildStartNotes (Task 4 / #8 calm bit-depth note)
#include "gui/SystemA11y.h"  // eb::SystemA11y - Reduce Motion / Increase Contrast / Reduce Transparency (HIG)
#if JUCE_DEBUG || defined (EB_ENABLE_HIG_HARNESS)
 #include "gui/juce_design_probe.h" // EB_HIG_STATES dev-QA harness: probe the header in every status state (vendored apple-hig)
#endif
#include "platform/DiracCompat.h"
#include "cal/CalibrationPairValidator.h"   // eb::validateCalibrationPair (P0-07)
#include "audio/CalibrationGeneration.h"    // eb::CalibrationGeneration (generation lifecycle)
#include "audio/RefMonitor.h"               // eb::RefMonState / gradeMeasurement / refMonBlocksGreen (Plan 5)
#include "audio/ResponseShape.h"            // SP3: the seven shape detectors + BandCurve (INFO-only, grade worker)
#include "audio/HardwareDiracDetect.h"      // eb::sweepWasInternal / hardwareDiracSuggestion (hardware-Dirac)
#include "platform/OutputActivity.h"        // eb::outputRenderPeakForName (is Dirac's PC output rendering?)
#include "gui/SnrStatus.h"                  // eb::kMinSweepSnrDb (sweep-to-noise SNR threshold; match-window SNR fix)
#include "gui/GradeGuards.h"                // #32: the grade-path predicates, shared with the tests (no mirror drift)
#include "gui/StatusLadder.h"               // #50: the PURE Running status ladder (honesty cluster #1 #4 #21 #44)
#include "gui/LiveInputStatus.h"            // eb::liveInputStatus (live per-channel in-sweep readout)
#include "gui/DeviceNameDistill.h"          // eb::distillDeviceName (P2.9 T8: spine metas -> W2-style short device summaries)
#include "audio/LoopbackReference.h"        // eb::captureLoopback / validateReferenceCapture / readDiracDeviceType (Plan 5)
#include "audio/SweepScheduleStore.h"       // eb::extractSchedule / serialize+deserialize (AutoPerEar schedule, P0-06)
#include "audio/ReferenceMetaStore.h"       // eb::serializeReferenceMeta / checkReferenceMeta (audit #5/#20 integrity sidecar)
#include "platform/EndpointFormat.h"        // eb::endpointMixSampleRateForName (the WASAPI mix-format rate)
#include "platform/EndpointUid.h"           // #34: bind/verify the reference's learn-time Dirac output endpoint
#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX   // keep <windows.h> from defining min/max macros that break std::min/std::max
 #endif
 #include <windows.h>
 #include <dwmapi.h>
 #pragma comment(lib, "dwmapi.lib")
 #undef min
 #undef max
#endif

namespace eb {

static void styleEyebrow (juce::Label& l, const juce::String& t) {
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, Theme::textDim());
    // All-caps eyebrows get a little tracking so the caps don't read as cramped/heavy.
    l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
}

// The raw-input clip warning's PRODUCTION copy, file-local so the ctor (the one live populate site -
// the timer only toggles visibility) and driveLevelClipForTest can never drift (P3 Task 4 copy rule).
static const juce::String kInputClipHintText =
    "EARS mic input near full scale" + eb::kDash + "lower Dirac's Master output (try ~-12.5 dB). "
    "Don't touch the EARS gain switch. Dirac won't flag this: the clip is at the "
    "mic, before Dirac records.";

// Human-readable step name for the wizard view (a11y titles/announcements + the banner "Fix in <step>").
static juce::String stepName (WizardStep s) {
    switch (s) {
        case WizardStep::Connect:   return "Connect";
        case WizardStep::Calibrate: return "Calibrate";
        case WizardStep::Level:     return "Level";
        case WizardStep::Measure:   return "Measure";
    }
    return {};
}

// Short cal-type name for the Calibrate spine done-summary ("HEQ pair" + kDash + "<serial>").
static juce::String calTypeShortName (eb::CalType t) {
    switch (t) {
        case eb::CalType::Heq: return "HEQ";
        case eb::CalType::Idf: return "IDF";
        case eb::CalType::Hpn: return "HPN";
        case eb::CalType::Raw: return "Raw";
        default:               return "Cal";
    }
}

// Human-readable name for a RefMonState int (the engine publishes the enum's underlying int). Used only
// for the diagnostic log so a transition reads "GradedClean" rather than a bare number.
static const char* refMonStateName (int s) {
    switch ((eb::RefMonState) s) {
        case eb::RefMonState::NotLearned:     return "NotLearned";
        case eb::RefMonState::Learned:        return "Learned";
        case eb::RefMonState::ReferenceStale: return "ReferenceStale";
        case eb::RefMonState::GradedClean:    return "GradedClean";
        case eb::RefMonState::GradedMarginal: return "GradedMarginal";
        case eb::RefMonState::GradedSuspect:  return "GradedSuspect";
        case eb::RefMonState::NotGraded:      return "NotGraded";
        case eb::RefMonState::GradingOffHardware: return "GradingOffHardware";
    }
    return "?";
}

// One-char name for a QualityBand int (for the ratification GRADE COMPLETE log line). G/O/R = green/orange/red.
static char bandChar (int b) {
    switch ((eb::QualityBand) b) {
        case eb::QualityBand::Green:  return 'G';
        case eb::QualityBand::Orange: return 'O';
        case eb::QualityBand::Red:    return 'R';
        default:                      return '?';   // Unknown (no valid measurement)
    }
}

// Linear amplitude -> dBFS string for a log line (clamped at a -120 dB floor so a silent block reads
// "-inf"-free). Pure formatting helper for the diagnostic log.
static juce::String linToDbStr (float lin) {
    if (lin <= 1.0e-6f) return "<-120";
    return juce::String (juce::Decibels::gainToDecibels (lin, -120.0f), 1);
}

// Symmetric dB range that comfortably contains a cal curve (snapped to a 6 dB multiple).
static float fitTopDb (const eb::CalFile& c) {
    float lo = 0.0f, hi = 0.0f;
    for (auto& p : c.points) { lo = std::min (lo, (float) p.splDb); hi = std::max (hi, (float) p.splDb); }
    const float mag = std::max ({ 6.0f, std::abs (lo), std::abs (hi) }) + 3.0f;
    return std::ceil (mag / 6.0f) * 6.0f;
}

MainComponent::MainComponent() : MainComponent (TestConfig { juce::File{}, /*disableNetwork*/ false }) {}  // real app: per-user everything, network on
MainComponent::MainComponent (const TestConfig& cfg)
    : settings (cfg.settingsDir),
      appDataOverride_ (cfg.appDataDir) {
    const bool disableNetwork = cfg.disableNetwork;
    setLookAndFeel (&theme);
    firPool = std::make_unique<juce::ThreadPool> (1);

    // --- Diagnostic log: open it FIRST so the whole ctor (and every later handler) can write to it ---
    // %TEMP%/EarsBridge/logs, rotating + size-capped (see DiagnosticLog). Logging is message-thread only.
    // #24: a headless test overrides the dir so the gate never writes into the real log folder.
    log_ = std::make_unique<eb::DiagnosticLog> (
        cfg.logDir.getFullPathName().isNotEmpty()
            ? cfg.logDir
            : juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("EarsBridge").getChildFile ("logs"));
    // Launch banner: app version, build flavour, and the OS. The serial backstop is already armed
    // (loggedSerials_ is empty until a cal loads, so redactSerial is a no-op here).
   #ifdef NDEBUG
    const char* buildFlavour = "Release";
   #else
    const char* buildFlavour = "Debug";
   #endif
    logLine (eb::DiagnosticLog::Level::Info,
             juce::String ("==== EARS Bridge launch ==== v") + EB_VERSION_STRING
           + " build=" + buildFlavour
           + " os=" + juce::SystemStats::getOperatingSystemName());

    // --- Title bar: brand glyph is painted; the live format cluster (W2 .fmt) rides the bar's right side ---
    addAndMakeVisible (fmtCluster_);

    // Current-version footnote, pinned bottom-right. Reads the single version source of truth.
    versionLabel.setText ("v" EB_VERSION_STRING, juce::dontSendNotification);
    versionLabel.setColour (juce::Label::textColourId, Theme::textDim());   // HIG: textFaint was 2.4:1; textDim ~5.3:1
    versionLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    versionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (versionLabel);

    // --- Transport (gated Start/Stop; P3 Task 4 re-homed it into LevelStage - the stage adopt()s it
    // below, so no addAndMakeVisible here). The face (text+glyph+primary) is owned by syncTransport();
    // these seeds only cover the pre-first-updateStartGate construction window.
    startStop.getProperties().set ("primary", true);
    startStop.getProperties().set ("glyph", "play");
    startStop.onClick = [this] { onStartStop(); };
    statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    statusLine.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLine.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLine);
    // statusLineR: OFF-TREE since P3 Task 7 (see the header note) - updateStatusLine still commits
    // the ladder's line2 to it, but it is never parented; the VerdictCards render that truth now.
    statusLineR.setColour (juce::Label::textColourId, Theme::textDim());
    statusLineR.setFont (juce::Font (juce::FontOptions (12.0f)));

    // Update link: hidden until a newer release is found; opens the release page in the browser.
    updateLink.setColour (juce::HyperlinkButton::textColourId, Theme::infoText());   // HIG: accent-as-text was 3.5:1; infoText passes ~4.6:1
    updateLink.setFont (juce::Font (juce::FontOptions (12.0f)), false, juce::Justification::centredRight);
    addChildComponent (updateLink);

    // --- Wizard view layer: the spine + stage host replace the scrolling rail (P1 Task 3 cutover) ---
    // The stages adopt() the existing leaf controls below (construct-once/reparent-once). The spine is a
    // pure view of the WizardState; clicking a step pins it and re-resolves the view. addAndMakeVisible
    // BOTH; the controls are parented into their stages by the adopt() calls further down.
    spine_.onStepClicked = [this] (WizardStep s) { pinnedStep_ = s; refreshWizardView(); };
    // L2 (spec §10): region titles so the spine + stage area are natural a11y landmarks. The spine sets its
    // own "Setup steps" title (WizardSpine ctor); the stage AREA's landmark is the VISIBLE stage itself,
    // which already carries the active step name as its title (ConnectStage/... ctors) and swaps on
    // navigation — so the host is left untitled (mirroring the name onto the host would double-announce the
    // step to a screen reader AND trips the gate's duplicate-label check).
    setWantsKeyboardFocus (true);   // so Ctrl/Cmd+1..4 reach keyPressed when no child consumes them
    addAndMakeVisible (spine_);
    addAndMakeVisible (stageHost_);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    inputPicker.setTitle ("Input device");   // HIG: accessible name (the eyebrow label is not auto-associated to the control)
    // (No ctor setExplicitFocusOrder here: adopt() re-scopes every control's focus order per stage — minor-5.)
    inputGainHint.setText ("Leave the EARS gain switch alone (changing it drops the jig from Windows). "
                           "Set levels in Dirac: Master output, then Mic gain.",
                           juce::dontSendNotification);
    inputGainHint.setColour (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    inputGainHint.setJustificationType (juce::Justification::topLeft);
    inputGainHint.setMinimumHorizontalScale (1.0f);

    // --- Combine selector ---
    styleEyebrow (combineLabel, "COMBINE MODE");
    combineModel = combineModeOrder();
    for (size_t i = 0; i < combineModel.size(); ++i) {
        juce::PopupMenu::Item it (combineModeLabel (combineModel[i].mode));
        it.itemID = (int) i + 1;                                   // same id scheme as before (index+1)
        it.shortcutKeyDescription = combineModeBadge (combineModel[i]);   // dim right-aligned badge (P2.9): JUCE routes this field to drawPopupMenuItem's shortcutKeyText param
        combineBox.getRootMenu()->addItem (std::move (it));
    }
    combineBox.onChange = [this] { onCombineChosen(); };
    combineBox.setTitle ("Combine mode");
    combineHint.setColour (juce::Label::textColourId, Theme::textDim());
    combineHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    combineHint.setJustificationType (juce::Justification::topLeft);
    combineHint.setMinimumHorizontalScale (1.0f);

    // --- Output picker + Dirac hint + preflight ---
    outputPicker.onDeviceChosen = [this] (const DeviceId& d) { onOutputChosen (d); };
    outputPicker.setTitle ("Output virtual cable");
    outputHint.setText ("In Dirac Live, choose this device's capture side as the recording input.",
                        juce::dontSendNotification);
    outputHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    outputHint.setJustificationType (juce::Justification::topLeft);
    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    // Calm, neutral fact line (NOT a warning): e.g. "Output: 32-bit float (shared mode) - normal."
    // The full honest explanation lives in its tooltip so the short line always fits one rail line.
    preflightInfo.setColour (juce::Label::textColourId, Theme::textDim());
    preflightInfo.setFont (juce::Font (juce::FontOptions (12.0f)));
    preflightInfo.setTooltip ("WASAPI shared mode always delivers 32-bit float; your bit-depth is a "
                              "stored preference and doesn't affect quality" + kDash + "this is expected.");

    // Standard-VB-CABLE-vs-Dirac compatibility hint + one-click fix (hidden unless that cable is chosen).
    diracCableHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    diracCableHint.setJustificationType (juce::Justification::topLeft);
    diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
    diracFixButton.onClick = [this] {
        juce::String msg;
        if (eb::enableDiracSharedMode (msg)) {
            diracCableHint.setColour (juce::Label::textColourId, Theme::ok());
            diracCableHint.setText ("Dirac set to shared mode. " + msg, juce::dontSendNotification);
            diracFixButton.setVisible (false);
        } else {
            diracCableHint.setColour (juce::Label::textColourId, Theme::danger());
            diracCableHint.setText (msg, juce::dontSendNotification);
        }
        resized();
    };

    // --- Rate + depth ---
    styleEyebrow (rateLabel, "RATE");
    rateBox.onChange = [this] { onRateChosen(); };
    rateBox.setTitle ("Sample rate");
    rateWarn.setColour (juce::Label::textColourId, Theme::warn());
    rateWarn.setFont (juce::Font (juce::FontOptions (12.0f)));
    styleEyebrow (bitLabel, "BIT DEPTH");
    bitBox.onChange = [this] { onBitDepthChosen(); };
    bitBox.setTitle ("Preferred bit depth");

    // --- FIR / options controls (the Advanced disclosure died; children re-homed into stages) ---
    complexPhaseToggle.setButtonText ("Complex (with-phase) FIR");
    complexPhaseToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Complex (with-phase) FIR=") + (complexPhaseToggle.getToggleState() ? "on" : "off"));
        settings.setComplexPhase (complexPhaseToggle.getToggleState());
        rebuildFirsAsync();
    };
    autoUpdateToggle.setToggleState (settings.autoCheckUpdates(), juce::dontSendNotification);
    autoUpdateToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Automatically check for updates=") + (autoUpdateToggle.getToggleState() ? "on" : "off"));
        settings.setAutoCheckUpdates (autoUpdateToggle.getToggleState());
        flushSettings();
        if (! autoUpdateToggle.getToggleState() && updateLink.isVisible()) {
            updateLink.setVisible (false);
            resized();
        }
    };
    addAndMakeVisible (autoUpdateToggle);   // spine footer (§5.5): a persistent option, no longer disclosure-gated
    // #3: advanced override toggle. Restore its persisted state; on click, persist + re-run the gate.
    overrideToggle.setToggleState (settings.advancedOverride(), juce::dontSendNotification);
    overrideToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Allow non-Dirac use=") + (overrideToggle.getToggleState() ? "on" : "off"));
        settings.setAdvancedOverride (overrideToggle.getToggleState());
        flushSettings();
        updateStartGate();   // recompute Start enabled-ness + the status line for the new policy
        connectStage_.syncOverrideDisclosure();   // lock/unlock the "Not using Dirac?" row to the new state
    };
    // Hardware-Dirac toggle: ON -> grading runs OFF the loopback (a hardware box generates its own sweep, so the
    // loopback captures no reference). Persist + tell the engine (publishes GradingOffHardware + suppresses the
    // grade) + suppress Learn-reference (nothing to learn). The auto-detect only SUGGESTS this toggle.
    hwDiracToggle.setToggleState (settings.diracHardwareProcessor(), juce::dontSendNotification);
    hwDiracToggle.onClick = [this] {
        const bool on = hwDiracToggle.getToggleState();
        logLine (eb::DiagnosticLog::Level::Info, juce::String ("Toggle: Dirac hardware processor=") + (on ? "on" : "off"));
        settings.setDiracHardwareProcessor (on);
        flushSettings();
        engine.setDiracHardwareProcessor (on);   // publish GradingOffHardware / clear
        learnRefButton.setEnabled (! on);        // a hardware box has no PC-render reference to learn
    };
    if (settings.diracHardwareProcessor()) { engine.setDiracHardwareProcessor (true); learnRefButton.setEnabled (false); }
    styleEyebrow (firLenLabel, "FIR LENGTH");
    firLenBox.addItem ("Auto (scales with rate)", kFirLenAutoId);
    for (int n : { 4096, 8192, 16384, 32768 }) firLenBox.addItem (juce::String (n), n);
    firLenBox.onChange = [this] {
        const int id = firLenBox.getSelectedId();
        settings.setFirLength (id == kFirLenAutoId ? 0 : id);
        rebuildFirsAsync();
    };
    firLenBox.setTitle ("FIR length");   // HIG: accessible name (the eyebrow label is not auto-associated to the control)
    trimLabel.setText ("Output trim", juce::dontSendNotification);          // P2.9: parameter-row label, not an eyebrow
    trimLabel.setColour (juce::Label::textColourId, Theme::textDim());
    trimLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    trimSlider.setRange (-24.0, 0.0, 0.1);
    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    trimSlider.setTextValueSuffix (" dB");
    trimSlider.onValueChange = [this] {
        settings.setOutputTrimDb (trimSlider.getValue());
        engine.setOutputTrimDb (trimSlider.getValue());   // apply live (the graph reads it lock-free)
    };
    trimSlider.setTitle ("Output trim");   // HIG: accessible name

    // L/R wiring check: play a tone into the LEFT earcup, then the engine reports which mic responded.
    verifyButton.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Button: ") + (engine.lrVerifyActive() ? "Stop L/R check clicked"
                                                                      : "Check L/R wiring clicked"));
        if (engine.lrVerifyActive()) {   // toggle off
            engine.endLrVerify();
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setText ({}, juce::dontSendNotification);
            return;
        }
        juce::String err;
        if (engine.beginLrVerify (Ear::Left, err)) {
            verifyTicks = 1;   // running; timerCallback polls the verdict and times out
            verifyButton.setButtonText ("Stop check");
            verifyResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
            verifyResultLabel.setText ("Listening" + kDash + "play a tone in the LEFT earcup" + kEllipsis, juce::dontSendNotification);
        } else {
            verifyResultLabel.setColour (juce::Label::textColourId, Theme::warn());
            verifyResultLabel.setText (err, juce::dontSendNotification);
        }
    };
    verifyResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    verifyResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    // T10: the WIRING CHECK result shares one 32px row with the button - wrap to two lines rather than
    // squish the text horizontally when the verdict string is long.
    verifyResultLabel.setJustificationType (juce::Justification::centredLeft);
    verifyResultLabel.setMinimumHorizontalScale (1.0f);

    // Reference-Based Measurement Monitor (Plan 5): learn the loopback reference. The capture itself is a
    // Windows WASAPI loopback (on-device) and must run with Dirac's Processor in Windows Audio (shared)
    // mode; we detect that read-only and inform. Only meaningful while Stopped (the loopback can't run
    // alongside the live ASIO measurement).
    learnRefButton.onClick = [this] { onLearnReference(); };
    learnRefButton.getProperties().set ("glyph", "refresh");   // P2.9: re-run reference (W2 refresh)
    learnRefResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());

    // Diagnostic-log export (Task 3). "Open log folder" reveals %TEMP%/EarsBridge/logs; "Export log..."
    // zips the whole logs dir to a user-chosen path. Both are message-thread-only affordances.
    openLogButton.onClick = [this] {
        if (log_) {
            logLine (eb::DiagnosticLog::Level::Debug, "Button: Open log folder clicked");
            logLine (eb::DiagnosticLog::Level::Info, "User opened the log folder.");
            log_->directory().revealToUser();
        }
    };
    openLogButton.getProperties().set ("glyph", "folder");   // P2.9
    addAndMakeVisible (openLogButton);   // spine footer (§5.5): support channel <= 2 clicks from anywhere
    exportLogButton.onClick = [this] {
        if (log_ == nullptr) return;
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Export log clicked");
        logLine (eb::DiagnosticLog::Level::Info, "User requested a log export.");
        auto suggested = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                             .getChildFile ("EarsBridge-logs.zip");
        logChooser = std::make_unique<juce::FileChooser> ("Export EARS Bridge logs", suggested, "*.zip");
        const auto dir = log_->directory();
        logChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles,
            [this, dir] (const juce::FileChooser& fc) {
                auto target = fc.getResult();
                if (target == juce::File()) return;   // user cancelled
                if (target.getFileExtension().isEmpty()) target = target.withFileExtension ("zip");
                // Zip every file currently in the logs dir (eb.log + any rotated backups).
                juce::ZipFile::Builder builder;
                for (auto& f : dir.findChildFiles (juce::File::findFiles, false))
                    builder.addFile (f, 9 /*compression*/, f.getFileName());
                target.deleteFile();
                if (auto stream = target.createOutputStream()) {
                    const bool ok = builder.writeToStream (*stream, nullptr);
                    stream.reset();   // flush + close before we log the outcome
                    logLine (ok ? eb::DiagnosticLog::Level::Info : eb::DiagnosticLog::Level::Warn,
                             ok ? ("Log exported to \"" + target.getFullPathName() + "\"")
                                : ("Log export FAILED writing \"" + target.getFullPathName() + "\""));
                } else {
                    logLine (eb::DiagnosticLog::Level::Warn,
                             "Log export FAILED to open \"" + target.getFullPathName() + "\"");
                }
            });
    };
    exportLogButton.getProperties().set ("glyph", "export");   // P2.9
    addAndMakeVisible (exportLogButton);   // spine footer (§5.5)

    // --- Right pane: cal cards + Levels ---
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f);  updateStartGate(); syncPlotScales(); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); updateStartGate(); syncPlotScales(); };
    // Any load ATTEMPT - success OR failure (parse-error / missing / oversize) - revokes a session's
    // unity acceptance: the attempt signals intent to calibrate, so "Done (unity)" must never co-exist
    // with a red load error. Refresh the gate/wizard NOW so the revoke is visible even on the FAILED path
    // (a failed load never reaches onCalLoaded, which is where a success re-renders). On success onCalLoaded
    // fires next and re-refreshes with the applied state; its own reset stays (idempotent, belt+braces).
    leftCal.onLoadAttempted  = [this] { unityAcceptedSession_ = false; updateStartGate(); };
    rightCal.onLoadAttempted = [this] { unityAcceptedSession_ = false; updateStartGate(); };
    // Removing a slot resets that ear to unity AND bumps a fresh (now-incomplete) generation, so the
    // Start gate (calibrationApplied()) closes instead of staying satisfied by the prior valid build.
    leftCal.onCalCleared  = [this] { settings.setLeftCalPath  ({}); engine.clearLeftCalFir();  logLine (eb::DiagnosticLog::Level::Info, "Cal cleared: LEFT");  rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    rightCal.onCalCleared = [this] { settings.setRightCalPath ({}); engine.clearRightCalFir(); logLine (eb::DiagnosticLog::Level::Info, "Cal cleared: RIGHT"); rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    // #54: log a loaded cal's parse warnings (through the redacting logLine, since skipped-row text can embed header content).
    leftCal.onParseWarnings  = [this] (const juce::StringArray& w) { for (auto& s : w) logLine (eb::DiagnosticLog::Level::Warn, "Cal parse (LEFT): "  + s); };
    rightCal.onParseWarnings = [this] (const juce::StringArray& w) { for (auto& s : w) logLine (eb::DiagnosticLog::Level::Warn, "Cal parse (RIGHT): " + s); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);
    styleEyebrow (levelsEyebrow, "LEVELS");
    addAndMakeVisible (levelsEyebrow);
    levelsHint.setText ("Set Dirac's Master output so the L and R meters reach the green band.", juce::dontSendNotification);
    levelsHint.setColour (juce::Label::textColourId, Theme::textDim());
    levelsHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    levelsHint.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (levelsHint);
    // Dirac Mic-gain caption: EARS Bridge attenuates its output for auto-headroom; tell the user to add
    // about that many dB of positive Mic gain in Dirac so Dirac records at a healthy level. The number
    // comes from engine.headroomAttenuationDb() and is refreshed whenever a cal loads (updateStartGate).
    diracMicGainHint.setColour (juce::Label::textColourId, Theme::textDim());
    diracMicGainHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    diracMicGainHint.setJustificationType (juce::Justification::topLeft);
    diracMicGainHint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (diracMicGainHint);
    updateDiracMicGainHint();   // seed it (likely the ~0 dB form until cals load)
    meterL.setTitle ("Left input level");
    meterR.setTitle ("Right input level");
    meterOut.setTitle ("Output level");
    // Capture meters carry the green target band (a level to aim the amp at); the Out meter does not.
    meterL.setShowTargetBand (true);
    meterR.setShowTargetBand (true);
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    addAndMakeVisible (meterOut);
    inputClipHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    inputClipHint.setJustificationType (juce::Justification::topLeft);
    inputClipHint.setColour (juce::Label::textColourId, Theme::danger());
    inputClipHint.setText (kInputClipHintText, juce::dontSendNotification);
    addChildComponent (inputClipHint);   // hidden until a raw-input clip is seen

    // --- Restore persisted state ---
    // Search combineModel for the saved mode rather than assuming menu position == enum value, so the
    // menu can be reordered without silently restoring the wrong mode.
    for (size_t i = 0; i < combineModel.size(); ++i)
        if (combineModel[i].mode == settings.combineMode())
            { combineBox.setSelectedId ((int) i + 1, juce::dontSendNotification); break; }
    complexPhaseToggle.setToggleState (settings.complexPhase(), juce::dontSendNotification);
    firLenBox.setSelectedId (settings.firLength() > 0 ? settings.firLength() : kFirLenAutoId,
                             juce::dontSendNotification);
    trimSlider.setValue (settings.outputTrimDb(), juce::dontSendNotification);
    onCombineChosen();   // seed the combine helper text

    // Live hot-plug: when the OS device list changes, re-populate the pickers (no restart needed),
    // re-sync the engine selection while stopped, and re-evaluate the Start gate.
    engine.onDevicesChanged = [this] {
        refreshDeviceLists();
        autoSelectDefaults();   // a freshly-plugged EARS / cable gets auto-selected into an empty slot
        if (engine.status() != EngineStatus::Running) {
            // Route the hot-plug input through applyResolvedInput so a re-resolution that lands on a
            // DIFFERENT device (incl. the EARS gain-DIP rename fallback) clears the Level green-band latch
            // (§3.2) — the direct engine.setInput here used to skip that, so Level kept claiming "In green
            // band" on old-gain evidence after a replug. The OUTPUT routes through applyResolvedOutput for
            // the same doctrine (P3 Task 5 obligation): a re-resolution onto a DIFFERENT cable is a
            // different signal path into Dirac, so the old-path verdicts must read stale.
            if (auto in  = inputPicker.selectedDevice())  applyResolvedInput (*in);
            if (auto out = outputPicker.selectedDevice()) applyResolvedOutput (*out);
            rebuildRateMenu();
            rebuildBitDepthMenu();
            updateDiracCableHint();
        }
        updateStartGate();
        logDeviceSnapshot ("devices changed (hot-plug)");
    };

    refreshDeviceLists();
    autoSelectDefaults();   // first run / saved device absent: auto-pick a detected EARS + standard VB-CABLE
    // refreshDeviceLists()/setDevices() only PRESELECT the saved devices in the pickers — they do
    // NOT notify the engine (that happens only when the user manually picks). Without this, a fresh
    // launch + Start opens an unset input and fails with "could not create input device". Push the
    // restored sample rate + input/output selections to the engine here.
    engine.setSampleRate (settings.sampleRate());
    engine.setOutputTrimDb (settings.outputTrimDb());   // apply the restored Output-trim to the graph
    // Push the restored selection to the engine. Heal the INPUT key if an old-format / pre-rekey key
    // resolved via the single-EARS fallback (reliably the user's one EARS, so persisting is safe). Do
    // NOT auto-persist the OUTPUT: its fallback can pick a *different* virtual sink, and silently
    // overwriting the saved cable with a best-guess is worse than re-resolving it each launch.
    if (auto in  = inputPicker.selectedDevice())  { applyResolvedInput (*in);  if (in->key() != settings.inputKey()) settings.setInputKey (in->key()); }
    if (auto out = outputPicker.selectedDevice()) applyResolvedOutput (*out);   // seeds the output-key memo
    rebuildRateMenu();
    rebuildBitDepthMenu();
    updateDiracCableHint();   // show the standard-cable/Dirac warning on launch if applicable

    if (settings.leftCalPath().isNotEmpty())
        leftCal.loadFromFile (juce::File (settings.leftCalPath()));
    if (settings.rightCalPath().isNotEmpty())
        rightCal.loadFromFile (juce::File (settings.rightCalPath()));

    loadStoredReference();   // reload a previously-learned reference so it survives a restart

    // Initial device + Dirac snapshot for the log (after the saved devices/cals are restored).
    logDeviceSnapshot ("launch");
    logDiracSnapshot  ("launch");
    if (! loadedReferenceL_.empty())
        logLine (eb::DiagnosticLog::Level::Info,
                 "Reference: reloaded a stored loopback reference ("
               + juce::String (loadedReferenceL_.size() / loadedReferenceRateL_, 1) + " s)");

    // --- Wizard stages: reparent every leaf control into its stage ONCE (construct-once/reparent-once) ---
    // adopt() calls addAndMakeVisible on each control, moving it from `this`/its prior parent into the stage.
    // Order matches the §5 re-homing map exactly; nothing is dropped or double-homed.
    connectStage_.adopt (inputPicker, inputGainHint, combineLabel, combineBox, combineHint,
                         outputPicker, outputHint, preflightLabel, preflightInfo,
                         diracCableHint, diracFixButton, rateLabel, rateBox, rateWarn,
                         bitLabel, bitBox, verifyButton, verifyResultLabel, overrideToggle);
    connectStage_.syncOverrideDisclosure();   // restore: a persisted ON override opens the disclosure locked
    calibrateStage_.adopt (leftCal, rightCal, complexPhaseToggle,
                           firLenLabel, firLenBox, trimLabel, trimSlider);
    // Advanced FIR: open at launch iff any setting is non-default (a hidden non-default would
    // be a silent config surprise; the summary line states the values either way). ONE home for
    // the rule (P2.9 T7) - shared with the harness restore below so they can never drift.
    calibrateStage_.setAdvancedOpen (CalibrateStage::advancedFirNonDefault (
        settings.complexPhase(), settings.firLength(), settings.outputTrimDb()));
    levelStage_.adopt (startStop, levelsEyebrow, levelsHint, diracMicGainHint, meterL, meterR, meterOut, inputClipHint);
    measureStage_.adopt (statusLine, learnRefButton, learnRefResultLabel, hwDiracToggle);
    // §3.3: Measure's header CTA is this stage's transport - the SAME one engine action (onStartStop),
    // a per-stage face (syncTransport owns "Start listening"/"Stop"; Task 7 adds the Measure-again arm).
    measureStage_.onTransport = [this] { onMeasureTransport(); };
    stageHost_.setStages ({ &connectStage_, &calibrateStage_, &levelStage_, &measureStage_ });
    // The stage Continue callbacks are created disabled + unwired in P1 (Task 4 wires enablement + the pin
    // navigation); give them the same pin-and-refresh path the spine uses so the seam is already in place.
    connectStage_.onContinue   = [this] { pinnedStep_ = WizardStep::Calibrate; refreshWizardView(); };
    calibrateStage_.onContinue = [this] { pinnedStep_ = WizardStep::Level;     refreshWizardView(); };
    // §5.2 unity path: an EXPLICIT choice to continue without calibration. It flips the SESSION-scoped
    // flag (never persisted) so the WIZARD's Calibrate reads Done, then navigates like any continue. It
    // deliberately does NOT touch the engine Start gate (startReady already allows noCalsLoaded), and is
    // NOT frozen while Running (accepting unity changes wizard done-ness only, never the engine).
    calibrateStage_.onContinueWithoutCal = [this] {
        unityAcceptedSession_ = true;                        // explicit choice, this session only
        logLine (eb::DiagnosticLog::Level::Info,
                 "Calibrate: continue without calibration (unity passthrough)");
        pinnedStep_ = WizardStep::Level;                     // user-invoked navigation (a continue)
        refreshWizardView();
    };
    levelStage_.onContinue     = [this] { pinnedStep_ = WizardStep::Measure;   refreshWizardView(); };

    updateStartGate();
    refreshWizardView();   // seed the spine + resolve the launch stage (first unmet; all met => Measure)
    syncPlotScales();
    setSize (960, 780);   // >= the 900-wide minimum (§4); tall enough for the stage content without cramping
    startTimerHz (30);

    // Background update check: runs on every launch, gated only by the user's opt-out toggle.
    // It is async + non-blocking, so a newer release surfaces the title-bar link each time the
    // app starts. (No throttle: GitHub's unauthenticated rate limit is 60/h — far above any
    // realistic launch rate — and a failed/offline check simply shows nothing and retries next time.)
    if (settings.autoCheckUpdates() && ! disableNetwork) {
        updateChecker.start (juce::String (EB_VERSION_STRING),
            [this] (UpdateInfo info) {
                if (info.updateAvailable) {
                    updateLink.setButtonText ("Update available" + kDash + "v" + info.latestVersion);
                    updateLink.setURL (juce::URL (info.releaseUrl));
                    updateLink.setVisible (true);
                    resized();
                }
            });
    }
}

MainComponent::~MainComponent() {
    stopTimer();
    // Arm the learn cancel token BEFORE waiting on the pool: a live Learn capture polls it every packet
    // (~10 ms) and exits well inside the 2 s wait. Without this, quitting mid-learn timed out here, then
    // ~ThreadPool force-killed (TerminateThread) the worker INSIDE a live WASAPI loopback - a ~7.5 s frozen
    // shutdown + leaked COM/audio-client state (audit #2). Bump the grade generation for the same reason
    // (a queued grade continuation must not land during teardown).
    learnCancelRequested_.store (true);
    gradeRunGen_.fetch_add (1);
    if (firPool) firPool->removeAllJobs (true, 2000);
    engine.stop();
    setLookAndFeel (nullptr);
}

double MainComponent::activeRate() const { return settings.sampleRate(); }

// ---- Diagnostic log (Task 3) ----------------------------------------------------------
void MainComponent::logLine (eb::DiagnosticLog::Level level, const juce::String& msg) {
    // The backstop: every message is scrubbed of the loaded serial (in both its dashed and dash-less
    // forms) before it can land on disk, so the EARS serial can NEVER leak even if a call site forgets.
    // Call sites already avoid the value; this is defense in depth. MESSAGE-THREAD ONLY.
    if (log_ == nullptr) return;
    juce::String scrubbed = msg;
    for (const auto& s : loggedSerials_) scrubbed = eb::DiagnosticLog::redactSerial (scrubbed, s);   // #51: scrub ALL known serials
    log_->write (level, scrubbed);
}

void MainComponent::flushSettings() {
    if (! settings.flush() && ! settingsSaveWarned_) {   // #53: a silent save failure loses the user's selections
        settingsSaveWarned_ = true;
        logLine (eb::DiagnosticLog::Level::Warn,
                 "Settings save FAILED (read-only profile / file lock?) - selections will not persist this session.");
    }
}

void MainComponent::logDeviceSnapshot (const juce::String& reason) {
    // Selected input/output name + endpoint format + virtual-sink classification + the live raw-rail
    // (OS-SRC) state, pulled entirely from the EXISTING device-layer reads — no new probing. Granted
    // rate/depth come from the engine's last open; the WASAPI mix-format rate from EndpointFormat.
    const auto in  = inputPicker.selectedDevice();
    const auto out = outputPicker.selectedDevice();
    juce::String m = "Devices (" + reason + "): ";
    if (in) {
        const double mix = eb::endpointMixSampleRateForName (in->name, /*isInput*/ true);
        m << "input=\"" << in->name << "\" key=" << in->key()
          << " requestedRate=" << juce::String (settings.sampleRate(), 0)
          << " mixRate=" << juce::String (mix, 0);
    } else {
        m << "input=<none>";
    }
    m << "; ";
    if (out) {
        const auto kind = eb::DeviceManager::classifyVirtualSink (out->name);
        const char* kindStr = kind == eb::DeviceManager::VirtualSinkKind::StdVbCable  ? "StdVbCable"
                            : kind == eb::DeviceManager::VirtualSinkKind::HiFiCable    ? "HiFiCable"
                            : kind == eb::DeviceManager::VirtualSinkKind::OtherVirtual ? "OtherVirtual"
                                                                                       : "NotVirtual";
        m << "output=\"" << out->name << "\" key=" << out->key()
          << " virtualSink=" << (out->isVirtualSink ? "yes" : "no")
          << " sinkKind=" << kindStr
          << " preferredDepth=" << juce::String (settings.outputBitDepth()) << "-bit";
    } else {
        m << "output=<none>";
    }
    // Live format facts when running: the GRANTED rate/depth (WASAPI shared mode can override the
    // request) + the raw-rail (was the input OS-resampled?) + the sticky FormatChanged flag.
    if (engine.status() == EngineStatus::Running) {
        const auto rr = engine.rawRail();
        const auto fl = engine.health().flags;
        m << "; granted: rate=" << juce::String (engine.grantedSampleRate(), 0)
          << " depth=" << juce::String (engine.grantedOutputBitDepth()) << "-bit (shared/float)"
          << " rawRailVerified=" << (rr.verified ? "yes" : "no")
          << " osResampled=" << (any (fl & HealthFlag::OsResampled) ? "yes" : "no")
          << " formatChanged=" << (any (fl & HealthFlag::FormatChanged) ? "yes" : "no");
    }
    logLine (eb::DiagnosticLog::Level::Info, m);
}

void MainComponent::logDiracSnapshot (const juce::String& reason) {
    // Read-only Dirac config: version, the Processor's output deviceType (must be Windows Audio for a
    // learn to capture the loopback), the device Dirac plays the sweep TO, and the active device rate.
    // Every read is best-effort and returns "" off-Windows / when the settings file is absent.
    const juce::String ver  = eb::readDiracVersion();
    const juce::String type = eb::readDiracDeviceType();
    const juce::String outDev = eb::readDiracOutputDeviceName();
    juce::String m = "Dirac (" + reason + "): version=" + (ver.isNotEmpty() ? ver : juce::String ("unknown"))
        + " deviceType=" + (type.isNotEmpty() ? type : juce::String ("unknown"))
        + " outputDevice=" + (outDev.isNotEmpty() ? ("\"" + outDev + "\"") : juce::String ("unknown"))
        + " deviceRate=" + juce::String (activeRate(), 0);
    logLine (eb::DiagnosticLog::Level::Info, m);
}

bool MainComponent::isRealEarsInput() const noexcept {
    const auto in = inputPicker.selectedDevice();
    return in && (in->model == EarsModel::Ears || in->model == EarsModel::EarsPro);
}

bool MainComponent::isRealEarsWithCable() const noexcept {
    const auto out = outputPicker.selectedDevice();
    return isRealEarsInput() && out && out->isVirtualSink;
}

void MainComponent::refreshDeviceLists() {
    inputPicker.setDevices  (engine.inputDevices(),  settings.inputKey());
    outputPicker.setDevices (engine.outputDevices(), settings.outputKey());
}

void MainComponent::autoSelectDefaults() {
    // Convenience: when a device slot has no selection (first run, or the saved device is absent), pick
    // the obvious default so the user doesn't have to. Input -> a recognised EARS / EARS Pro; output ->
    // a STANDARD VB-CABLE (never the Hi-Fi Cable, which has no SRC). Skipped while running, and skipped
    // whenever a slot already holds a selection, so it never overrides an explicit choice.
    if (engine.status() == EngineStatus::Running) return;

    if (! inputPicker.selectedDevice()) {
        auto ins = engine.inputDevices();
        for (auto& d : ins)
            if (d.model != EarsModel::Unknown) {            // a recognised EARS in the capture list
                inputPicker.setDevices (ins, d.key());
                settings.setInputKey (d.key());             // persist: reliably the user's one jig
                break;
            }
    }
    if (! outputPicker.selectedDevice()) {
        auto outs = engine.outputDevices();
        for (auto& d : outs)
            if (DeviceManager::classifyVirtualSink (d.name) == DeviceManager::VirtualSinkKind::StdVbCable) {
                outputPicker.setDevices (outs, d.key());     // not persisted: re-derived each launch
                break;
            }
    }
}

void MainComponent::applyResolvedInput (const DeviceId& d) {
    engine.setInput (d);
    // §3.2: a changed input device INVALIDATES the Level green-band latch — the latch is per-device level
    // evidence, and a different device (or the same jig re-enumerated at a different gain, via the EARS
    // gain-DIP rename fallback) is different evidence. Both the user pick and the hot-plug re-resolution
    // land here, so the spine never keeps reading "In green band" on old-gain evidence. Guard on the KEY so
    // a hot-plug that re-applies the SAME device does not spuriously drop a still-valid latch.
    if (d.key() != lastAppliedInputKey_) {
        levelLatched_       = false;
        lastAppliedInputKey_ = d.key();
        // Same doctrine, applied to the VERDICTS (P3 Task 1 review, Major): a different device is
        // different evidence, so grades measured on the old jig must not present fresh after a replug
        // (the EARS gain-DIP rename fallback lands here too, and it is a real occurrence). Advance the
        // config generation so verdictGen < configGen downgrades them to STALE. Direct bump, not
        // rebuildFirsAsync(): the input's identity never enters the FIR design, so a rebuild here would
        // be wasted work (the user-pick path still rebuilds separately in onInputChosen for the
        // rate-menu fix-up it performs).
        bumpConfigGeneration();
    }
}

void MainComponent::applyResolvedOutput (const DeviceId& d) {
    engine.setOutput (d);
    // P3 Task 5 ledgered obligation (Task 1 review + user ruling): the OUTPUT mirror of the input-key
    // memo above. The hot-plug/ctor re-resolution paths call engine.setOutput directly (bypassing
    // onOutputChosen), so a re-resolution landing on a DIFFERENT virtual cable used to leave old-path
    // verdicts presenting fresh. A changed output KEY advances the config generation (staleness only —
    // the output identity never enters the FIR design, so no rebuild). Guard on the key so a hot-plug
    // that re-applies the SAME cable does not spuriously downgrade still-valid verdicts. Belt-and-braces
    // with the refEndpointMismatch_ wait-hint (#34): hard staleness AND the hint, per the user ruling.
    if (d.key() != lastAppliedOutputKey_) {
        lastAppliedOutputKey_ = d.key();
        bumpConfigGeneration();
    }
}

void MainComponent::onInputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    applyResolvedInput (d);   // engine.setInput + latch invalidation on a key change (§3.2)
    settings.setInputKey (d.key());
    auto rates = engine.supportedSampleRates (d);
    if (! rates.empty()) {
        bool ok = false;
        for (double r : rates) if (std::abs (r - settings.sampleRate()) < 0.5) ok = true;
        if (! ok) { settings.setSampleRate (rates.front()); engine.setSampleRate (rates.front()); }
    }
    rebuildRateMenu();
    rebuildBitDepthMenu();
    rebuildFirsAsync();
    updateStartGate();   // input now selected -> may enable Start
    logDeviceSnapshot ("input changed");
}

void MainComponent::onOutputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    engine.setOutput (d);
    lastAppliedOutputKey_ = d.key();   // keep the staleness memo in sync (this path bumps below anyway)
    settings.setOutputKey (d.key());
    preflightLabel.setText (d.isVirtualSink ? juce::String()
                                            : "Selected output is not a known virtual cable.",
                            juce::dontSendNotification);
    preflightInfo.setText ({}, juce::dontSendNotification);   // a fresh pick clears any prior-run fact line
    rebuildBitDepthMenu();
    updateDiracCableHint();
    // USER RULING (P3 Task 1 review): a different output device is a different signal path into Dirac,
    // so verdicts measured through the old device must read STALE. Note the bit-depth fallback inside
    // rebuildBitDepthMenu() above (a saved depth the new device lacks silently becomes 24-bit) is also
    // covered by this one bump. Belt-and-braces with the PLANNED P3 wait-hint (refEndpointMismatch_,
    // a later task): that hint will WARN that the learned reference no longer matches the endpoint;
    // this bump independently downgrades verdict freshness - they coexist, neither replaces the other.
    bumpConfigGeneration();
    updateStartGate();   // output now selected -> may enable Start
    logDeviceSnapshot ("output changed");
}

void MainComponent::updateDiracCableHint() {
    auto out = outputPicker.selectedDevice();
    // Route off the single virtual-sink classifier so the preflight (isVirtualSink) and this hint can't
    // drift. Std VB-CABLE: Dirac records it exclusive (600007) -> one-click shared-mode fix. Hi-Fi
    // Cable: bit-perfect, no SRC -> connects but drops our mono feed (silent measurement) -> warn off it.
    using VK = eb::DeviceManager::VirtualSinkKind;
    const VK kind = out ? eb::DeviceManager::classifyVirtualSink (out->name) : VK::NotVirtual;

    if (kind == VK::HiFiCable) {
        diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
        diracCableHint.setText (eb::hints::kDiracHiFiCable, juce::dontSendNotification);
        diracFixButton.setVisible (false);
    } else if (kind == VK::StdVbCable) {
        if (eb::diracSharedModeEnabled()) {
            diracCableHint.setColour (juce::Label::textColourId, Theme::ok());
            diracCableHint.setText (eb::hints::kDiracStdCableShared, juce::dontSendNotification);
            diracFixButton.setVisible (false);
        } else {
            diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
            diracCableHint.setText (eb::hints::kDiracStdCableExclusive, juce::dontSendNotification);
            diracFixButton.setVisible (true);
        }
    } else if (kind == VK::OtherVirtual) {
        diracCableHint.setColour (juce::Label::textColourId, Theme::textDim());
        diracCableHint.setText (eb::hints::kDiracOtherVirtual, juce::dontSendNotification);
        diracFixButton.setVisible (! eb::diracSharedModeEnabled());   // the one-click fix helps any cable
    } else {   // NotVirtual
        diracCableHint.setVisible (false);
        diracFixButton.setVisible (false);
        resized();
        return;
    }
    diracCableHint.setVisible (true);
    resized();
}

void MainComponent::rebuildRateMenu() {
    auto sel = inputPicker.selectedDevice();
    std::vector<double> native = sel ? engine.supportedSampleRates (*sel)
                                     : std::vector<double> { 48000.0 };
    rateModel = buildRateMenu (native, settings.sampleRate());
    rateBox.clear (juce::dontSendNotification);
    int selectId = 0;
    bool warn = false;
    for (size_t i = 0; i < rateModel.size(); ++i) {
        auto& it = rateModel[i];
        juce::String txt = juce::String (it.rate / 1000.0, 1) + " kHz";
        if (it.resampleWarning) txt += "  (resample)";
        rateBox.addItem (txt, (int) i + 1);
        if (it.selected) { selectId = (int) i + 1; warn = it.resampleWarning; }
    }
    if (selectId != 0) rateBox.setSelectedId (selectId, juce::dontSendNotification);
    rateWarn.setText (warn ? juce::String (eb::hints::kRateResample) : juce::String(),
                      juce::dontSendNotification);
}

void MainComponent::rebuildBitDepthMenu() {
    auto sel = outputPicker.selectedDevice();
    bitModel = sel ? engine.supportedBitDepths (*sel) : std::vector<int> { 16, 24, 32 };
    std::vector<int> allowed;
    for (int b : bitModel) if (b == 16 || b == 24 || b == 32) allowed.push_back (b);
    if (allowed.empty()) allowed = { 24 };
    bitModel = allowed;
    bitBox.clear (juce::dontSendNotification);
    int selectId = 0;
    for (size_t i = 0; i < bitModel.size(); ++i) {
        bitBox.addItem (juce::String (bitModel[i]) + "-bit", (int) i + 1);
        if (bitModel[i] == settings.outputBitDepth()) selectId = (int) i + 1;
    }
    if (selectId == 0 && ! bitModel.empty()) {
        int defaultIdx = 0;
        for (size_t i = 0; i < bitModel.size(); ++i) if (bitModel[i] == 24) { defaultIdx = (int) i; break; }
        selectId = defaultIdx + 1;
        settings.setOutputBitDepth (bitModel[(size_t) defaultIdx]);
    }
    if (selectId != 0) bitBox.setSelectedId (selectId, juce::dontSendNotification);
    engine.setOutputBitDepth (settings.outputBitDepth());
}

void MainComponent::onRateChosen() {
    const int idx = rateBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) rateModel.size()) return;
    const double sr = rateModel[(size_t) idx].rate;
    settings.setSampleRate (sr);
    engine.setSampleRate (sr);
    rateWarn.setText (rateModel[(size_t) idx].resampleWarning ? juce::String (eb::hints::kRateResample)
                                                              : juce::String(),
                      juce::dontSendNotification);
    rebuildFirsAsync();
    updateStatusLine();
    logLine (eb::DiagnosticLog::Level::Info,
             "Rate changed: " + juce::String (sr, 0) + " Hz"
           + (rateModel[(size_t) idx].resampleWarning ? " (resample)" : ""));
}

void MainComponent::onBitDepthChosen() {
    const int idx = bitBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) bitModel.size()) return;
    settings.setOutputBitDepth (bitModel[(size_t) idx]);
    engine.setOutputBitDepth (bitModel[(size_t) idx]);
    logLine (eb::DiagnosticLog::Level::Info,
             "Bit depth changed: preferred " + juce::String (bitModel[(size_t) idx]) + "-bit");
    // USER RULING (P3 Task 1 review): an output bit-depth change re-quantizes the signal Dirac records,
    // so verdicts measured under the old depth must read STALE. Mirrors the rate path's generation
    // advance (onRateChosen -> rebuildFirsAsync), minus the FIR redesign: unlike the rate, the bit
    // depth never enters the FIR design, so the non-rebuilding bump stamps the same staleness.
    bumpConfigGeneration();
}

void MainComponent::onCombineChosen() {
    const int idx = combineBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) combineModel.size()) return;
    const auto mode = combineModel[(size_t) idx].mode;
    settings.setCombineMode (mode);
    engine.setCombineMode (mode);
    const char* modeName = mode == CombineMode::AutoPerEar ? "Auto per-ear"
                         : mode == CombineMode::Average    ? "Average"
                         : mode == CombineMode::Sum        ? "Sum"
                         : mode == CombineMode::LeftOnly   ? "Left only"
                                                           : "Right only";
    logLine (eb::DiagnosticLog::Level::Debug, juce::String ("Combine mode: ") + modeName);
    juce::String h;
    switch (mode) {
        case CombineMode::AutoPerEar:
            h = "Use this with Dirac. Records only the earcup Dirac is sweeping" + kDash + "clean per-ear "
                "capture in one pass, no open-back crosstalk.";
            break;
        case CombineMode::Average:
            h = "Not for Dirac (it won't follow Dirac's sweep, so Dirac errors). Mono average for "
                "other/test uses; folds in open-back leakage on open-backs.";
            break;
        case CombineMode::Sum:
            h = "Not for Dirac. Sums both ears (+6 dB) for other/test uses; clip risk.";
            break;
        case CombineMode::LeftOnly:
        case CombineMode::RightOnly:
            h = "Not for Dirac. One fixed ear won't follow Dirac's both-channel sweep, so Dirac "
                "errors. For testing or non-Dirac use.";
            break;
    }
    combineHint.setText (h, juce::dontSendNotification);
    updateStartGate();   // D7: re-evaluate Start gate immediately on mode change
}

void MainComponent::onLeftCalLoaded (const juce::File& f) {
    unityAcceptedSession_ = false;   // new evidence supersedes the unity choice (map #8)
    settings.setLeftCalPath (f.getFullPathName());
    // Track the serial so logLine() can scrub it everywhere (the backstop). NEVER log the value itself —
    // only that a serial is present and its length, per the brief.
    juce::String serial;
    if (auto c = leftCal.calFile()) serial = c->serial;
    if (serial.isNotEmpty()) loggedSerials_.addIfNotAlreadyThere (serial);   // #51: accumulate both ears' serials
    logLine (eb::DiagnosticLog::Level::Info,
             "Cal load: LEFT serial present (" + juce::String (serial.length()) + " chars)");
    rebuildFirsAsync();
}
void MainComponent::onRightCalLoaded (const juce::File& f) {
    unityAcceptedSession_ = false;   // new evidence supersedes the unity choice (map #8)
    settings.setRightCalPath (f.getFullPathName());
    juce::String serial;
    if (auto c = rightCal.calFile()) serial = c->serial;
    if (serial.isNotEmpty()) loggedSerials_.addIfNotAlreadyThere (serial);   // #51: accumulate both ears' serials
    logLine (eb::DiagnosticLog::Level::Info,
             "Cal load: RIGHT serial present (" + juce::String (serial.length()) + " chars)");
    rebuildFirsAsync();
}

void MainComponent::rebuildFirsAsync() {
    const int genId = ++calGenCounter_;                 // monotonic request id
    engine.setRequestedGeneration (genId);
    const double sr = activeRate();
    const int taps = (settings.firLength() > 0) ? settings.firLength() : numTapsForRate (sr);
    const auto mode = settings.complexPhase() ? FirMode::ComplexWithPhase : FirMode::MinPhaseMagnitude;
    auto left  = leftCal.calFile();                     // std::optional<CalFile> (copies)
    auto right = rightCal.calFile();
    juce::Component::SafePointer<MainComponent> safe (this);

    // #14: purge ONLY superseded FIR-build jobs. The old removeAllJobs(false,0) deleted EVERY queued job on
    // the single-threaded pool - including a queued Learn capture, whose continuation (the only place
    // learning_ clears) then never ran, wedging the Learn button for the session. FIR builds are posted as
    // NAMED jobs so the purge can select exactly them; the genId check below remains the correctness guard.
    struct FirBuildSelector : juce::ThreadPool::JobSelector {
        bool isJobSuitable (juce::ThreadPoolJob* j) override { return j->getJobName() == "fir-build"; }
    } firSel;
    firPool->removeAllJobs (false, 0, &firSel);

    struct FirBuildJob : juce::ThreadPoolJob {
        std::function<void()> work;
        explicit FirBuildJob (std::function<void()> w) : juce::ThreadPoolJob ("fir-build"), work (std::move (w)) {}
        JobStatus runJob() override { work(); return jobHasFinished; }
    };
    firPool->addJob (new FirBuildJob ([safe, genId, sr, taps, mode, left, right]() mutable {
        eb::CalibrationGeneration g;
        g.id = genId; g.sampleRate = sr; g.taps = taps; g.mode = mode;
        if (left && right) {
            g.leftHash = left->contentHash; g.rightHash = right->contentHash; g.serial = left->serial;
            const auto v = eb::validateCalibrationPair (*left, *right, mode);   // P0-07
            g.valid = v.valid; g.diagnostic = v.reason;
            if (g.valid) {
                FirDesignParams p; p.sampleRate = sr; p.numTaps = taps; p.mode = mode; p.invert = true;
                g.leftFir  = FirDesigner::design (*left,  p);                   // BOTH ears in ONE job
                g.rightFir = FirDesigner::design (*right, p);
            }
        } else {
            g.diagnostic = "Load both ear calibrations";
        }
        juce::MessageManager::callAsync ([safe, g = std::move (g)]() mutable {
            auto* mc = safe.getComponent();
            if (! mc) return;
            if (g.id != mc->calGenCounter_.load()) return;   // STALE: a newer request superseded this build -> discard
            mc->engine.applyCalibrationGeneration (std::move (g));
            mc->updateStartGate();
        });
    }), /*deleteJobWhenFinished*/ true);
    // #3: the generation just bumped, so the gate is logically CLOSED until the async build lands - grey the
    // Start button NOW (rate/FIR-length/complex handlers previously left it enabled through the build window).
    updateStartGate();
}

// P3 Task 1 review: advance the config generation WITHOUT redesigning the FIRs, so verdicts stamped under
// the previous generation read STALE (verdictGen < configGen) while the Start gate and calBuilding are
// untouched (both read the ENGINE's requested/built generations, which this leaves converged). Used where
// the measurement CONTEXT changed but the FIR content did not (input identity, output device, output bit
// depth) - a full rebuildFirsAsync() here would redo identical design work and grey the gate through the
// build window for an unchanged result. Staleness-conservative: the counter only ever advances.
// If a FIR build IS in flight (requested != built), route through rebuildFirsAsync() instead: a bare
// counter bump would orphan the in-flight build (its continuation discards on the genId mismatch, so
// requested/built would never converge), wedging the gate closed on a phantom "rebuilding".
void MainComponent::bumpConfigGeneration() {
    if (engine.requestedGeneration() != engine.builtGeneration()) { rebuildFirsAsync(); return; }
    calGenCounter_.fetch_add (1, std::memory_order_relaxed);
    updateStartGate();   // same gate outcome, but the wizard view must re-render verdictsStale NOW
}

void MainComponent::onStartStop() {
    if (engine.status() == EngineStatus::Running) {
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Stop clicked");
        engine.stop();
        // (P3 Task 4: the transport face is owned by syncTransport, reached via updateStartGate below.)
        // Task 4: re-arm BOTH per-ear grade pollers on Stop so a stale match from this run can't carry into
        // the next (each ear again needs two consecutive matched polls before its first grade).
        gradePollTick_ = 0; gradePollerL_.reset(); gradePollerR_.reset();
        // Release the off-thread grade guard on Stop. It is normally cleared by the worker chain (didGrade-false at
        // the post, or after the publish), but if a continuation is ever dropped the guard would latch true and
        // SILENTLY kill all future grading. Clearing it on every Stop (and Start, below) guarantees a fresh run is
        // never gated off. Safe: clearing it can only ALLOW a grade, never falsely green one. (Review #3.)
        gradeInFlight_.store (false);
        gradeRunGen_.fetch_add (1, std::memory_order_relaxed);   // invalidate any in-flight grade job from this run (#4)
        // Re-arm the hardware-Dirac auto-detect too: a fresh run must re-observe the output silence + mic sweep.
        maxOutputRenderPeak_ = 0.0f; hwMicRunPeak_ = 0.0f; hwOutputReadable_ = false; hwDetectTick_ = 0;
        // Reset the live in-sweep readout state too, so a held level / sweep-active hold / live text from this
        // run can't bleed into the idle line after Stop (matches the Start-path reset).
        liveHeldLDb_ = liveHeldRDb_ = -120.0f;
        sweepActiveTicks_ = 0; sweepActiveReleaseTicks_ = 0; liveTextTick_ = 0; liveWasActive_ = false;
        liveHeldPrimary_.clear();
        logLine (eb::DiagnosticLog::Level::Info, "Stop: measurement stopped by the user.");
        // #3 heal: if a FIR-affecting change landed MID-RUN, its build continuation no-oped on the reconfig
        // gate before recording builtGenId_, and nothing re-posts - calibrationApplied() would stay false
        // forever ("Preparing calibration..." with Start dead). Now that we are stopped, re-post the build.
        if (engine.requestedGeneration() != engine.builtGeneration())
            rebuildFirsAsync();
    } else {
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Start clicked");
        // #3 TOCTOU re-check AT CLICK TIME: a rate/FIR-length/complex-phase change bumps the cal generation
        // asynchronously and the button's enabled-state may be stale - never start a run on a superseded or
        // half-built calibration. Refuse, resync the gate, and let the user re-click once it's ready.
        if (const auto gate = computeStartGate(); ! gate.ready) {
            logLine (eb::DiagnosticLog::Level::Warn, "Start refused: gate not ready at click time (stale button state)");
            updateStartGate();
            return;
        }
        gradeInFlight_.store (false);   // begin every run ungated even if the prior run ended via device-loss (no Stop). #3
        gradeRunGen_.fetch_add (1, std::memory_order_relaxed);   // new run generation: a prior run's late grade job won't publish (#4)
        if (verifyTicks > 0) {   // a pending L/R check holds the capture device; clear its GUI state
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setText ({}, juce::dontSendNotification);
        }
        juce::String err;
        if (engine.start (err)) {
            // (P3 Task 4: the transport flips to the Stop face in syncTransport, via updateStartGate below.)
            // #10: record WHICH clock path this run took (macOS aggregate engaged vs the two-clock ASRC
            // fallback) so an on-device validation can't test the wrong path unknowingly. Empty on Windows.
            if (engine.aggregateNote().isNotEmpty())
                logLine (eb::DiagnosticLog::Level::Info, "Clock path: " + engine.aggregateNote());
            inputClipHold_ = 0; silentTicks_ = 0; lowLevelTicks_ = 0; lowSnrTicks_ = 0; statusErrorMsg_.clear();   // no prior-run state bleed
            // P3 Task 6: a fresh run re-arms the capture cards - the sticky Failed card (§3.1) and the
            // capture tracking both re-scope to THIS run.
            failedCaptureEar_ = -1; captureEar_ = -1; captureTicks_ = 0;
            // Live in-sweep readout: start a fresh run at the silent floor with the sweep-active attack/release
            // debounce, the text cadence, and the held live-line text reset, so a prior run's held level /
            // sweep-active state / live text can't bleed in.
            liveHeldLDb_ = liveHeldRDb_ = -120.0f;
            sweepActiveTicks_ = 0; sweepActiveReleaseTicks_ = 0; liveTextTick_ = 0; liveWasActive_ = false;
            liveHeldPrimary_.clear();
            // Task 4 match-poll debounce: a fresh run starts un-matched and un-graded, so the first sustained
            // match (two consecutive matched polls) grades exactly once.
            gradePollTick_ = 0; gradePollerL_.reset(); gradePollerR_.reset(); lastListenTextLogged_.clear();
            maxOutputRenderPeak_ = 0.0f; hwMicRunPeak_ = 0.0f; hwOutputReadable_ = false; hwDetectTick_ = 0;   // fresh hardware-Dirac auto-detect
            smootherL_.reset(); smootherR_.reset();   // fresh anti-flicker band history each run (the
                                                      // held smoothedBands describe the LAST verdict -
                                                      // evidence, so they survive; §3.2 never deletes)
            // Surface a silent format downgrade: WASAPI shared mode can grant a different rate/depth
            // than the user selected, which would otherwise resample with no indication. The split:
            // genuine cautions (a real resample, an unverifiable rail) go on preflightLabel (yellow);
            // the 32-bit-float fact is NORMAL, so it goes on the neutral preflightInfo line (#8).
            auto notes = eb::buildStartNotes (engine.grantedSampleRate(), settings.sampleRate(),
                                              engine.grantedOutputBitDepth(), settings.outputBitDepth(),
                                              engine.rawRail());
            // #66: warn up front when the loaded reference was learned at a DIFFERENT rate than this session -
            // every grade would honestly read "re-learn", but the user should see WHY before sweeping.
            if (engine.referenceLoaded() && std::abs (loadedReferenceRateL_ - activeRate()) > 1.0)
                notes.warnings.add ("Reference was learned at " + juce::String (loadedReferenceRateL_ / 1000.0, 1)
                                  + " kHz; this session is " + juce::String (activeRate() / 1000.0, 1)
                                  + " kHz" + kDash + "re-learn for this rate.");
            // #34: warn when Dirac's output DEVICE differs from the one the reference was learned
            // against - a swap silently changes the comparison basis (the grades would read "re-learn"
            // with no visible reason). Advisory only; legacy sidecars (no binding) stay silent.
            if (engine.referenceLoaded() && loadedReferenceEndpoint_.isNotEmpty()) {
                if (const auto curName = eb::readDiracOutputDeviceName(); curName.isNotEmpty()) {
                    auto curId = eb::endpointUidForName (curName, /*isInput*/ false);
                    if (curId.isEmpty()) curId = curName;
                    if (curId != loadedReferenceEndpoint_ && curName != loadedReferenceEndpoint_)
                        notes.warnings.add ("Reference was learned against a different Dirac output device "
                                            "- re-learn if measurements read 're-learn'.");
                }
            }
            preflightLabel.setText (notes.warnings.joinIntoString (" "), juce::dontSendNotification);
            preflightInfo.setText (notes.info, juce::dontSendNotification);
            resized();   // the info line claims a row only when non-empty -> relayout the rail
            logLine (eb::DiagnosticLog::Level::Info, "Start: measurement started.");
            if (notes.warnings.size() > 0)
                logLine (eb::DiagnosticLog::Level::Warn, "Start notes: " + notes.warnings.joinIntoString (" "));
            logDeviceSnapshot ("start");   // capture the GRANTED rate/depth + raw-rail now the devices are open
        } else {
            preflightLabel.setText ("Start failed: " + err, juce::dontSendNotification);
            preflightInfo.setText ({}, juce::dontSendNotification);
            resized();
            logLine (eb::DiagnosticLog::Level::Error, "Start FAILED: " + err);
        }
    }
    updateStartGate();
}

MainComponent::GateSnapshot MainComponent::computeStartGate() const {
    GateSnapshot g;
    g.haveDevs = inputPicker.selectedDevice().has_value()
              && outputPicker.selectedDevice().has_value();
    // Start requires a VALID, fully-built, atomically-applied generation (requested==built==applied,
    // valid) -- not merely two parsed files. This is the stale/incomplete/invalid guard (P0-02/P0-07).
    g.haveCals = engine.calibrationApplied();
    // D7/R17: with real EARS + virtual cable, block non-AutoPerEar so the user can't record a
    // summed/single-ear signal into Dirac.
    g.wrongMode = isRealEarsWithCable() && settings.combineMode() != CombineMode::AutoPerEar;
    // P1-09: a real EARS into an output that is NOT a verified virtual sink would record into a
    // device Dirac never sees. Block Start until the user picks the virtual audio cable.
    g.physicalOutput = isRealEarsInput()
                    && outputPicker.selectedDevice().has_value()
                    && ! outputPicker.selectedDevice()->isVirtualSink;
    // No cal file loaded for EITHER ear -> the engine runs a neutral unity passthrough (clearLeftCalFir/
    // clearRightCalFir restore unity), a valid-but-uncalibrated state. Allow Start (with a warning) rather
    // than gating on it; a cal that IS loaded but not yet validly applied (half-built) still blocks via haveCals.
    g.noCalsLoaded = settings.leftCalPath().isEmpty() && settings.rightCalPath().isEmpty();
    // #3: the advanced override relaxes ONLY the two policy gates (wrongMode, physicalOutput) for non-Dirac
    // use cases. It NEVER bypasses haveDevs. The cal requirement is met by a valid applied cal OR no cal at all.
    g.ready = eb::startReady (g.haveDevs, g.haveCals, g.wrongMode, g.physicalOutput,
                              settings.advancedOverride(), g.noCalsLoaded);
    return g;
}

// §3.3 + §2 "Measure again" (P3 Task 7): with verdicts showing, the ONE Measure action re-arms -
// the VIEW returns to the instruction/wait state; a stopped engine is started through the SAME
// gate-checked path. Done-ness stays computed - the spine is untouched. Never auto-advances,
// never claims to run a sweep (Dirac owns the sweep).
void MainComponent::onMeasureTransport() {
    if (measureVerdictShowing()) {
        userRearmed_ = true;
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Measure again (view re-armed)");
        if (engine.status() != EngineStatus::Running) { onStartStop(); return; }   // gate-checked start
        refreshWizardView();
        return;
    }
    onStartStop();
}

bool MainComponent::measureVerdictShowing() const {
    const bool anyGraded = eb::earIsGraded (engine.refMonState (0)) || eb::earIsGraded (engine.refMonState (1));
    return anyGraded && ! userRearmed_;
}

void MainComponent::syncTransport (bool running, bool gateReady) {
    // §3.3: ONE engine action (onStartStop), per-stage button faces. Text+glyph+primary flip together
    // (P2.9 rule); repaint only on a real change. `running`/`gateReady` are passed IN by updateStartGate
    // (engine truth + its already-computed gate snapshot - no recompute); the Level transport's
    // enabled/helpText stay owned by updateStartGate, the Measure CTA mirrors them here (M1 parity).
    const auto face = [] (juce::TextButton& b, const juce::String& text, const char* glyph, bool primary) {
        bool changed = false;
        if (b.getButtonText() != text)                        { b.setButtonText (text); changed = true; }
        if (b.getProperties()["glyph"].toString() != glyph)   { b.getProperties().set ("glyph", glyph); changed = true; }
        if ((bool) b.getProperties()["primary"] != primary)   { b.getProperties().set ("primary", primary); changed = true; }
        if (changed) b.repaint();
    };
    face (startStop, running ? "Stop" : "Start monitoring", running ? "stop" : "play", ! running);
    // The Measure face is three-state (P3 Task 7): verdicts showing -> primary "Measure again"
    // (refresh glyph) even while Running - the frozen single-slot resolution (the frames' verdict
    // frame shows exactly ONE primary action; Stop stays available on Level).
    const bool again = measureVerdictShowing();
    face (measureStage_.transportButton(),
          again ? "Measure again" : running ? "Stop" : "Start listening",
          again ? "refresh"       : running ? "stop" : "play",
          again || ! running);
    measureStage_.transportButton().setEnabled (again || running || gateReady);
    measureStage_.transportButton().setHelpText (startStop.getHelpText());   // M1 parity
}

void MainComponent::updateStartGate() {
    const bool running  = engine.status() == EngineStatus::Running;
    const auto gate     = computeStartGate();
    const bool haveDevs = gate.haveDevs, haveCals = gate.haveCals, wrongMode = gate.wrongMode,
               physicalOutput = gate.physicalOutput, noCalsLoaded = gate.noCalsLoaded, ready = gate.ready;
    startStop.setEnabled (running || ready);
    // M1 (HIG, spec §10): surface WHY Start is unavailable as the button's accessible help text — the same
    // first-unmet reason the wizard machine emits for Connect/Calibrate, so a screen-reader user hears it.
    // Empty when ready/running (nothing to explain). Additive: no gate/behavioral change.
    {
        juce::String reason;
        if (! (running || ready)) {
            // minor-4: reuse the WizardState machine reason strings (single source) + a trailing period so
            // the same first-unmet phrasing reaches the screen reader here and the spine. Every way `ready`
            // can be false with the four flags is covered by one of these branches (startReady's truth
            // table), so the former "Finish the setup steps to start." else was unreachable — removed.
            if (! haveDevs)                        reason = eb::kReasonNoDevices()   + ".";
            else if (! haveCals && ! noCalsLoaded) reason = eb::kReasonNoCals()      + ".";
            else if (wrongMode)                    reason = eb::kReasonWrongMode()   + ".";
            else if (physicalOutput)               reason = eb::kReasonPhysicalOut() + ".";
        }
        if (reason != startStop.getHelpText())
            startStop.setHelpText (reason);
    }
    // §3.3: the transport faces follow engine truth - the ONE face writer. AFTER the helpText block so
    // the Measure CTA's helpText mirror inside reads the fresh value (P3 Task 5 signature amendment).
    syncTransport (running, ready);
    // Debug, on-change only (updateStartGate runs at 30 Hz): record WHETHER the gate is enabled and, when
    // disabled, the exact reason from the locals above — this is what explains a greyed-out Start button.
    {
        const int gateState = (running || ready) ? 1 : 0;
        if (gateState != lastStartGateLogged_) {
            lastStartGateLogged_ = gateState;
            if (gateState == 1) {
                logLine (eb::DiagnosticLog::Level::Debug, "Start gate: enabled");
            } else {
                logLine (eb::DiagnosticLog::Level::Debug,
                         juce::String ("Start gate: disabled (haveDevs=") + (haveDevs ? "1" : "0")
                       + " haveCals="       + (haveCals ? "1" : "0")
                       + " wrongMode="      + (wrongMode ? "1" : "0")
                       + " physicalOutput=" + (physicalOutput ? "1" : "0")
                       + " noCalsLoaded="   + (noCalsLoaded ? "1" : "0")
                       + " override="       + (settings.advancedOverride() ? "1" : "0") + ")");
            }
        }
    }
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateCalProblems();
    updateControlsEnabled();
    updateDiracMicGainHint();   // a cal load/clear changed the auto-headroom -> refresh the Mic-gain number
    updateStatusLine();
    refreshWizardView();        // P1: recompute the spine + resolve the active stage from live truth
}

void MainComponent::updateDiracMicGainHint() {
    // EARS Bridge attenuates its own output by the auto makeup-headroom (see ProcessingGraph::recomputeHeadroom).
    // Surface that number so the user adds about the same positive Mic gain in Dirac and Dirac records at a
    // healthy level. The number changes only when a cal loads, so this is cheap to call from updateStartGate.
    const float n = engine.headroomAttenuationDb();   // >= 0 dB; 0 when no attenuation (unity / cut / flat cal)
    juce::String text = (n >= 0.5f)
        ? "EARS Bridge attenuates its output ~" + juce::String (juce::roundToInt (n))
          + " dB for headroom" + kDash + "add about +" + juce::String (juce::roundToInt (n))
          + " dB on Dirac's Mic gain (watch Dirac's input meter)."
        : "Set Dirac's Mic gain so Dirac records at a healthy level (watch Dirac's input meter).";
    if (text != diracMicGainHint.getText())
        diracMicGainHint.setText (text, juce::dontSendNotification);
}

void MainComponent::computeCalProblems (juce::String& leftProblem, juce::String& rightProblem) const {
    // Surface a wrong cal file LOUDLY on the offending card so a left/right swap is impossible to miss
    // (the status line buries it under device/level warnings). Two independent sources:
    //   1. PER-SLOT single-file side check -- fires with just ONE slot loaded, the moment a file's
    //      detected side (CalFile::side, from filename + content) contradicts the slot it's in. This
    //      is what catches "R_HPN in the LEFT slot" before the second file is ever loaded.
    //   2. PAIR diagnostic (serial mismatch / bad type / a swap the validator catches) -- only
    //      meaningful once BOTH files are loaded and the current build has caught up (a stale
    //      diagnostic from the PREVIOUS generation must not flash).
    // Pure computation (no side effects) so BOTH updateCalProblems() (sets the card text) and
    // anyCalProblem() (feeds the wizard snapshot) resolve a rejected cal identically — #M3: the
    // snapshot previously covered ONLY branch (2), so a single swapped file (branch 1) never lit the
    // Calibrate step Error.
    leftProblem.clear();
    rightProblem.clear();

    // (1) Per-slot side check (independent of the other slot).
    if (leftCal.hasCal()
        && eb::calSideMismatched (true, leftCal.calFile()->side, eb::CalSide::Left))
        leftProblem = "This looks like the RIGHT cal, but it's in the LEFT slot" + kDash + "swap the files.";
    if (rightCal.hasCal()
        && eb::calSideMismatched (true, rightCal.calFile()->side, eb::CalSide::Right))
        rightProblem = "This looks like the LEFT cal, but it's in the RIGHT slot" + kDash + "swap the files.";

    // (2) Pair diagnostic (only when both loaded + the build is current + the pair was rejected).
    const bool building = engine.requestedGeneration() != engine.builtGeneration();
    const auto diag     = building ? juce::String() : engine.calibrationDiagnostic();
    const bool pairReject = leftCal.hasCal() && rightCal.hasCal()
                         && ! engine.calibrationApplied() && diag.isNotEmpty();
    if (pairReject) {
        const bool leftSlotSwapped  = diag.containsIgnoreCase ("Left calibration slot holds");
        const bool rightSlotSwapped = diag.containsIgnoreCase ("Right calibration slot holds");
        if (leftSlotSwapped || rightSlotSwapped) {
            // A swap the validator named -- but the per-slot check above usually already set it.
            if (leftSlotSwapped && leftProblem.isEmpty())
                leftProblem = "This looks like the RIGHT cal, but it's in the LEFT slot" + kDash + "swap the files.";
            if (rightSlotSwapped && rightProblem.isEmpty())
                rightProblem = "This looks like the LEFT cal, but it's in the RIGHT slot" + kDash + "swap the files.";
        } else {
            // Serial mismatch / HEQ / unknown-type: not ear-specific. Show on both, but never clobber
            // a more specific per-slot swap message.
            if (leftProblem.isEmpty())  leftProblem  = diag;
            if (rightProblem.isEmpty()) rightProblem = diag;
        }
    }
}

bool MainComponent::anyCalProblem() const {
    juce::String l, r;
    computeCalProblems (l, r);
    return l.isNotEmpty() || r.isNotEmpty();
}

void MainComponent::updateCalProblems() {
    juce::String leftProblem, rightProblem;
    computeCalProblems (leftProblem, rightProblem);
    leftCal.setProblem (leftProblem);
    rightCal.setProblem (rightProblem);
}

void MainComponent::updateControlsEnabled() {
    // Defensive GUI freeze: while capturing, a cal/rate/mode/FIR change must not race the running
    // engine. These controls are DISABLED while Running so their handlers cannot fire at all (a disabled
    // JUCE control emits no onChange/onClick) -- that, not a per-handler Running guard, is what makes them
    // unreachable mid-capture; the engine's reconfigAllowed() no-op (Task 6) backstops the FIR-pair path.
    const bool frozen = engine.status() == EngineStatus::Running;
    leftCal.setEnabled            (! frozen);   // greys the slot (Replace/Remove) — no dedicated per-button API
    rightCal.setEnabled           (! frozen);
    combineBox.setEnabled         (! frozen);
    rateBox.setEnabled            (! frozen);
    bitBox.setEnabled             (! frozen);
    firLenBox.setEnabled          (! frozen);
    complexPhaseToggle.setEnabled (! frozen);
    trimSlider.setEnabled         (! frozen);
    // #6: the device pickers were the ONLY config controls left live while Running - a mid-run pick committed
    // to the combo while the handler early-returned, so the UI named device B while the engine measured device
    // A (and every gate/status read evaluated B), persisting after Stop. Freeze them like everything else.
    inputPicker.setEnabled        (! frozen);
    outputPicker.setEnabled       (! frozen);
    // #22: the learn enable-state is computed HERE, in one place, from ALL of its inputs. The old unconditional
    // re-enable clobbered the hardware-Dirac disable and the cancel-window disable on the next gate refresh.
    const bool cancelPending = learning_ && learnCancelRequested_.load (std::memory_order_relaxed);
    learnRefButton.setEnabled (! frozen && ! settings.diracHardwareProcessor() && ! cancelPending);
}

void MainComponent::syncPlotScales() {
    // Both ear curves share one dB axis so they're directly comparable.
    float top = 6.0f;
    if (auto l = leftCal.calFile())  top = std::max (top, fitTopDb (*l));
    if (auto r = rightCal.calFile()) top = std::max (top, fitTopDb (*r));
    leftCal.setPlotRange (top);
    rightCal.setPlotRange (top);
}

// ---- Wizard view layer (P1 Task 3 cutover) -----------------------------------------------------

eb::WizardInputs MainComponent::snapshotWizardInputs() const {
    eb::WizardInputs in;
    const auto gate = computeStartGate();
    in.haveDevs       = gate.haveDevs;
    in.haveCals       = gate.haveCals;
    in.wrongMode      = gate.wrongMode;
    in.physicalOutput = gate.physicalOutput;
    in.noCalsLoaded   = gate.noCalsLoaded;
    in.gateReady      = gate.ready;
    in.deviceError    = statusErrorMsg_.isNotEmpty();
    // calBuilding: async FIR generation in flight — the SAME expression updateCalProblems() uses.
    in.calBuilding    = engine.requestedGeneration() != engine.builtGeneration();
    // calProblem: any rejected cal the card would surface. #M3: this must mirror BOTH branches of
    // updateCalProblems() — the PER-SLOT side-mismatch (one swapped file) as well as the PAIR
    // diagnostic — so a single swapped file lights the Calibrate step Error, not just a rejected pair.
    // anyCalProblem() is the shared computation; its pair branch is already build-generation-guarded
    // (a stale diagnostic from the prior generation reads empty), matching the old calBuilding mask.
    in.calProblem     = anyCalProblem();
    // §5.2: the unity choice only ever applies while noCalsLoaded holds - a loaded cal masks
    // it even before the reset in onXCalLoaded lands (belt + braces; done-ness stays computed).
    in.unityAccepted  = unityAcceptedSession_ && gate.noCalsLoaded;
    in.engineRunning  = engine.status() == EngineStatus::Running;
    in.levelLatched   = levelLatched_;   // Task 4: set once L+R reached the green band this session (timerCallback)
    in.referenceLoaded = ! loadedReferenceL_.empty() && ! loadedReferenceR_.empty();
    in.hwDirac        = settings.diracHardwareProcessor();
    in.overrideOn     = settings.advancedOverride();
    in.configGen      = calGenCounter_.load (std::memory_order_relaxed);
    in.verdictGenL    = verdictGenL_;   // stamped at publish (stampVerdictGeneration); -1 = never
    in.verdictGenR    = verdictGenR_;
    // earGraded from the per-ear published grade state (the same states the quality dots are read from).
    const auto gradedState = [] (int s) {
        const auto rs = (eb::RefMonState) s;
        return rs == eb::RefMonState::GradedClean || rs == eb::RefMonState::GradedMarginal
            || rs == eb::RefMonState::GradedSuspect;
    };
    in.earGradedL = gradedState (engine.refMonState (0));
    in.earGradedR = gradedState (engine.refMonState (1));
    return in;
}

void MainComponent::stampVerdictGeneration (int ear) {
    (ear == 1 ? verdictGenR_ : verdictGenL_) = calGenCounter_.load (std::memory_order_relaxed);
}

// The guarded HEAD of the live publish continuation, extracted so a test can pin the ORDER (P3 Task 1
// review Fix 4a): the run-generation guard runs FIRST, and a STALE generation publishes nothing and -
// critically - stamps nothing. A stamp under a stale run would refresh verdictGen to the CURRENT
// configGen and falsely clear verdictsStale with evidence from a dead run (measurement honesty).
bool MainComponent::publishGradeIfRunCurrent (int ear, int state, float irSnrDb, float thdPercent,
                                              bool mismatch, bool lowQuality, uint32_t runGen) {
    if (gradeRunGen_.load (std::memory_order_relaxed) != runGen)
        return false;   // a Stop/Start happened since this job was posted -> stale: drop it whole (#4)
    // Publish THIS ear's verdict snapshot (the SNR lesson: trio published together); raise the guidance
    // flag matching the verdict. NEITHER flag invalidates the capture (they are guidance only).
    engine.publishReferenceGrade (ear, state, irSnrDb, thdPercent, mismatch, lowQuality);
    stampVerdictGeneration (ear);   // §3.2: stamp the generation this verdict was measured under
    return true;
}

void MainComponent::publishGradeForTest (int ear, int refMonState, float sweepSnrDb) {
    engine.publishReferenceGrade (ear, refMonState, 50.0f, 0.5f, false, false);
    if (sweepSnrDb > 0.0f)
        engine.publishCompletedSweepSnrDb (ear, sweepSnrDb);   // the live continuation's sibling publish
    stampVerdictGeneration (ear);
    {   // Mirror the live continuation's band store (P3 Task 7): the card's badge is band-driven.
        auto& sm = ear == 1 ? smootherR_ : smootherL_;
        (ear == 1 ? smoothedBandsR_ : smoothedBandsL_) = sm.update (sweepSnrDb, sweepSnrDb > 0.0f, 50.0f, 0.5f);
    }
    userRearmed_ = false;   // new evidence arrived: the verdict view returns (mirrors the live publish)
    updateStartGate();   // the full refresh path (gate + status + wizard view) - what a live publish reaches
}

void MainComponent::refreshWizardView() {
    fmtCluster_.setParts (formatClusterParts (settings.sampleRate(), settings.outputBitDepth(),
                                              settings.combineMode()));
    renderWizardView (computeWizardState (snapshotWizardInputs(), pinnedStep_));
}

void MainComponent::renderWizardView (const eb::WizardState& ws) {
    // View-owned per-step DONE summaries (§ spine meta override). Connect carries the device pair when both
    // devices are chosen; Calibrate the loaded-pair "<type> pair · <serial>"; Level the green-band state.
    juce::String viewMetas[kWizardStepCount];
    if (auto in = inputPicker.selectedDevice())
        if (auto out = outputPicker.selectedDevice())
            viewMetas[(int) WizardStep::Connect] = distillDeviceName (in->name) + kArrow
                                                 + distillDeviceName (out->name);
    // Calibrate done-summary "HEQ pair · <serial>" (spec § view metas). Only when the step is Done (both
    // cals loaded + applied) — else the machine reason (Todo/Error/rebuilding) stays.
    if (ws.steps[(int) WizardStep::Calibrate].state == StepState::Done) {
        // §5.2 unity: Calibrate can be Done with NO cals (explicit continue-without-cal). Stamp the
        // honest meta - guarded on both-empty so it never collides with the pair summary below.
        if (! leftCal.hasCal() && ! rightCal.hasCal())
            viewMetas[(int) WizardStep::Calibrate] = "No calibration (unity)";
        if (auto lc = leftCal.calFile()) {
            const juce::String typeName = calTypeShortName (lc->type);
            const juce::String serial   = lc->serial.isNotEmpty() ? lc->serial
                                        : (rightCal.calFile() ? rightCal.calFile()->serial : juce::String());
            viewMetas[(int) WizardStep::Calibrate] = serial.isNotEmpty()
                ? (typeName + " pair" + kDash + serial) : (typeName + " pair");
        }
    }
    // Level done-summary "In green band" (spec § view metas). Only meaningful once Done (latched).
    if (ws.steps[(int) WizardStep::Level].state == StepState::Done)
        viewMetas[(int) WizardStep::Level] = "In green band";
    // Level and Measure share the SAME machine reason while both Blocked ("Finish Connect and Calibrate
    // first"), which renders two identical spine metas (a duplicate finding + it reads as a copy-paste bug).
    // Give the terminal Measure step a distinct, honest blocked summary so the two rows never collide.
    if (ws.steps[(int) WizardStep::Measure].state == StepState::Blocked
        && ws.steps[(int) WizardStep::Level].state == StepState::Blocked)
        viewMetas[(int) WizardStep::Measure] = "Available after Level";

    // Reference footer (§5.5): line1 = the reference status, line2 = what it was learned from.
    juce::String refLine1, refLine2;
    if (settings.diracHardwareProcessor()) {
        refLine1 = "n/a (hardware Dirac)";
    } else if (! loadedReferenceL_.empty() && ! loadedReferenceR_.empty()) {
        refLine1 = "learned";
        refLine2 = loadedReferenceEndpoint_.isNotEmpty() ? distillDeviceName (loadedReferenceEndpoint_)
                                                         : juce::String ("Windows Audio");
    } else {
        refLine1 = "not learned";
    }

    // The spine ALWAYS renders the machine's truth (which step regressed, is rebuilding, etc.).
    spine_.setState (ws, viewMetas, refLine1, refLine2);

    // ---- Which stage to SHOW (view decision, distinct from ws.active) ----------------------------------
    // Normally the view shows ws.active. The one exception is a TRANSIENT-only pin illegality (the recorded
    // Task-3 carryover) — resolveShownStage() decides, taking the state recomputed with calBuilding masked
    // out so the hold is honest (it holds a pin that is legal-but-for-the-rebuild, never a genuinely-blocked
    // one). Re-snapshot + mask here rather than trusting a synthetic ws, so the LIVE path reads live truth.
    auto maskedIn = snapshotWizardInputs();
    maskedIn.calBuilding = false;
    const WizardState masked = computeWizardState (maskedIn, pinnedStep_);
    const WizardStep toShow = resolveShownStage (ws, pinnedStep_, masked);

    // ---- CTA wiring: Connect/Calibrate Continue enabled iff THAT step is Done; Level is the §3.2
    // SOFT gate (P3): its Continue is NAVIGATION to Measure - only Blocked forbids it. The un-latched
    // caution rides the run-note; the SNR grade is the real enforcement.
    connectStage_.continueButton().setEnabled  (ws.steps[(int) WizardStep::Connect].state   == StepState::Done);
    calibrateStage_.continueButton().setEnabled (ws.steps[(int) WizardStep::Calibrate].state == StepState::Done);
    levelStage_.continueButton().setEnabled (ws.steps[(int) WizardStep::Measure].state != StepState::Blocked);
    levelStage_.setRunNote (LevelStage::levelRunNote (
        ws.steps[(int) WizardStep::Level].state == StepState::Blocked,
        levelLatched_, ws.steps[(int) WizardStep::Level].reason));
    // P3 Task 5+7: the Measure run-note (pure copy rule; the honesty contract in words - "Arms the
    // bridge", never "starts the sweep"; measure-again = "Then start the measurement again in Dirac
    // Live." - the correction rides the note, the button never claims to run a sweep).
    measureStage_.setRunNote (MeasureStage::measureRunNote (
        ws.steps[(int) WizardStep::Measure].state == StepState::Blocked,
        ws.steps[(int) WizardStep::Measure].reason,
        engine.status() == EngineStatus::Running, measureVerdictShowing()));

    // ---- P2 stage view feed (Connect's line lands with Task 7) ---------------------------------
    // Run-note = the machine's own reason (single wording source; empty once Done) - this is the
    // third home of the "Rebuilding filters..." building state (map #9).
    const auto runNoteFor = [&ws] (WizardStep s) {
        const auto& st = ws.steps[(int) s];
        return st.state == StepState::Done ? juce::String() : st.reason;
    };
    connectStage_.setRunNote   (runNoteFor (WizardStep::Connect));
    calibrateStage_.setRunNote (runNoteFor (WizardStep::Calibrate));
    const auto slotType = [] (const std::optional<eb::CalFile>& c) {
        return c.has_value() ? std::optional<eb::CalType> (c->type) : std::nullopt;
    };
    calibrateStage_.setStageCaption (CalibrateStage::stageCaptionFor (slotType (leftCal.calFile()),
                                                                      slotType (rightCal.calFile())));
    calibrateStage_.setAdvancedSummary (CalibrateStage::advancedFirSummary (
        settings.complexPhase(), settings.firLength(), settings.outputTrimDb()));
    // §5.2 unity path: the hint + button live at stage level and show only while BOTH slots are empty;
    // the accepted wording (button retired) rides `unityAcceptedSession_ && noCalsLoaded` (masked once a
    // cal loads, exactly like the snapshot input above).
    const bool noCalsLoaded = settings.leftCalPath().isEmpty() && settings.rightCalPath().isEmpty();
    calibrateStage_.setUnityState (noCalsLoaded, unityAcceptedSession_ && noCalsLoaded);
    leftCal.setSiblingLoaded  (rightCal.hasCal());   // map #8b/#10: the honest Required line
    rightCal.setSiblingLoaded (leftCal.hasCal());

    // ---- Regression banner (§3.1): render + wire the "Fix in <step>" jump --------------------------------
    // Compose the banner against the SHOWN step (`toShow`), not ws.active (minor-1): during a held-pin
    // rebuild the shown stage can be EARLIER than active, and a deviceError there must still surface a
    // banner + jump on the held stage. composeBanner re-runs the machine's rule against toShow.
    const auto shownBanner = eb::composeBanner (ws, toShow);
    stageHost_.setBanner (shownBanner.text,
                          shownBanner.text.isNotEmpty() ? stepName (shownBanner.target) : juce::String(),
                          [this, target = shownBanner.target] { pinnedStep_ = target; refreshWizardView(); });

    // ---- Show the stage; on a REAL switch place focus + post the a11y announcement + refresh the title --
    const bool switching = (toShow != shownStage_) || (stageHost_.shown() != toShow);
    stageHost_.showStage (toShow);
    if (switching) {
        shownStage_ = toShow;
        // The stage-area landmark is the visible stage's OWN title (its ctor set the step name); no host
        // title mirror (that would double-announce + trip the gate's duplicate-label check).
        // isShowing() guard: JUCE asserts (jassert isShowing(), juce_Component.cpp:2694) on a grab before
        // the component is on screen (headless tests / mid-construction). Release strips the jassert; debug
        // builds abort. Only grab when the target is actually showing.
        if (auto* f = firstFocusTarget (toShow); f != nullptr && f->isShowing()) f->grabKeyboardFocus();
        postStageAnnouncement (toShow);
    }

    // P3 Task 5: refresh the Measure view LAST, so both the live path (refreshWizardView) and the
    // forced/test path (forceWizardStepForTest) feed it from the same resolved state.
    refreshMeasureView (ws);
}

void MainComponent::refreshMeasureView (const eb::WizardState& ws) {
    const bool running  = engine.status() == EngineStatus::Running;
    const bool refLoaded = ! loadedReferenceL_.empty() && ! loadedReferenceR_.empty();
    using Lead = MeasureStage::Lead;
    const Lead lead = settings.diracHardwareProcessor() ? Lead::HwDirac
                    : (! refLoaded ? Lead::Reference : Lead::Waiting);
    measureStage_.setLead (lead);
    const bool showVerdicts = measureVerdictShowing();
    const auto head = MeasureStage::measureHeadCopy (lead, settings.advancedOverride(), showVerdicts,
                                                     settings.combineMode());   // P3 T7 ruling: mode-aware head
    measureStage_.setHeadCopy (head.title, head.sub);
    measureStage_.setWaitHint (running && lead == Lead::Waiting
        ? MeasureStage::waitingHint (armedNoSweepTicks_ / 30, refEndpointMismatch_,
                                     (chainVerdict_.checked && ! chainVerdict_.all48k) || chainVerdict_.unverifiable,
                                     chainVerdict_.summary, silentTicks_ >= kSilentHoldTicks)
        : juce::String());
    // P3 Task 7: compose + feed the verdict cards (before the capture feed - a graded ear's slot
    // flips CaptureCard -> VerdictCard in the stage's grid). ws.verdictsStale is the §3.2 staleness
    // truth (verdictGen < configGen, computed - never a stored flag); the bands are the smoothed
    // per-ear stores the grade publish fills (anti-flicker preserved from the retired dots).
    const bool hw = settings.diracHardwareProcessor();
    const auto vModel = [&] (int ear, const char* name) {
        const auto& bands = ear == 1 ? smoothedBandsR_ : smoothedBandsL_;
        return eb::verdictCardModel (name, earGradeSnapshot (ear),
                                     bands.sweepSnr, bands.irSnr, bands.thd,
                                     engine.shapeFlags (ear), shapeScalars (ear),
                                     ws.verdictsStale, hw);
    };
    measureStage_.setVerdictModels (showVerdicts ? vModel (0, "LEFT EAR") : eb::VerdictCardModel{},
                                    showVerdicts ? vModel (1, "RIGHT EAR") : eb::VerdictCardModel{},
                                    ws.verdictsStale);
    // P3 Task 6: compose + feed the capture grid (composeCaptureModel: Failed wins the §3.1 sticky
    // card; Capturing carries HONEST learned-duration progress; else Waiting).
    measureStage_.setCaptureModels (composeCaptureModel (0, "LEFT EAR", running),
                                    composeCaptureModel (1, "RIGHT EAR", running));
}

// One ear's capture-card composition (P3 Task 6, extracted in Task 7 for the headless seam).
// Failed wins (the §3.1 sticky card names ONE capture); Capturing carries HONEST progress -
// elapsed GUI ticks over the LEARNED per-ear reference duration ("~N s sweep", approximate by
// label). No learned duration -> captureFraction claims 0 and the copy never invents Dirac timing.
eb::CaptureCardModel MainComponent::composeCaptureModel (int ear, const char* name, bool running) const {
    if (failedCaptureEar_ == ear) return eb::CaptureCardModel::failed (name);
    if (captureEar_ == ear && running) {
        const auto& ref   = ear == 1 ? loadedReferenceR_ : loadedReferenceL_;
        const double rate = ear == 1 ? loadedReferenceRateR_ : loadedReferenceRateL_;
        const double secs = (rate > 0.0 && ! ref.empty()) ? (double) ref.size() / rate : 0.0;
        return eb::CaptureCardModel::capturing (name, eb::CaptureCard::captureFraction (captureTicks_, secs),
                                                juce::roundToInt (secs));
    }
    return eb::CaptureCardModel::waiting (name, settings.combineMode());   // P3 T7 ruling: mode-aware sub
}

eb::WizardStep MainComponent::resolveShownStage (const eb::WizardState& ws,
                                                 std::optional<WizardStep> pinned,
                                                 const eb::WizardState& masked) {
    // Hold the pinned step across a TRANSIENT rebuild: it must be Blocked NOW (in ws) but NOT Blocked once
    // calBuilding is masked out (in `masked`). That is exactly "Blocked only because a FIR rebuild is in
    // flight" — a flicker to avoid. Everything else shows ws.active (the machine's first-unmet resolution).
    if (pinned && *pinned != ws.active
        && ws.steps[(int) *pinned].state == StepState::Blocked
        && masked.steps[(int) *pinned].state != StepState::Blocked)
        return *pinned;
    return ws.active;
}

// The first enabled, focusable leaf inside a stage's subtree (top-down DFS). Used for explicit focus
// placement on a stage switch (§4: a hidden component gives focus away, so we place it deterministically).
juce::Component* MainComponent::firstFocusTarget (WizardStep step) {
    juce::Component* stage = nullptr;
    switch (step) {
        case WizardStep::Connect:   stage = &connectStage_;   break;
        case WizardStep::Calibrate: stage = &calibrateStage_; break;
        case WizardStep::Level:     stage = &levelStage_;     break;
        case WizardStep::Measure:   stage = &measureStage_;   break;
    }
    if (stage == nullptr) return nullptr;
    std::function<juce::Component*(juce::Component*)> dfs = [&] (juce::Component* c) -> juce::Component* {
        for (auto* child : c->getChildren()) {
            // Skip a Viewport (minor-3): a juce::Viewport wants keyboard focus by default (to catch scroll
            // keys), so a naive first-focusable DFS would land focus on the scroll container instead of the
            // first real control inside it. Descend PAST it to the actual control.
            if (child->isVisible() && child->isEnabled() && child->getWantsKeyboardFocus()
                && dynamic_cast<juce::Viewport*> (child) == nullptr)
                return child;
            if (auto* deeper = dfs (child)) return deeper;
        }
        return nullptr;
    };
    if (auto* leaf = dfs (stage)) return leaf;
    return stage;   // no focusable child -> the stage itself (a keyboardFocusContainer)
}

void MainComponent::postStageAnnouncement (WizardStep step) {
    // §4 a11y: announce the newly-opened stage ("Level, step 3 of 4"). postAnnouncement is a static that
    // routes to the active accessibility client; with no peer (headless tests) it is a safe no-op.
    const int n = (int) step + 1;
    const juce::String msg = stepName (step) + ", step " + juce::String (n) + " of "
                           + juce::String (kWizardStepCount);
    juce::AccessibilityHandler::postAnnouncement (msg, juce::AccessibilityHandler::AnnouncementPriority::medium);
}

void MainComponent::forceWizardStepForTest (WizardStep step) {
    // Test-only seam — DETERMINISTIC forcing per spec §8.2: the gate must score EVERY stage's
    // hand-laid layout, so this seam FORCES the requested stage to be the visible one regardless of
    // the machine's pin-legality rules (in the hermetic gate env Connect is Todo, so Level/Measure
    // are Blocked and an ordinary pin would be illegal — the machine would fall back to Connect and
    // those stages would never be scored). We render the spine with the state as the machine computes
    // it but with `.active` overridden to `step`, then show `step` unconditionally. The LIVE path
    // (refreshWizardView -> renderWizardView) never bypasses the machine; only this seam does.
    pinnedStep_ = step;
    auto ws = computeWizardState (snapshotWizardInputs(), pinnedStep_);
    ws.active = step;                 // override for the spine render (which stage is "open")
    renderWizardView (ws);            // paints the spine + shows ws.active == step
    stageHost_.showStage (step);      // FORCE the shown stage even if renderWizardView showed another
    resized();
}

bool MainComponent::keyPressed (const juce::KeyPress& key) {
    // Ctrl/Cmd + 1..4 -> jump to that step (§4), but only when it is NAVIGABLE (not Blocked). Pinning a
    // Blocked step is a no-op — the machine would just fall back to first-unmet, so we don't even pin.
    const auto mods = key.getModifiers();
    if (! (mods.isCommandDown() || mods.isCtrlDown()))
        return false;
    for (int i = 0; i < kWizardStepCount; ++i) {
        if (key.isKeyCode ('1' + i)) {
            const auto ws = computeWizardState (snapshotWizardInputs(), pinnedStep_);
            if (ws.steps[i].state == StepState::Blocked)
                return true;                          // consumed but inert (un-navigable)
            pinnedStep_ = (WizardStep) i;
            refreshWizardView();
            return true;
        }
    }
    return false;
}

void MainComponent::forceThemeForTest (bool dark) {
    theme.setDarkForTest (dark);
    applyTextColours();
    // Settle the two labels applyTextColours does NOT own, so the gate scores a STEADY-STATE frame (what the
    // user sees after the next timer tick), not a transient mid-switch frame: levelsHint's colour/text is the
    // timer's (updateActiveEarIndicator), and diracCableHint's is state-driven (updateDiracCableHint - hidden
    // when no virtual output is selected, which is the headless default).
    updateActiveEarIndicator (true);
    updateDiracCableHint();
    sendLookAndFeelChange();   // children re-read LookAndFeel colours (mirrors the live theme tick)
    resized();
}

// #68 re-point (see the header): drive the per-ear VerdictCards for the render gate. ORDER matters
// (the Task-6 harness lesson): forceWizardStepForTest FIRST - its renderWizardView ->
// refreshMeasureView recomposes the verdict feed from LIVE truth (no grades headless -> empty
// models) and would clobber anything driven before it. Then force the Waiting lead (headless has
// no learned reference, so the live lead is Reference and the grid would be hidden) + the verdict
// head copy + the given models + the details state. Display only, no engine/grading state.
void MainComponent::driveVerdictForTest (const eb::VerdictCardModel& l, const eb::VerdictCardModel& r,
                                         bool detailsOpenRight) {
    forceWizardStepForTest (WizardStep::Measure);
    measureStage_.setLead (MeasureStage::Lead::Waiting);
    const auto head = MeasureStage::measureHeadCopy (MeasureStage::Lead::Waiting, false, /*verdictShowing*/ true,
                                                     settings.combineMode());   // hermetic gates: AutoPerEar default
    measureStage_.setHeadCopy (head.title, head.sub);
    measureStage_.setVerdictModels (l, r, l.stale || r.stale);
    measureStage_.verdictCardForTest (1).setDetailsOpen (detailsOpenRight);
}

void MainComponent::driveConnectWarningsForTest (bool stdCableHintWithFix, bool rateResampleWarn) {
    if (stdCableHintWithFix) {
        diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
        diracCableHint.setText (eb::hints::kDiracStdCableExclusive, juce::dontSendNotification);
        diracCableHint.setVisible (true);
        diracFixButton.setVisible (true);
    } else {
        updateDiracCableHint();   // re-derive from the real output. autoSelectDefaults picks a std VB-CABLE
                                  // when one is enumerated, so on a machine WITH a virtual sink the derived
                                  // state is the hint VISIBLE (the green shared-mode variant if Dirac shared
                                  // mode is on, else the amber exclusive variant), NOT hidden. Only a machine
                                  // with no virtual sink at all derives the hidden state.
    }
    rateWarn.setText (rateResampleWarn ? juce::String (eb::hints::kRateResample) : juce::String(),
                      juce::dontSendNotification);
    resized();
}

void MainComponent::driveLevelClipForTest (bool on) {
    // PRODUCTION copy rule: the ctor set inputClipHint's text from kInputClipHintText once; the live
    // clip path (timerCallback) only toggles VISIBILITY. Mirror that mechanism - force the visibility
    // and re-assert the shared constant (never a test-local copy, never cleared: clearing would leave
    // the live path showing an empty warning after a test drive).
    inputClipHint.setText (kInputClipHintText, juce::dontSendNotification);
    inputClipHint.setVisible (on);
    resized();
}

void MainComponent::driveDeviceErrorForTest (const juce::String& msg) {
    statusErrorMsg_ = msg;
    updateStartGate();          // -> updateStatusLine + refreshWizardView render the error/banner truth
}

void MainComponent::applyTextColours() {
    // Re-set every theme-dependent label colour (the Theme statics return the active mode);
    // paint-based components (meters, cards, plots) pick the mode up on repaint.
    // (The format cluster is paint-based - it reads Theme::textDim/textFaint at paint time, nothing to re-apply.)
    versionLabel.setColour (juce::Label::textColourId, Theme::textDim());   // HIG: textFaint was 2.4:1; textDim ~5.3:1
    styleEyebrow (combineLabel,  "COMBINE MODE");
    styleEyebrow (rateLabel,     "RATE");
    styleEyebrow (bitLabel,      "BIT DEPTH");
    styleEyebrow (firLenLabel,   "FIR LENGTH");
    trimLabel.setText ("Output trim", juce::dontSendNotification);          // P2.9: parameter-row label, not an eyebrow
    trimLabel.setColour (juce::Label::textColourId, Theme::textDim());
    trimLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    styleEyebrow (levelsEyebrow, "LEVELS");
    // levelsHint colour/text is owned by the timer (updateActiveEarIndicator), so it is not set here.
    combineHint.setColour      (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setColour    (juce::Label::textColourId, Theme::textDim());
    diracMicGainHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setColour       (juce::Label::textColourId, Theme::textDim());
    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightInfo.setColour  (juce::Label::textColourId, Theme::textDim());
    rateWarn.setColour       (juce::Label::textColourId, Theme::warn());
    inputPicker.applyTheme();
    outputPicker.applyTheme();
    leftCal.applyTheme();
    rightCal.applyTheme();
    // Wizard view layer: re-colour each stage's OWN labels + re-render the spine (setState re-reads Theme)
    // so a live light/dark flip repaints every re-homed surface, not just MainComponent's own labels.
    connectStage_.applyTheme();
    calibrateStage_.applyTheme();
    levelStage_.applyTheme();
    measureStage_.applyTheme();
    updateStatusLine();
    refreshWizardView();
}

void MainComponent::applyTitleBarTheme() {
   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
        if (auto* hwnd = (HWND) peer->getNativeHandle()) {
            BOOL dark = Theme::dark() ? TRUE : FALSE;
            ::DwmSetWindowAttribute (hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof (dark));
        }
   #endif
}

MainComponent::LiveReadout MainComponent::updateLiveInputReadout() {
    LiveReadout out;
    // Live per-channel input peaks from the engine (dBFS, NOT clamped at 0 so a clip reads positive).
    const float liveL = engine.lastInputPeakLDb();
    const float liveR = engine.lastInputPeakRDb();

    // Sweep-active debounce off the LIVE (not held) peak with an ATTACK and a RELEASE (Bug B-2). Computed FIRST
    // so we know when a sweep ENGAGES (to reset the running max for it):
    //   attack  - peakMax must hold above the gate ~0.3 s (kSweepActiveHoldTicks) before the live line engages,
    //             so a momentary blip can't flip the status on;
    //   release - once engaged it STAYS engaged for ~1.5 s (kSweepActiveReleaseTicks) after the level last
    //             cleared the gate. The release countdown bleeds down instead of snapping to 0, so the quiet
    //             L<->R inter-sweep gap and brief sub-gate dips don't flip the live readout off and back.
    const float livePeakMax = juce::jmax (liveL, liveR);
    const bool  aboveGate    = livePeakMax > kLiveActiveGateDb;
    sweepActiveTicks_        = aboveGate ? (sweepActiveTicks_ + 1) : 0;          // attack counter (consecutive above)
    const bool attacked      = sweepActiveTicks_ >= kSweepActiveHoldTicks;       // attack satisfied this tick
    // Re-arm the release hold once attacked-or-still-above; otherwise let it bleed down (pure step, unit-tested).
    sweepActiveReleaseTicks_ = eb::sweepActiveRelease (sweepActiveReleaseTicks_,
                                                       attacked || (aboveGate && sweepActiveReleaseTicks_ > 0),
                                                       kSweepActiveReleaseTicks);
    out.owns = attacked || sweepActiveReleaseTicks_ > 0;   // engaged while attacking-above OR within the hold

    // TRUE PEAK-HOLD over the WHOLE sweep, NOT a fast decay. The earlier ~-12 dB/s decay made the held value
    // jitter DOWN as the chirp swept across the earcup's frequency response, so at a rolled-off moment one
    // channel read low + imbalanced and tripped "(R low - reseat the earcup?)" — even while the sweep PEAK was
    // CLIPPING (a flat contradiction the user hit). Hold the running MAX since this sweep ENGAGED instead: reset
    // on the idle->active edge, accumulate the max while engaged. The readout's level / clip / L-R imbalance is
    // then the stable per-channel PEAK of the whole sweep — what's needed to set the level and spot a REAL
    // imbalance, not the instantaneous frequency-dependent dip.
    if (out.owns && ! liveWasActive_) liveHeldLDb_ = liveHeldRDb_ = -120.0f;   // new sweep -> fresh running max
    if (out.owns) {
        liveHeldLDb_ = juce::jmax (liveHeldLDb_, liveL);
        liveHeldRDb_ = juce::jmax (liveHeldRDb_, liveR);
    }
    liveWasActive_ = out.owns;

    if (! out.owns) {
        // Idle: hand the status back to the per-ear verdicts and render the live line immediately on the next
        // engage (don't wait out the text cadence). The running max is reset on the next idle->active edge.
        liveTextTick_ = 0;
        liveHeldPrimary_.clear();
        return out;
    }

    // Sweep-active: rebuild the readout text from the HELD (steady) peak only on the ~7 Hz cadence tick, but
    // CARRY the last-rendered text/colour out on EVERY engaged tick (Bug B-1). render==true on the first
    // engaged tick (liveTextTick_ resets to 0 when idle above) and every kLiveTextEveryTicks after; on the
    // in-between ticks we re-emit liveHeldPrimary_/liveHeldColour_ so the live line persists through the
    // set-if-changed commit instead of blinking.
    if (liveTextTick_ == 0) {
        const auto s = eb::liveInputStatus (liveHeldLDb_, liveHeldRDb_, true);
        // Map the colour-free severity to a Theme role (semantic colours only; no hardcoded hex). A clip uses
        // the danger role; the WORDS ("CLIPPING" + the dB) carry the meaning without colour.
        liveHeldColour_  = (s.severity == eb::LiveInputSeverity::Clip) ? Theme::danger() : Theme::text();
        liveHeldPrimary_ = s.text;     // single-line peak readout (the live line never owns statusLineR now)
    }
    out.render  = (liveTextTick_ == 0);
    out.primary = liveHeldPrimary_;   // carried EVERY engaged tick so the line persists (no blink)
    out.colour  = liveHeldColour_;
    if (++liveTextTick_ >= kLiveTextEveryTicks) liveTextTick_ = 0;
    return out;
}

// Commit a status label only when its text OR colour changed (Bug B: nothing is cleared-then-reset, so the
// line is persistent and never flickers between render ticks). Compares getText()/findColour first. Tooltip
// is handled separately (setTooltipIfChanged) because most branches leave it empty.
bool MainComponent::setLabelIfChanged (juce::Label& label, const juce::String& text, juce::Colour colour) {
    const bool textChanged   = label.getText() != text;
    const bool colourChanged = label.findColour (juce::Label::textColourId) != colour;
    if (! textChanged && ! colourChanged) return false;
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, colour);
    return true;
}

// Tooltip set-if-changed companion (only the low-SNR / per-ear-warn branches carry one; everything else is "").
static void setTooltipIfChanged (juce::Label& label, const juce::String& tip) {
    if (label.getTooltip() != tip) label.setTooltip (tip);
}

// One ear's published-grade snapshot (P3 Task 7: the exact body of the old updateStatusLine
// earSnap lambda, extracted so the ladder AND the VerdictCard composition read ONE truth).
eb::EarGradeSnapshot MainComponent::earGradeSnapshot (int ear) const {
    eb::EarGradeSnapshot e;
    e.state      = engine.refMonState (ear);
    e.irSnrDb    = engine.refIrSnrDb (ear);
    e.thdPercent = engine.refThdPercent (ear);
    e.sweepSnrDb = engine.refSweepSnrDb (ear);
    e.peakDb     = engine.referenceSweepPeakDb (ear);
    // SP3 INFO note: the worst-offender shape anomaly for this ear (empty when none). Read the
    // published flags + magnitudes lock-free and compose the neutral one-liner. INFO-ONLY.
    const unsigned sf = engine.shapeFlags (ear);   // ONE acquire load; the scalars follow (relaxed)
    e.shapeNote  = eb::shapeInfoNote (sf, engine.shapeDriftMaxDb (ear),
                                      engine.shapeCombDelayMs (ear), engine.shapeEffHiHz (ear),
                                      engine.shapeEffLoHz (ear), engine.shapeHumBaseHz (ear));
    // SP3 INFO tooltip: the FULL numbers behind that note (all active findings, "label: value"
    // lines) so an over-long status line can elide the note and still be discoverable.
    e.shapeTip   = eb::shapeInfoTip (sf, engine.shapeDriftMaxDb (ear), engine.shapeHfShelfDb (ear),
                                     engine.shapeCombDepthDb (ear), engine.shapeCombDelayMs (ear),
                                     engine.shapeEffLoHz (ear), engine.shapeEffHiHz (ear),
                                     engine.shapeLobeWidth (ear), engine.shapeStepDb (ear),
                                     engine.shapeHumBaseHz (ear), engine.shapeResonanceHz (ear));
    return e;
}

// The published shape-scalar bundle for one ear (P3 Task 7: the VerdictCard chips/observations
// read the same atomics the INFO note/tooltip above read).
eb::ShapeScalars MainComponent::shapeScalars (int ear) const {
    eb::ShapeScalars s;
    s.driftMaxDb  = engine.shapeDriftMaxDb (ear);   s.hfShelfDb   = engine.shapeHfShelfDb (ear);
    s.combDepthDb = engine.shapeCombDepthDb (ear);  s.combDelayMs = engine.shapeCombDelayMs (ear);
    s.effLoHz     = engine.shapeEffLoHz (ear);      s.effHiHz     = engine.shapeEffHiHz (ear);
    s.lobeWidth   = engine.shapeLobeWidth (ear);    s.stepDb      = engine.shapeStepDb (ear);
    s.resonanceHz = engine.shapeResonanceHz (ear);  s.humBaseHz   = engine.shapeHumBaseHz (ear);
    return s;
}

void MainComponent::updateStatusLine() {
    const auto st = engine.status();
    // Set-if-changed model (Bug B): every branch computes the FINAL (text, colour, tooltip) for statusLine and
    // statusLineR, then commits once through setLabelIfChanged / setTooltipIfChanged. We DON'T pre-clear the
    // lines here — clearing-then-conditionally-resetting is what made the notes blink. Defaults below are the
    // "blank second line / no tooltip" most branches want; a branch overrides them, and the single commit at
    // the end writes only what actually changed.
    juce::String sText, s2Text, sTip, s2Tip;
    juce::Colour sCol = Theme::textDim(), s2Col = Theme::warn();
    if (st == EngineStatus::Running) {
        // #50: the Running ladder is the PURE eb::runningStatus (gui/StatusLadder.h). This method only
        // BUILDS the snapshot from engine getters (including the stateful/debounced tick counters), maps
        // StatusTone -> Theme colours, and commits set-if-changed. The honesty fixes (#1 #4 #21 #44) and
        // the full precedence order are implemented + headlessly tested in the ladder itself
        // (tests/test_statusladder.cpp) - do NOT reintroduce branch logic here.
        const auto h = engine.health();
        // Advance the live-readout state (peak-hold/decay + the sweep-active debounce) EVERY tick, even
        // if a hard error wins this render, so the held level + debounce stay continuous.
        const LiveReadout live = updateLiveInputReadout();
        eb::RunningSnapshot snap;
        snap.cleanCapture = h.cleanCapture;
        if (! h.cleanCapture) snap.invalidMessage = eb::invalidMeasurementMessage (h.flags);
        snap.liveOwns     = live.owns;
        snap.liveText     = live.primary;
        snap.outputClip   = any (h.flags & HealthFlag::ClipOutput);
        snap.silentHold   = silentTicks_   >= kSilentHoldTicks;
        snap.lowSnrHold   = lowSnrTicks_   >= kLowSnrHoldTicks;
        snap.lowLevelHold = lowLevelTicks_ >= kLowLevelHoldTicks;
        snap.snrL = engine.refSweepSnrDb (0);
        snap.snrR = engine.refSweepSnrDb (1);
        snap.snrCombined = engine.completedSweepSnrDb();
        // #55: an UNVERIFIABLE chain (a poll ran, nothing readable) warns like a failed gate — the old
        // checked-only key left it silent, so an unverifiable chain read as implicitly fine.
        snap.rateVeto    = (chainVerdict_.checked && ! chainVerdict_.all48k) || chainVerdict_.unverifiable;
        snap.rateSummary = chainVerdict_.summary;
        snap.referenceLoaded = engine.referenceLoaded();
        // P3 Task 7: the per-ear snapshot is the extracted earGradeSnapshot() member - ONE truth
        // shared with the VerdictCard composition (refreshMeasureView) so ladder and card can't drift.
        snap.earL = earGradeSnapshot (0);
        snap.earR = earGradeSnapshot (1);
        snap.phaseIdleOrPreflight = h.session == SessionPhase::Idle || h.session == SessionPhase::Preflight;
        snap.phaseComplete        = h.session == SessionPhase::Complete;
        snap.gradeSignalPresent   = engine.gradeSignalPresent();
        snap.osResampled          = any (h.flags & HealthFlag::OsResampled);
        snap.autoDetectedHardwareDirac = engine.autoDetectedHardwareDirac();
        snap.hardwareDiracSetting      = settings.diracHardwareProcessor();
        snap.advisoryTail = chainAdvisoryTail();

        const auto lines = eb::runningStatus (snap);
        // Log each Listening <-> Sweep-in-progress transition (side-effect kept out of the pure ladder).
        if ((lines.line1.text.startsWith ("Listening for the Dirac sweep")
             || lines.line1.text.startsWith ("Sweep in progress"))
            && lines.line1.text != lastListenTextLogged_) {
            logLine (eb::DiagnosticLog::Level::Info, "RefMon status: " + lines.line1.text);
            lastListenTextLogged_ = lines.line1.text;
        }
        auto toneColour = [&live] (eb::StatusTone t) {
            switch (t) {
                case eb::StatusTone::Ok:     return Theme::ok();
                case eb::StatusTone::Warn:   return Theme::warn();
                case eb::StatusTone::Danger: return Theme::danger();
                case eb::StatusTone::Live:   return live.colour;   // the meter logic's own colour role
                default:                     return Theme::textDim();
            }
        };
        // SINGLE COMMIT (Bug B: set-if-changed): both lines committed every tick; a blank line2 clears
        // any stale per-ear verdict under a hard/global message (the header's two-line contract).
        setLabelIfChanged   (statusLine,  lines.line1.text, toneColour (lines.line1.tone));
        setTooltipIfChanged (statusLine,  lines.line1.tip);
        setLabelIfChanged   (statusLineR, lines.line2.text, toneColour (lines.line2.tone));
        setTooltipIfChanged (statusLineR, lines.line2.tip);
        return;
    }
    if (st == EngineStatus::Error) {
        // Render the specific reason (e.g. a device disconnect) so it survives a later re-render that
        // would otherwise clobber a directly-set label back to a bare "Error".
        sText = statusErrorMsg_.isNotEmpty() ? statusErrorMsg_ : juce::String ("Error");
        sCol  = Theme::danger();
    } else if (! (inputPicker.selectedDevice().has_value()
                  && outputPicker.selectedDevice().has_value())) {
        sText = "Select an input and output device";
        sCol  = Theme::textDim();
    } else if (isRealEarsInput()
               && outputPicker.selectedDevice().has_value()
               && ! outputPicker.selectedDevice()->isVirtualSink
               && ! settings.advancedOverride()) {
        // P1-09: a real EARS into a physical output can't reach Dirac. Start is disabled;
        // point the user at the virtual cable. Skipped when the advanced override is on:
        // the user has explicitly opted into a non-Dirac configuration.
        sText = "Select the virtual audio cable as the output to start";
        sCol  = Theme::warn();
    } else if (isRealEarsWithCable()
               && settings.combineMode() != CombineMode::AutoPerEar
               && ! settings.advancedOverride()) {
        // D7 / R17: non-Auto combine mode selected with a real EARS + virtual cable.
        // Start is disabled; tell the user exactly why and what to change. Skipped when the
        // advanced override is on (the user has opted into a non-Dirac use).
        sText = "Set Combine Mode to Auto per-ear (Dirac) to start";
        sCol  = Theme::warn();
    } else if (engine.calibrationApplied()) {
        // Ready: a valid generation is applied. Normally no redundant label (the enabled Start button is
        // the affordance) -- BUT call out two advisory states (dim, never a Start block):
        //   (a) the advanced override is ON and is actually relaxing a gate (the config would otherwise be
        //       blocked as a non-Dirac path) -- make the relaxed state visible so it isn't silent;
        //   (b) NEITHER cal file carried a side marker (no content line, no filename L/R), so the validator
        //       could not have caught a swap.
        const bool overrideRelaxing =
            settings.advancedOverride()
         && ((isRealEarsInput() && outputPicker.selectedDevice().has_value()
              && ! outputPicker.selectedDevice()->isVirtualSink)
             || (isRealEarsWithCable() && settings.combineMode() != CombineMode::AutoPerEar));
        const auto lCal = leftCal.calFile();   // bind once: calFile() returns optional<CalFile> by value
        const auto rCal = rightCal.calFile();
        const bool bothSideUnknown =
            lCal.has_value()  && lCal->side  == eb::CalSide::Unknown
         && rCal.has_value() && rCal->side == eb::CalSide::Unknown;
        if (overrideRelaxing) {
            sText = "Advanced override on" + kDash + "not the standard Dirac path.";
            sCol  = Theme::textDim();
        } else if (bothSideUnknown) {
            sText = "Couldn't confirm left/right from these files" + kDash + "double-check the slots.";
            sCol  = Theme::textDim();
        } else if ((chainVerdict_.checked && ! chainVerdict_.all48k) || chainVerdict_.unverifiable) {
            // 48k-everywhere warning surfaced PRE-START: devices + cals are ready and no higher-precedence
            // gate (device/mode/override/side) is pending, but the chain isn't all-48k — or (#55) it
            // couldn't be verified at all. Warn now so the user fixes the rate BEFORE running a sweep the
            // reference monitor would have to invalidate.
            sText = chainVerdict_.summary;
            sCol  = Theme::warn();
        } else {
            sText = {};
            sCol  = Theme::textDim();
        }
    } else if (leftCal.hasCal() && rightCal.hasCal()) {
        // Both files are loaded but no valid generation is applied yet: either the validator rejected the
        // pair (surface the reason in warn colour) or the build is still in flight (dim "Preparing...").
        // While a new build is in flight, calibrationDiagnostic() still reflects the PREVIOUS generation, so
        // only trust it once the build has caught up (requested == built) -- otherwise a stale reject reason
        // could flash for a pair that is actually valid and still building.
        const bool building = engine.requestedGeneration() != engine.builtGeneration();
        const auto diag     = engine.calibrationDiagnostic();
        if (! building && diag.isNotEmpty()) {
            sText = diag;
            sCol  = Theme::warn();
        } else {
            sText = "Preparing calibration" + kEllipsis;
            sCol  = Theme::textDim();
        }
    } else {
        sText = "Load both ear calibrations to start";
        sCol  = Theme::textDim();
    }

    // SECONDARY chain-config advisory (input-channels warn / 16-bit / non-2ch): a calm INFO tail ONLY
    // when the RATE is fine and the chosen line is CALM (ok/dim, never a warn/error). Folded into the
    // text BEFORE the single commit so the appended note is computed once and stays put (Bug B). The
    // Running branch returned above with its own commit - only the stopped/config states land here.
    const auto tail = chainAdvisoryTail();
    if (tail.isNotEmpty() && ((sCol == Theme::ok()) || (sCol == Theme::textDim())))
        sText = sText.isNotEmpty() ? (sText + tail) : chainVerdict_.advisory;
    // SINGLE COMMIT (Bug B: set-if-changed, no clear-then-reset). Each branch wrote the desired
    // statusLine/statusLineR (text, colour, tooltip) into locals; commit once, writing each label only
    // when it actually changed so the lines persist and the notes don't flicker.
    setLabelIfChanged (statusLine,  sText,  sCol);
    setTooltipIfChanged (statusLine,  sTip);
    setLabelIfChanged (statusLineR, s2Text, s2Col);
    setTooltipIfChanged (statusLineR, s2Tip);
}

// kDash + "<advisory>" when the chain-config advisory should decorate a calm status line; empty otherwise.
juce::String MainComponent::chainAdvisoryTail() const {
    return (chainVerdict_.checked && chainVerdict_.all48k && chainVerdict_.advisory.isNotEmpty())
         ? kDash + chainVerdict_.advisory : juce::String();
}


// ---- Reference-Based Measurement Monitor (Plan 5): learn + grade ----------------------
void MainComponent::loadStoredReference() {
    // Reload a previously-learned reference so it survives an app restart -- re-learning needs the
    // fiddly Windows-Audio + exclusive-off dance, so don't make the user repeat it every launch. v1 is
    // 48k-only (the capture asserted 48k), so the raw f32 samples read back at 48 kHz. A STALE reference
    // (Dirac's sweep changed since it was learned) is caught at grade time by the match-gate -> the
    // status reads "re-learn", never a false grade, so reloading a possibly-old reference is safe.
    // Per-Ear Per-Channel Grading (Task 2): the reference is now stored PER CHANNEL as two files
    // (reference_L.f32 + reference_R.f32). An old single mono reference.f32 CANNOT grade per-ear (it
    // downmixed away Dirac's L/R separation), so we must NOT silently load it as a grade reference —
    // that would mis-grade. classifyReferenceFiles encodes the three outcomes (PURE; unit-tested):
    //   both present -> load; only the old mono (or one channel) -> force a RE-LEARN; nothing -> idle.
    auto dir = appDataDir();   // #24: the TestConfig override keeps headless tests off the real reference store
    auto refL   = dir.getChildFile ("reference_L.f32");
    auto refR   = dir.getChildFile ("reference_R.f32");
    auto refOld = dir.getChildFile ("reference.f32");

    // A channel is "present" only if it is a real (long-enough) reference, not a stray short/empty file —
    // mirror the old sanity floor (a real reference is many seconds long, not a few stray samples).
    auto longEnough = [] (const juce::File& f) {
        return f.existsAsFile() && (f.getSize() / (juce::int64) sizeof (float)) >= 48000;
    };
    const bool hasL    = longEnough (refL);
    const bool hasR    = longEnough (refR);
    const bool hasMono = longEnough (refOld);

    switch (eb::classifyReferenceFiles (hasL, hasR, hasMono)) {
        case eb::ReferenceFilesState::PerChannel: {
            juce::MemoryBlock mbL, mbR;
            if (! refL.loadFileAsData (mbL) || ! refR.loadFileAsData (mbR)) return;
            const int nL = (int) (mbL.getSize() / sizeof (float));
            const int nR = (int) (mbR.getSize() / sizeof (float));
            if (nL < 48000 || nR < 48000) return;   // re-check after the read (defensive)
            const auto* fL = static_cast<const float*> (mbL.getData());
            const auto* fR = static_cast<const float*> (mbR.getData());
            // #5/#20: verify the integrity sidecar BEFORE trusting the pair. It binds the learn-time RATE
            // (the loader previously hardcoded 48 kHz while the capture asserted the settings rate - a
            // mislabeled reference defeated the grade-time rate guard in BOTH directions) and each channel's
            // length + SHA-256 (nothing previously detected a truncated write or a mixed-generation pair).
            // Absent/mismatched -> honest re-learn, exactly like the old-mono path below.
            const auto diskL = eb::makeReferenceMetadata (fL, nL, 0.0);
            const auto diskR = eb::makeReferenceMetadata (fR, nR, 0.0);
            const auto meta  = eb::checkReferenceMeta (dir.getChildFile ("reference.meta").loadFileAsString(),
                                                       diskL, diskR);
            if (! meta.valid) {
                engine.setReferenceLoaded (false);
                learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
                // SHORT for the rail (the HIG gate caught the long form clipping); the reason lives in the
                // tooltip + log. Every pre-sidecar install hits this ONCE (their reference predates the
                // integrity metadata) - a single re-learn upgrades them.
                learnRefResultLabel.setText ("Re-learn the reference (integrity check failed).",
                                             juce::dontSendNotification);
                learnRefResultLabel.setTooltip ("The saved reference failed its integrity/rate check: " + meta.reason
                                                + ". Click Learn reference (Windows Audio) to capture a new one.");
                logLine (eb::DiagnosticLog::Level::Warn,
                         "Reference reload REFUSED: sidecar check failed (" + meta.reason + ") - re-learn required.");
                return;
            }
            loadedReferenceL_.assign (fL, fL + nL);
            loadedReferenceR_.assign (fR, fR + nR);
            loadedReferenceRateL_ = meta.rate;       // #5: the REAL learn-time rate from the verified sidecar
            loadedReferenceRateR_ = meta.rate;
            loadedReferenceEndpoint_ = meta.endpoint;   // #34: "" on a legacy sidecar (no binding recorded)
            referenceStatePath_  = refL.getFullPathName();
            engine.setReferenceLoaded (true);
            // AutoPerEar: reload the schedule sidecar ONLY if it matches this reference (stale guard), then install
            // it (startup -> bridge stopped). A mismatched / absent sidecar -> the router falls back to the envelope.
            const auto sched = eb::deserializeSchedule (dir.getChildFile ("schedule.txt").loadFileAsString(),
                                                        diskL.contentHash);
            if (sched.valid) engine.setSweepSchedule (sched);
            return;
        }
        case eb::ReferenceFilesState::ReLearnNeeded: {
            // An old mono reference (or a half-written per-channel pair) is on disk. It cannot grade per-ear,
            // so leave referenceLoaded FALSE and tell the user once: they must RE-LEARN. We do NOT auto-delete
            // the old file here (only a successful re-learn replaces it) — a failed re-learn shouldn't lose it.
            engine.setReferenceLoaded (false);
            learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
            learnRefResultLabel.setText ("Re-learn needed: the saved reference is the old mono format and "
                                         "can't grade each ear. Learn a new one (Windows Audio).",
                                         juce::dontSendNotification);
            logLine (eb::DiagnosticLog::Level::Warn,
                     "Reference reload: only an old mono reference found - per-ear grading needs a re-learn.");
            return;
        }
        case eb::ReferenceFilesState::None:
        default:
            return;   // nothing learned yet; stay idle
    }
}

void MainComponent::onLearnReference() {
    // The button doubles as Learn / Cancel. WHILE a capture is in flight, a click REQUESTS CANCEL: flip the
    // atomic the firPool job polls (inside captureLoopback) and show a brief "Cancelling..." note. The job's
    // callAsync continuation owns restoring the idle state, so we don't touch learning_/text here.
    if (learning_) {
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Cancel learning clicked");
        learnCancelRequested_.store (true, std::memory_order_relaxed);
        learnRefButton.setEnabled (false);   // re-enabled by the job continuation; blocks a double-cancel/start
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
        learnRefResultLabel.setText ("Cancelling" + kEllipsis, juce::dontSendNotification);
        return;
    }

    logLine (eb::DiagnosticLog::Level::Debug, "Button: Learn reference clicked");

    // --- Pre-start guards (only apply before a capture is in flight) ---
    // The learn capture is a Windows WASAPI loopback (on-device) — it can only succeed while Dirac's
    // Processor plays through "Windows Audio" (shared); in ASIO/exclusive the loopback is silent. We
    // detect the mode READ-ONLY from the settings file (never edit it — auto-switch is deferred to v2)
    // and warn before we even try. The capture + validate runs OFF the message thread (firPool).
    if (engine.status() == EngineStatus::Running) {
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Stop the bridge before learning a reference.", juce::dontSendNotification);
        logLine (eb::DiagnosticLog::Level::Warn, "Learn refused: bridge is running.");
        return;
    }
    if (settings.diracHardwareProcessor()) {
        // #22 backstop: the button is normally disabled in hardware-Dirac mode (updateControlsEnabled), but a
        // stale enable must still refuse - the box plays its sweep internally, there is no PC loopback to learn.
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Hardware Dirac: the processor plays the sweep internally" + kDash + "there is no PC loopback to learn.",
                                     juce::dontSendNotification);
        logLine (eb::DiagnosticLog::Level::Warn, "Learn refused: hardware-Dirac processor mode.");
        return;
    }

    // Snapshot the Dirac config before every learn (the loopback can only capture in Windows Audio mode).
    logDiracSnapshot ("before learn");

#if JUCE_WINDOWS
    // Read-only Dirac-mode detection: inform if it's not in Windows Audio (the loopback will be silent).
    const juce::String deviceType = eb::readDiracDeviceType();
    if (deviceType.isNotEmpty() && ! eb::diracDeviceTypeIsWindowsAudio (deviceType)) {
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Dirac is in \"" + deviceType + "\"" + kDash + "set it to Windows Audio AND turn off "
                                     "exclusive mode on your output device, then learn.", juce::dontSendNotification);
        logLine (eb::DiagnosticLog::Level::Warn,
                 "Learn refused: Dirac deviceType is \"" + deviceType + "\" (not Windows Audio).");
        return;
    }

    // --- Start the learn: enter the learning state. The button stays ENABLED and becomes "Cancel learning". ---
    learning_ = true;
    learnCancelRequested_.store (false, std::memory_order_relaxed);
    learnRefButton.setButtonText ("Cancel learning");
    learnRefButton.setEnabled (true);
    // The loopback target is the device DIRAC PLAYS THE SWEEP TO -- read straight from Dirac's own
    // settings (DEVICESETUP audioOutputDeviceName), so it works for ANY user's output device, not a
    // specific model. NOT EARS Bridge's output: the VB-CABLE carries the RESPONSE Dirac records, not the
    // sweep. If Dirac's setting can't be read (e.g. a different Dirac product/folder), an EMPTY target
    // tells captureLoopback to use the system DEFAULT render endpoint -- a sound guess for where Dirac
    // plays -- rather than the wrong cable. Either way the status names what we land on so the user can
    // verify it's right (or cancel and fix it).
    const juce::String diracDevice = eb::readDiracOutputDeviceName();
    const juce::String renderTarget = diracDevice;   // "" -> default render endpoint inside captureLoopback
    logLine (eb::DiagnosticLog::Level::Info,
             "Learn start: loopback target=" + (diracDevice.isNotEmpty() ? ("\"" + diracDevice + "\"")
                                                                          : juce::String ("default render endpoint")));

    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    learnRefResultLabel.setText (diracDevice.isNotEmpty()
            ? ("Learning from \"" + diracDevice + "\"" + kDash + "run a Dirac measurement now (stops automatically when the sweep ends)" + kEllipsis)
            : "Learning from the default playback device" + kDash + "run a Dirac measurement now (stops automatically when the sweep ends)" + kEllipsis,
        juce::dontSendNotification);

    juce::Component::SafePointer<MainComponent> safe (this);
    const double rate = activeRate();
    // The cancel token is a MainComponent member; capture its address now (message thread, `this` is alive).
    // The component is destroyed only on the message thread, and only after this job's callAsync continuation
    // has cleared learning_ — so the pointer stays valid for the duration of the capture.
    const std::atomic<bool>* cancel = &learnCancelRequested_;
    firPool->addJob ([safe, rate, renderTarget, cancel]() mutable {
        // Per-Ear Per-Channel Grading (Task 2): capture the loopback PER CHANNEL (no downmix), then validate
        // EACH channel independently. Dirac HARD-PANS its measurement sweeps, so ref_L = render ch0 holds the
        // LEFT sweep (with the RIGHT half silent) and ref_R = ch1 holds the RIGHT sweep — each channel contains
        // exactly ONE sweep, so the existing single-sweep self-test (validateReferenceCapture) run PER CHANNEL
        // finds that channel's sweep. A channel that fails to validate -> the learn fails, NAMING which ear, so
        // we never store a half-bad per-ear reference. The capture AUTO-STOPS the moment Dirac's sweep sequence
        // ends (end-of-sweep detector inside captureLoopbackStereo, keyed off max(|L|,|R|) so the L-then-R
        // sequence stays whole); 35 s is only the MAXIMUM cap. The cancel token lets a Cancel click abort.
        auto cap = eb::captureLoopbackStereo (renderTarget, 35.0, rate, cancel);   // Dirac's output render endpoint; 35 s = max cap
        juce::String resultMsg; bool ok = false; bool cancelled = cap.cancelled;
        std::vector<float> samplesL, samplesR; double capRate = rate;
        eb::SweepSchedule schedule;   // learned from the FULL stereo loopback (before trimming) for AutoPerEar routing
        if (cap.cancelled) {
            resultMsg = "Learning cancelled.";
        } else if (! cap.ok) {
            resultMsg = "Capture failed: " + cap.reason;
        } else {
            // Dirac HARD-PANS its sweeps, so each captured channel is [its own sweep ~10 s][silence ~11 s]
            // (the silent half is where the OTHER ear sweeps on the other channel). Grading deconvolves
            // against the FULL reference, so that silent half divides by ~zero and amplifies noise, dragging
            // IR-SNR strongly negative even on a clean capture. TRIM each channel to JUST its own sweep span
            // BEFORE validating + storing, so the stored reference holds only the ~10 s sweep. A channel with
            // no sweep (all-silent) or an implausibly short span fails the learn, NAMING which ear.
            const auto spanL = eb::findActiveSpan (cap.samplesL.data(), (int) cap.samplesL.size(), cap.rate);
            const auto spanR = eb::findActiveSpan (cap.samplesR.data(), (int) cap.samplesR.size(), cap.rate);
            if (! spanL.valid)      { resultMsg = "Rejected (Left ear): no sweep found"; }
            else if (! spanR.valid) { resultMsg = "Rejected (Right ear): no sweep found"; }
            // #35: the RELATIVE -40 dB trim threshold can mis-place a span under leakage/crosstalk. Two sanity
            // bounds catch that: the two ears' spans must be DISJOINT in time (Dirac hard-pans - overlapping
            // spans mean the "silent half" wasn't silent), and neither span may swallow most of the capture.
            else if (spanL.first < spanR.last && spanR.first < spanL.last) {
                resultMsg = "Rejected: the two ears' sweeps overlap in time" + kDash + "not cleanly panned "
                            "(leakage, a second source, or a non-hard-panned processor)";
            }
            else if ((spanL.last - spanL.first) > (int) (0.8 * (double) cap.samplesL.size())
                  || (spanR.last - spanR.first) > (int) (0.8 * (double) cap.samplesR.size())) {
                resultMsg = "Rejected: a sweep span covers most of the capture" + kDash + "no clean silent half "
                            "(noise or leakage on the quiet channel?)";
            }
            else {
                std::vector<float> trimL (cap.samplesL.begin() + spanL.first, cap.samplesL.begin() + spanL.last);
                std::vector<float> trimR (cap.samplesR.begin() + spanR.first, cap.samplesR.begin() + spanR.last);
                // Validate each TRIMMED channel on its own — each must contain a clean single sweep. Name the
                // failing ear precisely. minSeconds=3 here (NOT the full-sequence default ~8 s): the trimmed
                // span is ONE per-ear sweep, which Dirac plays in ~5 s — so 8 s would wrongly reject a correct
                // single-sweep reference (it did: a real R sweep trimmed to 5.1 s failed the 8 s floor). 3 s
                // still rejects a stray/truncated blip.
                const auto vL = eb::validateReferenceCapture (trimL.data(), (int) trimL.size(), cap.rate, 3.0);
                const auto vR = eb::validateReferenceCapture (trimR.data(), (int) trimR.size(), cap.rate, 3.0);
                if (! vL.ok)      { resultMsg = "Rejected (Left ear): " + vL.reason; }
                else if (! vR.ok) { resultMsg = "Rejected (Right ear): " + vR.reason; }
                else {
                    ok = true;
                    samplesL = std::move (trimL);
                    samplesR = std::move (trimR);
                    capRate  = cap.rate;
                    // Learn the AutoPerEar SCHEDULE from the FULL untrimmed loopback (the [L gap R gap L] structure
                    // lives in the gaps that trimming removes). Offline/pure - safe on this worker thread.
                    schedule = eb::extractSchedule (cap.samplesL.data(), cap.samplesR.data(),
                                                    (int) cap.samplesL.size(), cap.rate);
                    resultMsg = "Reference learned" + kDash + "both ears captured (L "
                                + juce::String (samplesL.size() / capRate, 1) + " s, R "
                                + juce::String (samplesR.size() / capRate, 1) + " s)" + kDash + "see tip to resume listening.";
                }
            }
        }
        juce::MessageManager::callAsync ([safe, ok, cancelled, resultMsg, renderTarget,
                                          samplesL = std::move (samplesL), samplesR = std::move (samplesR),
                                          schedule = std::move (schedule), capRate]() mutable {
            auto* mc = safe.getComponent();
            if (! mc) return;
            // Back to idle: restore the button to "Learn reference (Windows Audio)" and re-enable, so the
            // user can immediately restart. A cancelled result is shown NEUTRAL (dim), not as a failure.
            mc->learning_ = false;
            mc->learnCancelRequested_.store (false, std::memory_order_relaxed);
            mc->learnRefButton.setButtonText ("Learn reference (Windows Audio)");
            mc->learnRefButton.setEnabled (true);
            mc->learnRefResultLabel.setColour (juce::Label::textColourId,
                                               cancelled ? Theme::textDim() : (ok ? Theme::ok() : Theme::warn()));
            mc->learnRefResultLabel.setText (resultMsg, juce::dontSendNotification);
            // Log the learn OUTCOME (the resultMsg is already user-facing + serial-free; backstopped anyway).
            mc->logLine (cancelled ? eb::DiagnosticLog::Level::Info
                                   : (ok ? eb::DiagnosticLog::Level::Info : eb::DiagnosticLog::Level::Warn),
                         "Learn result: " + resultMsg);
            if (ok) {
                // The short rail label can't hold the full round-trip reminder, so park it in a tooltip.
                mc->learnRefResultLabel.setTooltip ("Re-enable exclusive mode on your output device and set "
                                                    "Dirac back to ASIO to resume normal listening.");
                // Store BOTH channels on disk as reference_L.f32 / reference_R.f32 (raw f32, same format as the
                // old single file) so the per-ear reference survives a restart. DELETE the obsolete mono
                // reference.f32 if present — it would be mistaken for a valid reference on a future reload.
                auto dir = mc->appDataDir();   // #24: honours the TestConfig override
                dir.createDirectory();
                auto refL = dir.getChildFile ("reference_L.f32");
                auto refR = dir.getChildFile ("reference_R.f32");
                // #5/#20: CHECK the writes (they were fire-and-forget - a disk-full mid-write could install a
                // truncated or mixed-generation pair) and bind the pair with an integrity sidecar carrying the
                // learn-time RATE + each channel's length + SHA-256. The loader verifies all of it (#5 fixed
                // the reload hardcoding 48 kHz while the capture ran at the settings rate).
                const auto metaL  = eb::makeReferenceMetadata (samplesL.data(), (int) samplesL.size(), capRate);
                const auto metaR  = eb::makeReferenceMetadata (samplesR.data(), (int) samplesR.size(), capRate);
                // #34: bind the pair to the endpoint it was learned FROM (Dirac's output render device) -
                // stable UID when it resolves, else the display name; an empty target (system default
                // render) records nothing. A later device swap then gets a start-time warning instead of
                // silently grading against a reference from a different signal path.
                juce::String learnEndpoint;
                if (renderTarget.isNotEmpty()) {
                    learnEndpoint = eb::endpointUidForName (renderTarget, /*isInput*/ false);
                    if (learnEndpoint.isEmpty()) learnEndpoint = renderTarget;
                }
                mc->loadedReferenceEndpoint_ = learnEndpoint;
                const bool wroteL = refL.replaceWithData (samplesL.data(), samplesL.size() * sizeof (float));
                const bool wroteR = refR.replaceWithData (samplesR.data(), samplesR.size() * sizeof (float));
                const bool wroteM = dir.getChildFile ("reference.meta")
                                       .replaceWithText (eb::serializeReferenceMeta (metaL, metaR, learnEndpoint));
                const bool persistOk = wroteL && wroteR && wroteM;
                if (! persistOk) {
                    // Fail LOUDLY and leave the disk CLEAN: a partial pair that still cross-correlates would
                    // grade against garbage on the next launch. The in-memory reference stays active for THIS
                    // session (honest - the message says so).
                    refL.deleteFile(); refR.deleteFile();
                    dir.getChildFile ("reference.meta").deleteFile();
                    dir.getChildFile ("schedule.txt").deleteFile();
                    mc->learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
                    mc->learnRefResultLabel.setText ("Reference learned for THIS session, but saving it FAILED "
                                                     "(disk full / permissions?)" + kDash + "it will need re-learning next launch.",
                                                     juce::dontSendNotification);
                    mc->logLine (eb::DiagnosticLog::Level::Error,
                                 "Reference save FAILED (L=" + juce::String (wroteL ? 1 : 0)
                               + " R=" + juce::String (wroteR ? 1 : 0) + " meta=" + juce::String (wroteM ? 1 : 0)
                               + ") - partial files removed");
                }
                auto refOld = dir.getChildFile ("reference.f32");
                if (refOld.existsAsFile()) refOld.deleteFile();   // obsolete mono ref -> remove on a successful re-learn
                mc->referenceStatePath_ = refL.getFullPathName();
                // Hold both channels IN MEMORY so the per-ear grade path (gradeOneEar) can deconvolve each
                // earcup against its own reference channel (ref_L / ref_R).
                mc->loadedReferenceL_     = samplesL;   // copy (moved-in but still readable here)
                mc->loadedReferenceR_     = samplesR;
                mc->loadedReferenceRateL_ = capRate;
                mc->loadedReferenceRateR_ = capRate;
                mc->engine.setReferenceLoaded (true);
                // #67: if Start raced in during the capture window, a poller mid-debounce could pair the OLD
                // reference's alignment with the NEW reference bytes - re-arm both pollers and invalidate any
                // in-flight grade so the first grade against this reference starts from a clean match.
                if (mc->engine.status() == EngineStatus::Running) {
                    mc->gradePollerL_.reset(); mc->gradePollerR_.reset();
                    mc->gradeRunGen_.fetch_add (1, std::memory_order_relaxed);
                    mc->gradeInFlight_.store (false);
                }
                // #65: record what the capture actually saw so an endpoint/config mismatch is visible in the log.
                mc->logLine (eb::DiagnosticLog::Level::Info,
                             "Learn capture: endpoint mix " + juce::String (capRate, 0) + " Hz (Dirac output target)");
                // AutoPerEar: persist the learned schedule keyed to this reference's content hash, then install it
                // (the bridge is STOPPED during Learn). A future re-learn changes the hash -> the stale schedule
                // drops. NOT persisted when the reference itself failed to save (#20 - a schedule bound to an
                // unsaved reference would be stale by construction).
                if (schedule.valid) {
                    const auto refHash = metaL.contentHash;
                    if (persistOk)
                        dir.getChildFile ("schedule.txt").replaceWithText (eb::serializeSchedule (schedule, refHash));
                    // Install ONLY if the engine is stopped: setSweepSchedule -> loadSchedule allocates + is not mid-run
                    // safe. If Start raced in during the Learn capture, skip the live install - the sidecar is already
                    // persisted, so the next launch's loadStoredReference installs it on the stopped engine.
                    const bool installed = mc->engine.reconfigAllowed();
                    if (installed) mc->engine.setSweepSchedule (schedule);
                    mc->logLine (eb::DiagnosticLog::Level::Info, "AutoPerEar schedule learned ("
                                 + juce::String ((int) schedule.segments.size()) + " segments)"
                                 + (installed ? juce::String() : juce::String (" - install deferred, engine running")));
                } else {
                    dir.getChildFile ("schedule.txt").deleteFile();   // no schedule learned -> drop any stale sidecar
                }
            }
        });
    });
#else
    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
    learnRefResultLabel.setText ("Reference learning needs the Windows WASAPI loopback.", juce::dontSendNotification);
#endif
}

void MainComponent::pollReferenceGrade() {
    // Per-Ear Per-Channel Grading (Task 4). The capture thread buffers BOTH mic channels CONTINUOUSLY into two
    // independent rolling rings (left mic -> ringL, right mic -> ringR) and publishes a fresh window snapshot per
    // ear every ~0.5 s. Here, throttled to ~2 s, we grade EACH earcup against the channel that drove it — Dirac
    // hard-pans its sweeps, so ringL is graded vs ref_L and ringR vs ref_R via TWO independent pollers. The MATCH
    // is the DETECTOR (any level), and there is NO absolute level gate anywhere in this grade path.
    //
    // Grade on a STABLE MATCH, not on silence (the old "wait for the signal to settle" trigger never fired because
    // the EARS mic always registers ambient): each ear's poller grades only when ITS match holds across two
    // consecutive ~2 s polls. The two ears are FULLY INDEPENDENT — one earcup can grade GradedClean while the
    // other (a silent ring, or no sweep on that channel) never matches and stays at its base Learned state. A
    // silent ear can NEVER publish a clean grade or raise LowSnr: decide() returns not-matched on silence, so
    // gradeOneEar publishes nothing for it (the honesty gate).
    // Hardware-Dirac (DDRC-24/SHD/Flex): the box makes its OWN sweep, so there is no loopback reference and no
    // grade can run. Toggle ON -> grading already off (engine published GradingOffHardware); skip the grade.
    if (engine.diracHardwareProcessorActive()) return;
    // Toggle OFF + a measurement running -> AUTO-DETECT the box (suggest only). Throttled (~2 s) because the
    // OutputActivity read is a COM device enumeration. This runs even with NO reference loaded (the hardware case),
    // so it sits BEFORE the reference guards below.
    if (engine.status() == eb::EngineStatus::Running && ++hwDetectTick_ >= kGradePollTicks) {
        hwDetectTick_ = 0;
        constexpr float kHwMicSweepFloor = 0.01f;   // ~-40 dBFS: above ambient, below a real sweep (PROVISIONAL)
        const float peak = eb::outputRenderPeakForName (eb::readDiracOutputDeviceName());
        if (peak >= 0.0f) { hwOutputReadable_ = true; maxOutputRenderPeak_ = juce::jmax (maxOutputRenderPeak_, peak); }
        // #16: derive micHeard from the UNCONDITIONAL run peak - the session-arm-gated maxSweepPeakL/R never
        // accumulate on a gradual Dirac log-sweep (the +12 dB rise-arm is proven dead on real sweeps), which
        // made this suggestion structurally unreachable in the field.
        const bool micHeard  = hwMicRunPeak_ > kHwMicSweepFloor;
        const bool validMode = eb::diracDeviceTypeIsWindowsAudio (eb::readDiracDeviceType());
        engine.updateHardwareDiracAutoDetect (micHeard, maxOutputRenderPeak_, hwOutputReadable_, validMode);
        if (engine.autoDetectedHardwareDirac())
            logLine (eb::DiagnosticLog::Level::Info, juce::String ("Hardware-Dirac auto-detect: micHeard=")
                + (micHeard ? "1" : "0") + " outRenderPeak=" + juce::String (maxOutputRenderPeak_, 4)
                + " readable=" + (hwOutputReadable_ ? "1" : "0") + " validMode=" + (validMode ? "1" : "0") + " -> SUGGEST");
    }

    if (! engine.referenceLoaded()) return;                              // nothing learned -> nothing to grade against
    if (loadedReferenceL_.empty() && loadedReferenceR_.empty()) return;  // no per-ear reference present

    // ONE in-flight guard for BOTH ears: we grade them SEQUENTIALLY within a tick (L first, then R) so their
    // off-thread jobs never race over the shared firPool/publish; whichever ear is ready + stable posts a job and
    // claims the guard, the other ear grades on the next eligible tick. Both ears eventually grade because their
    // ring snapshots keep republishing and each poller holds its own debounce until ITS sweep lands.
    if (gradeInFlight_.load()) return;                                   // a prior grade (either ear) is still running
    if (++gradePollTick_ < kGradePollTicks) return;                     // ~2 s match-poll throttle (shared clock)
    gradePollTick_ = 0;

    // Grade LEFT vs ref_L; if it did not post a job this tick, grade RIGHT vs ref_R. Each is independent — both
    // run their own snapshot/rate-guard/decide/grade/publish. (If L posts a job, R grades next eligible tick; the
    // guard serializes them so only one off-thread grade is in flight at a time.)
    // Alternate which ear is tried FIRST each eligible poll. gradeOneEar now posts an OFF-THREAD match for the ear
    // it handles and claims gradeInFlight_ (which serializes the two ears across polls), so without alternation the
    // left ear would always win the guard and the right would never grade. Toggling keeps them fair.
    if ((gradeEarToggle_ ^= 1) != 0) {
        if (gradeOneEar (0, gradePollerL_, loadedReferenceL_, loadedReferenceRateL_)) return;
        gradeOneEar (1, gradePollerR_, loadedReferenceR_, loadedReferenceRateR_);
    } else {
        if (gradeOneEar (1, gradePollerR_, loadedReferenceR_, loadedReferenceRateR_)) return;
        gradeOneEar (0, gradePollerL_, loadedReferenceL_, loadedReferenceRateL_);
    }
}

MainComponent::ShapeResult MainComponent::runShapeDetectors (int ear, bool gradedClean,
        const eb::BandCurve* baseline, const std::vector<float>& ir, double bandLoHz, double bandHiHz,
        const std::vector<float>& window, int windowStart, int gradedLen,
        const std::vector<float>& reference, double rate) {
    // WORKER-THREAD (the single-thread firPool): the seven INFO-ONLY shape detectors, run on the SAME IR /
    // band the grade already keyed off. Nothing here gates, demotes, or invalidates a grade or touches
    // cleanCapture. It reads NO engine/component state (static): the D1 baseline is passed in as a COPY and
    // the decision to LEARN a new baseline is returned, so the message thread stays the single writer of the
    // engine baseline (mirrors publishReferenceGrade). spec 2026-07-03-response-drift-monitor-design §4/§5.
    juce::ignoreUnused (ear);
    ShapeResult res;
    if (ir.empty() || rate <= 0.0) return res;

    // The analysis band: the reference's derived band (SP2), pulled straight from the grade artifacts. On the
    // flat fallback (bandLoHz == 0) fall back to the full 20 Hz .. 20 kHz sweep span (windowedBandSpectrum
    // pulls it in 1/12 oct internally). D3 needs the SAME reference edges to compare the measurement against.
    const bool haveBand = bandLoHz > 0.0 && bandHiHz > bandLoHz;
    const double bLo = haveBand ? bandLoHz : 20.0;
    const double bHi = haveBand ? bandHiHz : 20000.0;

    const eb::WindowedSpectrum ws = eb::windowedBandSpectrum (ir.data(), (int) ir.size(), rate, bLo, bHi);

    if (ws.valid) {
        // ---- D1 session drift (baseline-relative) ----------------------------------------------------
        const eb::BandCurve cur = eb::computeBandCurve (ws);
        if (cur.valid) {
            if (baseline != nullptr && baseline->valid) {
                const eb::DriftReport d = eb::compareCurves (*baseline, cur);
                if (d.valid) {
                    res.driftMaxDb = d.maxDeltaDb;
                    res.hfShelfDb  = d.hfShelfDb;
                    if (d.exceedsTolerance) res.flags |= eb::ShapeFlag::kDrift;
                }
            } else if (gradedClean) {
                // The FIRST GradedClean per ear learns the baseline (no note; D1 stays silent until it exists).
                // Returned to the message thread, which is the engine baseline's single writer.
                res.newBaseline = cur;
                res.setBaseline = true;
            }
        }

        // ---- D2 comb / echo --------------------------------------------------------------------------
        const eb::CombReport comb = eb::detectComb (ws, ir.data(), (int) ir.size());
        if (comb.found) {
            res.flags      |= eb::ShapeFlag::kComb;
            res.combDepthDb = comb.depthDb;
            res.combDelayMs = comb.delayMs;
        }

        // ---- D3 band truncation vs the reference band (needs a valid reference band) ------------------
        if (haveBand) {
            const eb::TruncationReport tr = eb::detectTruncation (ws, bandLoHz, bandHiHz);
            if (tr.valid) {
                res.effLoHz = tr.effLoHz;
                res.effHiHz = tr.effHiHz;
                // No measurable measurement band under a VALID reference (both edges zero): the loudest D3
                // finding — the chain is dropping content the reference had. detectTruncation reports this
                // as valid-with-zero-edges (spec §6); raise kNoBand so the INFO note words it separately.
                if (tr.effLoHz == 0.0f && tr.effHiHz == 0.0f) res.flags |= eb::ShapeFlag::kNoBand;
                if (tr.truncatedHi) res.flags |= eb::ShapeFlag::kTruncHi;
                if (tr.truncatedLo) res.flags |= eb::ShapeFlag::kTruncLo;
            }
        }

        // ---- D5b narrow resonance --------------------------------------------------------------------
        const eb::SpikeReport spk = eb::detectResonance (ws);
        if (spk.found) {
            res.flags      |= eb::ShapeFlag::kResonance;
            res.resonanceHz = spk.hz;
        }
    }

    // ---- D6 clock-skew lobe width (raw IR; no windowed spectrum needed) -------------------------------
    const float lobe = eb::mainLobeWidthSamples (ir.data(), (int) ir.size());
    if (lobe > 0.0f) {
        res.lobeWidth = lobe;
        constexpr float kLobeWidthMax = 10.0f;   // spec 4.D6 provisional (pending #54C on-device ratification)
        if (lobe > kLobeWidthMax) res.flags |= eb::ShapeFlag::kSkew;
    }

    // ---- D5a mains hum in the PRE-SWEEP noise region [0, windowStart) ---------------------------------
    // Only when the leading noise region is long enough for the Welch average (spec 4.D5a: len >= 16384).
    if (windowStart >= 16384 && (int) window.size() >= windowStart) {
        const eb::HumReport hum = eb::detectMainsHum (window.data(), windowStart, rate);
        if (hum.found) {
            res.flags    |= eb::ShapeFlag::kHum;
            res.humBaseHz = (int) std::lround (hum.baseHz);
        }
    }

    // ---- D7 level step on the ALIGNED pair (window+windowStart, gradedLen) vs the reference -----------
    if (gradedLen > 0 && windowStart >= 0 && (int) window.size() >= windowStart + gradedLen
        && (int) reference.size() >= gradedLen) {
        const eb::StepReport st = eb::detectLevelStep (window.data() + windowStart, reference.data(),
                                                       gradedLen, rate);
        if (st.found) {
            res.flags  |= eb::ShapeFlag::kStep;
            res.stepDb  = st.stepDb;
        }
    }

    // ---- D4 cross-ear polarity: extract THIS ear's 2048-sample peak segment (the pair is compared on the
    //      message thread once BOTH ears have a fresh matched segment this session). ----------------------
    res.peakSeg.assign ((size_t) kShapePolaritySeg, 0.0f);
    eb::extractPeakSegment (ir.data(), (int) ir.size(), res.peakSeg.data(), kShapePolaritySeg);
    return res;
}

bool MainComponent::gradeOneEar (int ear, eb::ReferenceGradePoller& poller,
                                 const std::vector<float>& reference, double referenceRate) {
    // Grade ONE earcup against ITS OWN reference channel. Returns true iff it posted an off-thread grade job this
    // tick (so the caller can serialize the two ears under the single gradeInFlight_ guard). ear 0 = LEFT, 1 = RIGHT.
    if (reference.empty()) return false;                                 // that ear has no reference -> skip it
    if (! engine.gradingResponseReady (ear)) return false;              // no fresh ring window published for this ear

    // Copy the FULL rolling-ring WINDOW for THIS ear out of the engine into our own buffer (the allocation lives
    // here, not the engine). The window is leading silence/room noise + the sweep somewhere inside it; the worker
    // aligns the reference inside it via cross-correlation, so we must NOT truncate it to the reference length.
    std::vector<float> window ((size_t) juce::jmax (1, (int) std::lround (engine.gradingResponseRate()
                                                                          * eb::AudioEngine::gradingWindowSeconds())), 0.0f);
    const int got = engine.snapshotGradeRing (ear, window.data(), (int) window.size());
    if (got <= 0) return false;
    window.resize ((size_t) got);

    // Fix 2 (honesty): guard the capture rate against THIS ear's reference rate. A measurement at 44.1k matched
    // against a 48k reference is the SAME chirp stretched ~8.8% — it can still clear the provisional match cutoffs
    // and read green "verified", which is a lie. The predicate lives in gui/GradeGuards.h (#32) so the tests
    // assert the PRODUCTION condition. When it refuses, publish ReferenceStale FOR THIS EAR and do NOT grade it.
    const double respRate = engine.gradingResponseRate();
    if (! eb::rateAllowsGrade (referenceRate, respRate)) {
        poller.reset();   // a rate change is not a sweep -> re-arm so a later same-rate sweep can grade
        engine.setLastMatchCoherence (ear, 0.0f);
        engine.publishReferenceGrade (ear, (int) eb::RefMonState::ReferenceStale, 0.0f, 0.0f, /*mismatch*/ true, false);
        return false;
    }

    // The match-gate (cross-correlate align + referenceMatches) is ~3 large FFTs over the WHOLE 28 s window. It
    // used to run here SYNCHRONOUSLY on the message thread every poll and froze the UI for ~0.5 s as the window
    // grew. Move it OFF the message thread: claim the single in-flight guard, run matchAlign() on the firPool
    // worker, then return to the message thread to apply the (cheap, stateful) two-poll debounce — so the poller
    // debounce never races a Stop/reset(). If the debounce says grade, post the heavy deconvolve+grade (reusing
    // matchAlign's alignOffset) on the worker; the existing publish block is unchanged.
    gradeInFlight_.store (true);
    const uint32_t myGen = gradeRunGen_.load (std::memory_order_relaxed);   // stamp this job's run; a later Stop/Start drops it (#4)
    juce::Component::SafePointer<MainComponent> safe (this);
    auto refCopy = reference;                          // copy for the worker (message-thread snapshot)
    const double rate = referenceRate;
    eb::ReferenceGradePoller* pollerPtr = &poller;     // owned by `this`; debounce applied on the message thread only
    firPool->addJob ([safe, ear, refCopy = std::move (refCopy), window = std::move (window), rate, pollerPtr, myGen]() mutable {
      // OFF-THREAD: the heavy match-gate ONLY (no debounce state touched here).
      const eb::GradePollResult mr = eb::ReferenceGradePoller::matchAlign (
          window.data(), (int) window.size(), refCopy.data(), (int) refCopy.size());
      juce::MessageManager::callAsync (
          [safe, ear, mr, refCopy = std::move (refCopy), window = std::move (window), rate, pollerPtr, myGen]() mutable {
        auto* mc = safe.getComponent();
        if (! mc) return;                              // component gone (app closing) -> guard lives on it; moot
        if (mc->gradeRunGen_.load (std::memory_order_relaxed) != myGen) return;   // a Stop/Start happened since post -> drop this stale grade (#4)
        mc->engine.setLastMatchCoherence (ear, mr.coherence);
        // Debounce on the MESSAGE THREAD (no race with reset()): two consecutive matched polls -> grade.
        const bool didGrade = pollerPtr->applyDebounce (mr.matched);
        mc->logLine (eb::DiagnosticLog::Level::Debug,
                     juce::String ("GradePoll: ear=") + (ear == 1 ? "R" : "L")
                     + " coher=" + juce::String (mr.coherence, 3)
                     + " mainLobe=" + juce::String (mr.mainLobe, 3)
                     + " matched=" + juce::String (mr.matched ? "1" : "0")
                     + " align=" + juce::String (mr.alignOffset)
                     + " winLen=" + juce::String ((int) window.size())
                     + " didGrade=" + juce::String (didGrade ? "1" : "0"));
        if (! didGrade) { mc->gradeInFlight_.store (false); return; }   // no stable match -> release + done

        const int   refLen      = (int) refCopy.size();
        const int   alignOffset = mr.alignOffset;      // where matchAlign located the sweep (gate + grader agree)
        const float coherence   = mr.coherence;        // captured for the ratification log line
        // SP3: snapshot THIS ear's current D1 baseline (message thread — the engine baseline's single writer)
        // so the worker can compute drift against a COPY without touching engine state off-thread. Empty (invalid)
        // until the first GradedClean has set it; the worker returns setBaseline when it should be learned.
        eb::BandCurve baselineCopy;
        if (const eb::BandCurve* b = mc->engine.shapeBaseline (ear)) baselineCopy = *b;
        mc->firPool->addJob ([safe, ear, refCopy = std::move (refCopy), window = std::move (window), rate, refLen, alignOffset, coherence, myGen, baselineCopy = std::move (baselineCopy)]() mutable {
        // OFFLINE on the worker: the pure grade for the window decide() said to grade. gradeWindow() grades at the
        // SAME offset decide() located (Fix 1 — gate and grade agree; no second xcorr), re-runs the match-gate
        // FIRST there, then quality — a non-sweep segment fails the gate -> stale.
        eb::GradeArtifacts art;   // SP3: the graded IR + reference band + aligned-segment offset/len
        const auto g = eb::ReferenceGradePoller::gradeWindow (window.data(), (int) window.size(),
                                                              refCopy.data(), refLen, rate, alignOffset, &art);
        const int   state    = (int) g.state;
        const bool  mismatch = g.mismatch;
        const float irSnr    = g.irSnrDb;
        const float thd      = g.thdPercent;
        const bool  lowQ     = g.lowQuality;
        // Sweep-to-room-noise SNR computed from the SAME match-aligned window (off-thread, here on the worker).
        // snrValid is false when neither a leading nor a trailing noise region is usable -> we must NOT flag.
        const float sweepSnr = g.sweepSnrDb;
        const bool  snrValid = g.sweepSnrValid;
        // Gain-staging readout: the RAW input sample peak over the aligned sweep region (dBFS, NOT clamped at 0 so
        // an overshoot reads positive). Always published (unlike the SNR, it has no valid/invalid gate — a peak is
        // always measurable); -120 means a silent span. The status line derives the clip-correction guidance from it.
        const float sweepPeak = g.sweepPeakDb;
        const int snrBand = (int) g.sweepSnrBand;   // 3-color quality bands (sweepSNR gates; THD/IR-SNR advisory)
        const int irBand  = (int) g.irSnrBand;
        const int thdBand = (int) g.thdBand;
        // SP3 (INFO-ONLY): run the seven shape detectors on the SAME IR/band the grade keyed off — but ONLY on a
        // MATCHED graded outcome (GradedClean/Marginal/Suspect). A non-graded state (ReferenceStale) publishes
        // NOTHING: no verdict, no numbers, no baseline (spec §6 honesty). The detectors are heavy FFTs, so they
        // run here on the worker; the (cheap) cross-ear polarity + the publish happen on the message thread below.
        const auto rs = (eb::RefMonState) state;
        const bool graded = (rs == eb::RefMonState::GradedClean || rs == eb::RefMonState::GradedMarginal
                             || rs == eb::RefMonState::GradedSuspect);
        ShapeResult shape;
        if (graded)
            shape = runShapeDetectors (ear, /*gradedClean*/ rs == eb::RefMonState::GradedClean,
                                       baselineCopy.valid ? &baselineCopy : nullptr,
                                       art.ir, art.bandLoHz, art.bandHiHz,
                                       window, art.windowStart, art.gradedLen, refCopy, rate);
        juce::MessageManager::callAsync ([safe, ear, state, irSnr, thd, mismatch, lowQ, sweepSnr, snrValid, sweepPeak,
                                          snrBand, irBand, thdBand, coherence, alignOffset, refLen, rate, myGen,
                                          graded, shape = std::move (shape)]() mutable {
            auto* mc = safe.getComponent();
            if (! mc) return;
            // Guard-then-publish-then-stamp, extracted (publishGradeIfRunCurrent) so the ORDER is
            // test-pinned: a stale run generation publishes NOTHING and stamps NOTHING (#4 + §3.2).
            if (! mc->publishGradeIfRunCurrent (ear, state, irSnr, thd, mismatch, lowQ, myGen))
                return;   // a Stop/Start happened since post -> drop this stale publish whole (#4)
            {   // Anti-flicker bands for the VerdictCard (the retired dots' smoother, re-pointed at
                // the card): smooth THIS ear's bands across consecutive grades and store them; the
                // card model reads the store (refreshMeasureView). The raw dB/% renders beside each
                // band-toned value on the card (never colour alone).
                auto& sm = (ear == 1) ? mc->smootherR_ : mc->smootherL_;
                auto& bs = (ear == 1) ? mc->smoothedBandsR_ : mc->smoothedBandsL_;
                bs = sm.update (sweepSnr, snrValid, irSnr, thd);
            }
            mc->userRearmed_ = false;   // new evidence arrived: the verdict view returns (§2 re-arm clears)
            // Match-window sweep-SNR fix: publish THIS ear's sweep SNR and raise the GUIDANCE LowSnr flag when the
            // sweep ran too close to the room floor. Match-gate already passed (we only get here when g.didGrade),
            // so a non-sweep / silent ear never reaches this -> a silent ear NEVER raises LowSnr. GUIDANCE only.
            // The predicate lives in gui/GradeGuards.h (#32) so the tests assert the PRODUCTION condition.
            if (snrValid) {
                mc->engine.publishCompletedSweepSnrDb (ear, sweepSnr);
                if (eb::wouldRaiseLowSnr (snrValid, sweepSnr))
                    mc->engine.raiseLowSnr();
            }
            // Gain-staging readout: publish THIS ear's RAW input sweep peak (dBFS, clip reads positive) so the
            // per-ear status line can quantify the overshoot and suggest how much to lower the output. Always
            // published (no valid/invalid gate); GUIDANCE only — it does NOT invalidate the grade.
            mc->engine.publishCompletedSweepPeakDb (ear, sweepPeak);

            // SP3 (INFO-ONLY): finalize + publish the shape anomalies for this ear. Only MATCHED grades ran the
            // detectors (graded==true); a non-graded outcome publishes NOTHING here (no verdict, no numbers).
            if (graded) {
                const int e = (ear == 1) ? 1 : 0, other = 1 - e;
                // The FIRST GradedClean per ear learns the D1 baseline (message thread == the single writer).
                if (shape.setBaseline && shape.newBaseline.valid)
                    mc->engine.setShapeBaseline (ear, shape.newBaseline);
                // D4 cross-ear polarity: stash THIS ear's fresh peak segment (stamped with the run gen), then run
                // the L-vs-R cross-correlation ONLY when BOTH ears carry a fresh, same-run segment (both matched).
                if ((int) shape.peakSeg.size() == kShapePolaritySeg) {
                    mc->shapePeakSegPerEar_[e]    = std::move (shape.peakSeg);
                    mc->shapePeakSegGenPerEar_[e] = myGen;
                    mc->shapePeakSegFresh_[e]     = true;
                }
                if (mc->shapePeakSegFresh_[0] && mc->shapePeakSegFresh_[1]
                    && mc->shapePeakSegGenPerEar_[0] == myGen && mc->shapePeakSegGenPerEar_[1] == myGen
                    && (int) mc->shapePeakSegPerEar_[0].size() == kShapePolaritySeg
                    && (int) mc->shapePeakSegPerEar_[1].size() == kShapePolaritySeg) {
                    const eb::PolarityReport pol = eb::crossEarPolarity (
                        mc->shapePeakSegPerEar_[0].data(), kShapePolaritySeg,
                        mc->shapePeakSegPerEar_[1].data(), kShapePolaritySeg);
                    if (pol.valid && pol.inverted) {
                        // A cross-ear inversion is a property of the PAIR, so BOTH ears carry it. THIS ear
                        // gets the bit in its own flags (published just below); the OTHER ear was already
                        // published on its grade tick, so OR the bit straight into its published flags
                        // (message thread == single writer, so no store races the publish).
                        shape.flags |= eb::ShapeFlag::kPolarity;
                        mc->engine.raiseShapeFlag (other, eb::ShapeFlag::kPolarity);
                    }
                }
                mc->engine.publishShapeAnomalies (ear, shape.flags, shape.driftMaxDb, shape.hfShelfDb,
                                                  shape.combDepthDb, shape.combDelayMs, shape.effLoHz,
                                                  shape.effHiHz, shape.lobeWidth, shape.stepDb,
                                                  shape.humBaseHz, shape.resonanceHz);
                if (shape.flags & ~eb::ShapeFlag::kBaselineSet)
                    mc->logLine (eb::DiagnosticLog::Level::Info,
                        juce::String ("SHAPE: ear=") + (ear == 1 ? "R" : "L")
                        + " flags=0x" + juce::String::toHexString ((int) shape.flags)
                        + " drift=" + juce::String (shape.driftMaxDb, 1) + "dB"
                        + " hfShelf=" + juce::String (shape.hfShelfDb, 1) + "dB"
                        + " comb=" + juce::String (shape.combDepthDb, 1) + "dB@" + juce::String (shape.combDelayMs, 1) + "ms"
                        + " band=" + juce::String (shape.effLoHz, 0) + ".." + juce::String (shape.effHiHz, 0) + "Hz"
                        + " lobe=" + juce::String (shape.lobeWidth, 1)
                        + " step=" + juce::String (shape.stepDb, 1) + "dB"
                        + " hum=" + juce::String (shape.humBaseHz) + "Hz"
                        + " reson=" + juce::String (shape.resonanceHz, 0) + "Hz");
            }
            // Ratification-grade summary: ONE comprehensive line PER EAR per graded sweep so a single real
            // measurement carries everything needed to set the IR-SNR / THD / sweep-SNR / match-coherence cutoffs
            // and flip kIrThresholdsRatified. Info level, message-thread-only, serial-scrubbed by logLine.
            // config48k correlates the grade with the live 48k-everywhere chain verdict; ear names which earcup
            // this verdict is for (each ear graded against its OWN reference channel).
            mc->logLine (eb::DiagnosticLog::Level::Info,
                juce::String ("GRADE COMPLETE: ear=") + (ear == 1 ? "R" : "L")
                + " state=" + refMonStateName (state)
                + " bands=snr:" + juce::String::charToString (bandChar (snrBand))
                              + "/ir:" + juce::String::charToString (bandChar (irBand))
                              + "/thd:" + juce::String::charToString (bandChar (thdBand))
                + " IR-SNR=" + juce::String (irSnr, 1) + "dB"
                + " THD=" + juce::String (thd, 2) + "%"
                + " sweepSNR=" + (snrValid ? juce::String (sweepSnr, 1) + "dB" : juce::String ("n/a"))
                + " sweepPeak=" + (sweepPeak >= 0.0f ? juce::String ("+") : juce::String())
                                + juce::String (sweepPeak, 1) + "dBFS"
                + " coherence=" + juce::String (coherence, 3)
                + " alignOffset=" + juce::String (alignOffset)
                + " refLen=" + juce::String (refLen)
                + " rate=" + juce::String (rate, 0)
                + " config48k=" + juce::String (mc->chainVerdict_.checked
                                                ? (mc->chainVerdict_.all48k ? "yes" : "NO") : "unknown"));
            // P3 Task 7: immediate transport-face + wizard/Measure re-render from the NEW truth.
            // AFTER every publish above (grade, SNR, peak, shape) - the brief placed this beside the
            // band store, but that would compose the card from a half-published verdict for one
            // render; here the recompose reads the complete evidence. (The 30 Hz tick would catch
            // up anyway; this makes the flip immediate.)
            mc->updateStartGate();
            mc->gradeInFlight_.store (false);
        });
    });
      });   // close the message-thread callAsync (setLastMatchCoherence + debounce + grade dispatch)
    });     // close the off-thread matchAlign job
    return true;
}

void MainComponent::updateActiveEarIndicator (bool silent) {
    // In AutoPerEar mode, EARS Bridge feeds Dirac only the earcup Dirac is currently sweeping. Show
    // which side that is, so the user can confirm the bridge is following Dirac's left-then-right
    // sweep correctly: accent the live meter and replace the gain caption with a "capturing L/R" line.
    const bool running  = engine.status() == EngineStatus::Running;
    const bool autoMode = settings.combineMode() == CombineMode::AutoPerEar;
    const bool live     = running && autoMode && ! silent;

    const int ear = live ? engine.autoActiveEar() : -1;   // 0 = left, 1 = right, -1 = none
    meterL.setActive (ear == 0);
    meterR.setActive (ear == 1);
    measureStage_.setActiveEar (ear);   // mirror the accent on Measure's own live-meters strip (§5.4)

    juce::String text;
    juce::Colour col;
    if (running && autoMode) {
        if (live) {
            text = (ear == 0) ? "Auto per-ear" + kDash + "capturing the LEFT earcup"
                              : "Auto per-ear" + kDash + "capturing the RIGHT earcup";
            if (engine.autoEarAmbiguous()) {                 // the schedule router flagged this segment OFF-schedule
                text += kDash + "routing ambiguous, re-measure";
                col   = Theme::warn();
            } else {
                col   = Theme::text();   // readable primary text; the meter's accent dot carries the colour cue
            }
        } else {
            text = "Auto per-ear" + kDash + "waiting for the next sweep" + kEllipsis;
            col  = Theme::textDim();
        }
    } else {
        text = "Set Dirac's Master output so the L and R meters reach the green band.";
        col  = Theme::textDim();
    }
    // setColour no-ops when unchanged; setText only on a real change -> no per-tick repaint churn. The
    // timer is the sole owner of levelsHint's text+colour (so it is not touched in applyTextColours).
    levelsHint.setColour (juce::Label::textColourId, col);
    if (text != levelsHint.getText())
        levelsHint.setText (text, juce::dontSendNotification);
}

void MainComponent::pollChainConfig() {
    // 48k-everywhere chain-config check. Throttled to ~1 s (the GUI timer is 30 Hz). Windows fires NO
    // device-list event for a same-device format change (44.1k->48k) and Dirac's output rate has no event
    // at all, so we POLL the three endpoint mix formats on the timer (pre-Start too). Each read is a pure
    // EndpointFormat read (or "" off-Windows -> valid=false -> "couldn't read", never a false pass).
    if (++chainPollTick_ < kChainPollTicks) return;
    chainPollTick_ = 0;

    eb::ChainConfig cfg;
    // #28: key the reads on the STABLE endpoint UID when DeviceManager resolved one (uid != name) —
    // readEndpointFormat's exact-UID tier then hits the precise endpoint even when Windows exposes
    // duplicate friendly names ("Name" vs "Name (2)"). Unresolved uid falls back to the name tiers.
    if (const auto in = inputPicker.selectedDevice())
        cfg.input = eb::readEndpointFormat (in->uid.isNotEmpty() ? in->uid : in->name, /*isInput*/ true);
    if (const auto out = outputPicker.selectedDevice())
        cfg.cable = eb::readEndpointFormat (out->uid.isNotEmpty() ? out->uid : out->name, /*isInput*/ false);
    const auto diracName = eb::readDiracOutputDeviceName();   // hoisted: reused by the #34 check below
    if (diracName.isNotEmpty())
        cfg.diracOutput = eb::readEndpointFormat (diracName, /*isInput*/ false);   // a NAME from Dirac's settings file

    // #34 (P3 wait-hint input): does Dirac's CURRENT output match the endpoint the reference was
    // learned from? Reuses this poll's name read - no extra COM traffic on the timer.
    if (loadedReferenceEndpoint_.isNotEmpty()) {
        if (diracName.isNotEmpty()) {
            auto curId = eb::endpointUidForName (diracName, /*isInput*/ false);
            if (curId.isEmpty()) curId = diracName;
            refEndpointMismatch_ = curId != loadedReferenceEndpoint_ && diracName != loadedReferenceEndpoint_;
        } else refEndpointMismatch_ = false;
    } else refEndpointMismatch_ = false;

    chainVerdict_ = eb::checkChainConfig (cfg);
    chainFormats_ = cfg;   // keep the per-endpoint formats so the Signal-chain panel can render rate/channels/bits

    // Log ON CHANGE only. The change key folds in the RATE summary AND the secondary advisory (channels +
    // bit-depth), so an advisory-only change (e.g. Dirac output flips to 16-bit while the rate stays 48k)
    // still logs + refreshes the idle status line. "all-48k" / "unchecked" both summarise as "" for the rate
    // part. Include each endpoint's rate/channels/bits so the failing format is in the log, not just the verdict.
    const juce::String key = (chainVerdict_.checked
        ? (chainVerdict_.all48k ? juce::String ("ok") : chainVerdict_.summary)
        : juce::String ("unchecked")) + "|adv=" + chainVerdict_.advisory;
    if (key != lastChainSummaryLogged_) {
        auto fmtStr = [] (const eb::EndpointFormat& f) -> juce::String {
            if (! f.valid) return "<unreadable>";
            return juce::String (f.mixRateHz, 0) + "Hz/" + juce::String (f.channels) + "ch/"
                 + juce::String (f.bits) + "bit" + (f.isFloat ? "f" : "");
        };
        logLine (chainVerdict_.checked && ! chainVerdict_.all48k
                     ? eb::DiagnosticLog::Level::Info     // a real mismatch worth surfacing
                     : eb::DiagnosticLog::Level::Debug,   // ok / unchecked: quieter
                 juce::String ("Chain config (48k gate): ")
               + (chainVerdict_.checked ? (chainVerdict_.all48k ? "all 48k" : ("WARN " + chainVerdict_.summary))
                                        : "unchecked (no endpoint read)")
               + (chainVerdict_.advisory.isNotEmpty() ? (" | advisory: " + chainVerdict_.advisory) : juce::String())
               + " | input=" + fmtStr (cfg.input)
               + " cable=" + fmtStr (cfg.cable)
               + " diracOutput=" + fmtStr (cfg.diracOutput));
        lastChainSummaryLogged_ = key;
        // The pre-Start status line only refreshes on events (updateStartGate), so a chain change while
        // idle (e.g. Dirac's output rate switched) wouldn't otherwise surface. Refresh it on a verdict
        // change. When Running, timerCallback already refreshes the line every tick.
        if (engine.status() != EngineStatus::Running) updateStatusLine();
    }
}

void MainComponent::timerCallback() {
   #if JUCE_DEBUG || defined (EB_ENABLE_HIG_HARNESS)
    {   // HIG STATE-SWEEP HARNESS (dev-QA, inert unless EB_HIG_STATES=<dir> is set): drive the header through every
        // status state it can display and emit a native-render descriptor per state, so native-review can MEASURE
        // overlap/clip in states a single idle screenshot/probe never captures (e.g. the mid-sweep status next to
        // Start). Runs once after the window is shown. The next tick's normal status update restores the live text.
        // COMPILE-GATED out of shipped Release builds (audit #23: a user with the env var set would get fake UI
        // state - a phantom update link + fabricated dots). Dev builds opt in: cmake -DEB_ENABLE_HIG_HARNESS=ON.
        static bool higStatesDone = false;
        if (! higStatesDone && isShowing() && getWidth() > 0) {
            higStatesDone = true;   // latch on the FIRST shown tick: exactly one env read ever (audit #61 - was a
                                    // Win32 call + String alloc at 30 Hz for the app's lifetime)
            const auto outDir = juce::SystemStats::getEnvironmentVariable ("EB_HIG_STATES", {});
            if (outDir.isNotEmpty()) {
            const juce::File dir (outDir);
            // PNG-append workaround (T7 found-in-passing; the standalone chip owns the real writer fix):
            // writeSnapshot's FileOutputStream APPENDS to an existing file, so a harness re-run into the
            // same dir keeps every PNG's STALE first frame. Wipe the target PNG before each write.
            const auto probeScene = [this, &dir] (const juce::String& stem) {
                const auto png = dir.getChildFile (stem + ".png");
                png.deleteFile();
                hig::writeDesignProbe (*getTopLevelComponent(),
                                       dir.getChildFile (stem + ".json"), png);
            };
            // Each scene = a (step, state) pair (spec §8 item 4, P3 Task 8 rework): the Measure scenes
            // drive REAL state through the SAME seams the gates use (driveVerdictForTest /
            // setCaptureModels / setLead / setWaitHint / driveDeviceErrorForTest - production models and
            // machine state, never label injection); the stage scenes pin Connect/Calibrate/Level at
            // their natural (or forced-disclosure / warned / clip-warned) state. forceWizardStepForTest
            // (per-scene) settles the shown stage; p3 selects the driven P3 state (codes below).
            // Frame count: 24 scenes x 4 appearances + 2 startready = 98 frames (P3 Task 8: the scene
            // matrix owns capturing/midcapture-failed, so the Task-6 standalone capture-card block died;
            // verdict-suspect rides beyond the plan's 20-row table - see the task report).
            struct St { const char* name; juce::String statusText; bool update; WizardStep step;
                        bool calAdv; bool diracHint; int p3; };
            // p3 codes: 0 none | 1 armed-waiting | 2 timeout-hint | 3 capturing | 4 midcapture-failed
            //           5 verdict | 6 verdict-details | 7 verdict-stale | 8 hwdirac | 9 override
            //           10 level-clip | 11 regression-banner | 12 verdict-suspect
            //           13 measure-refneeded | 14 calibrate-empty | 15 calibrate-unity (T8)
            static const St states[] = {
                { "idle",             "",                                                          false, WizardStep::Measure, false, false, 0 },
                { "running",          "Running" + kDash + "waiting for the Dirac sweep" + kEllipsis,       false, WizardStep::Measure, false, false, 1 },
                { "lowlevel",         "Running" + kDash + "level low: turn your amp up to the green band", false, WizardStep::Measure, false, false, 1 },
                { "error",            "EARS or audio cable disconnected" + kDash + "measurement stopped.", false, WizardStep::Measure, false, false, 0 },
                { "update+armed",     "Listening for Dirac's sweep" + kEllipsis,                           true,  WizardStep::Measure, false, false, 1 },
                { "timeout-hint",     "Listening for Dirac's sweep" + kEllipsis,                           false, WizardStep::Measure, false, false, 2 },
                { "capturing",        "Sweep in progress" + kEllipsis,                                     false, WizardStep::Measure, false, false, 3 },
                { "midcapture-failed","EARS or audio cable disconnected" + kDash + "measurement stopped.", false, WizardStep::Measure, false, false, 4 },
                { "verdict",          "",                                                          false, WizardStep::Measure, false, false, 5 },
                { "verdict-details",  "",                                                          false, WizardStep::Measure, false, false, 6 },
                { "verdict-stale",    "",                                                          false, WizardStep::Measure, false, false, 7 },
                { "verdict-suspect",  "",                                                          false, WizardStep::Measure, false, false, 12 },
                { "hwdirac",          "",                                                          false, WizardStep::Measure, false, false, 8 },
                { "override",         "Listening for the sweep" + kEllipsis,                               false, WizardStep::Measure, false, false, 9 },
                { "measure-refneeded","",                                                          false, WizardStep::Measure, false, false, 13 },
                // STAGE SCENES: one per non-Measure step at its current state (the gate MEASURES each
                // hand-laid stage body - the "step axis actually probes 4 stages" honesty check).
                // connect-dirachint forces the standard-cable/Dirac warning + one-click fix (T10);
                // calibrate-advanced forces the Advanced-FIR disclosure OPEN (P2 Task 8); level-clip
                // forces the input-clip warning through the same production-copy seam the Level gate uses.
                { "connect",          "", false, WizardStep::Connect,   false, false, 0 },
                { "connect-dirachint","", false, WizardStep::Connect,   false, true,  0 },
                { "calibrate",        "", false, WizardStep::Calibrate, false, false, 0 },
                { "calibrate-advanced","",false, WizardStep::Calibrate, true,  false, 0 },
                { "calibrate-empty",  "", false, WizardStep::Calibrate, false, false, 14 },
                { "calibrate-unity",  "", false, WizardStep::Calibrate, false, false, 15 },
                { "level",            "", false, WizardStep::Level,     false, false, 0 },
                { "level-clip",       "", false, WizardStep::Level,     false, false, 10 },
                { "regression-banner","", false, WizardStep::Measure,   false, false, 11 },
            };
            // APPEARANCE MATRIX (EARS-vendored extension, 2026-07-04): the base sweep only rendered the
            // OS launch mode at normal contrast, but the composited-pixel HIG checks are mode/contrast
            // specific - primary-CTA label contrast (dark), control-boundary 3:1 (light), meter warn-zone
            // (light), and the increaseContrast() palette strengthening. So drive every status state
            // through {dark,light} x {normal, increase-contrast} and emit one descriptor+PNG per cell.
            // Dev-QA only (compile-gated). forceThemeForTest reapplies the palette + settles labels;
            // SystemA11y::setForTest forces the a11y flags Theme reads.
            struct Ap { const char* tag; bool dark; bool hc; };
            static const Ap appears[] = {
                { "dark-normal",   true,  false }, { "light-normal",   false, false },
                { "dark-contrast", true,  true  }, { "light-contrast", false, true  },
            };
            const bool wasDark = eb::Theme::dark();
            // The driven status lines + quality dots live in MeasureStage (the wizard cutover moved them off
            // the title bar), so each scene now pins ITS OWN step (s.step) inside the loop via
            // forceWizardStepForTest — the Measure scenes surface the driven labels; the stage scenes render
            // Connect/Calibrate/Level at their natural state. The pin also settles the shown stage + resizes.
            const auto pinnedWas = pinnedStep_;
            for (auto& ap : appears) {
                eb::SystemA11y::setForTest (false, ap.hc, ap.hc);   // reduceMotion off; contrast+transparency = hc
                forceThemeForTest (ap.dark);                        // reapply palette + settle the non-owned labels
                for (auto& s : states) {
                    // ORDER IS THE TASK-6 HARNESS LESSON (see driveVerdictForTest): machine STATE first
                    // (deviceError feeds computeWizardState -> the banner), then the PIN - whose
                    // renderWizardView -> refreshMeasureView recomposes every Measure feed from LIVE
                    // truth - and only THEN the driven display feeds. The plan's Task-8 snippet pinned
                    // LAST, which would have recomposed-away every driven P3 state and rendered 21
                    // identical live-truth frames (flagged in the task report; fixed here).
                    driveDeviceErrorForTest (s.p3 == 11 ? "Device error" + kDash + "check the EARS and cable"
                                                        : juce::String());
                    calibrateStage_.setAdvancedOpen (s.calAdv);     // P2: force the Advanced-FIR disclosure per scene
                    driveConnectWarningsForTest (s.diracHint, s.diracHint);   // T10: the cable-warning scene
                    // T8: the calibrate-empty/unity scenes render the FIRST-RUN empty state - clear both
                    // slots via the same route removeBtn.onClick drives. Every OTHER scene re-seeds the
                    // launch pair if a prior empty scene cleared it: the matrix loops appearances, so a
                    // cleared pair would otherwise contaminate every later frame (incl. the next pass).
                    if (s.p3 == 14 || s.p3 == 15) {
                        if (leftCal.hasCal())  leftCal.clearCal();
                        if (rightCal.hasCal()) rightCal.clearCal();
                    } else {
                        if (! leftCal.hasCal()  && settings.leftCalPath().isNotEmpty())
                            leftCal.loadFromFile (juce::File (settings.leftCalPath()));
                        if (! rightCal.hasCal() && settings.rightCalPath().isNotEmpty())
                            rightCal.loadFromFile (juce::File (settings.rightCalPath()));
                    }
                    setUnityAcceptedForTest (s.p3 == 15);   // accepted wording only for calibrate-unity (set AFTER any loads - loading resets the flag)
                    forceWizardStepForTest (s.step);                // pin + render + show this scene's stage, then resize
                    statusLine.setText (s.statusText, juce::dontSendNotification);
                    if (s.update) updateLink.setButtonText ("Update available");
                    updateLink.setVisible (s.update);
                    using Lead = MeasureStage::Lead;  using CCM = eb::CaptureCardModel;
                    // The DRIVEN Measure scenes (p3 1-9, 12) force the armed/hw workflow states that are
                    // unreachable in a cold harness run (they need a Running engine + a learned reference).
                    // p3 0/10/11 scenes render the pinned stage at LIVE truth - refreshMeasureView already
                    // recomposed the Measure feeds, and the banner scene's displacement IS that truth.
                    if ((s.p3 >= 1 && s.p3 <= 9) || s.p3 == 12 || s.p3 == 13) {
                        // Verdict models through the SAME pure composer the live feed uses (production
                        // path, never label injection): clean LEFT + marginal RIGHT with a flagged Drift
                        // chip; the stale scene sets the models' own stale bit; the suspect scene pairs
                        // the danger-toned card with a clean sibling (scene variety, per the #68 gate).
                        eb::EarGradeSnapshot ce; ce.state = (int) eb::RefMonState::GradedClean;
                        ce.sweepSnrDb = 30; ce.irSnrDb = 56; ce.thdPercent = 0.8f; ce.peakDb = -8;
                        eb::EarGradeSnapshot me = ce; me.state = (int) eb::RefMonState::GradedMarginal;
                        me.sweepSnrDb = 18; me.irSnrDb = 52; me.thdPercent = 0.3f;
                        eb::EarGradeSnapshot se = ce; se.state = (int) eb::RefMonState::GradedSuspect;
                        se.sweepSnrDb = 8; se.irSnrDb = 20; se.thdPercent = 6.0f;
                        eb::ShapeScalars ds; ds.driftMaxDb = -9.0f;
                        const auto cleanM = eb::verdictCardModelAuto ("LEFT EAR",  ce, 0u, {}, s.p3 == 7, false);
                        const auto margM  = eb::verdictCardModelAuto ("RIGHT EAR", me, eb::ShapeFlag::kDrift, ds,
                                                                      s.p3 == 7, false);
                        const auto suspM  = eb::verdictCardModelAuto ("LEFT EAR",  se, 0u, {}, false, false);
                        const auto cleanR = eb::verdictCardModelAuto ("RIGHT EAR", ce, 0u, {}, false, false);
                        const Lead lead = s.p3 == 8  ? Lead::HwDirac
                                        : s.p3 == 13 ? Lead::Reference : Lead::Waiting;
                        measureStage_.setLead (lead);
                        // The scene's HONEST head copy (the plan snippet drove it only for the override
                        // scene, leaving hwdirac/armed scenes with the Reference-lead head over a
                        // Waiting/HwDirac lead - a state the app can never show; flagged + fixed): the
                        // same pure rule refreshMeasureView applies. Canonical AUTO scenes (mode copy
                        // variants are pinned headless in test_wizardnav.cpp). The verdict scenes'
                        // driveVerdictForTest below re-applies its own verdict-showing head on top.
                        const auto head = MeasureStage::measureHeadCopy (lead, /*overrideOn*/ s.p3 == 9,
                                                                         /*verdictShowing*/ false,
                                                                         eb::CombineMode::AutoPerEar);
                        measureStage_.setHeadCopy (head.title, head.sub);
                        measureStage_.setWaitHint (s.p3 == 2
                            ? MeasureStage::waitingHint (MeasureStage::kArmedNoSweepHintSeconds,
                                                         false, false, {}, false)
                            : juce::String());
                        // T8 review note: for the verdict scenes (p3 5/6/7/12) this outer write is
                        // DEAD - driveVerdictForTest below re-pins Measure, and its renderWizardView
                        // -> refreshMeasureView recomposes the capture feed from live truth (the
                        // driven verdict models then hide the capture cards entirely). Kept
                        // unconditional for the non-verdict scenes' symmetry: one feed order for
                        // every driven scene.
                        measureStage_.setCaptureModels (
                            s.p3 == 3 ? CCM::capturing ("LEFT EAR", 0.58f, 10)
                          : s.p3 == 4 ? CCM::failed ("LEFT EAR")
                                      : CCM::waiting ("LEFT EAR", eb::CombineMode::AutoPerEar),
                            CCM::waiting ("RIGHT EAR", eb::CombineMode::AutoPerEar));
                        if (s.p3 == 5 || s.p3 == 6 || s.p3 == 7)
                            driveVerdictForTest (cleanM, margM, s.p3 == 6);   // re-pins Measure (its own order rule)
                        else if (s.p3 == 12)
                            driveVerdictForTest (suspM, cleanR, false);
                        else
                            measureStage_.setVerdictModels ({}, {}, false);
                    }
                    driveLevelClipForTest (s.p3 == 10);   // Level clip warning (production copy seam); clears itself
                    resized();                            // settle the direct stage feeds before the probe
                    probeScene (juce::String ("hig-") + ap.tag + "-" + s.name);
                }
            }
            // The matrix's LAST scene (regression-banner) leaves the device error latched; clear it
            // BEFORE the startready frames or they render banner-displaced Level stages (the plan put
            // this restore after the startready section - the leak is flagged in the task report).
            driveDeviceErrorForTest ({});
            // COMPONENT-STATE SCENE: the status sweep above leaves Start DISABLED (its accent fill paints
            // only when enabled), so force it enabled and capture the ready-CTA scene in dark+light. (The
            // former "Advanced expanded" scene died with the disclosure - its children live in the stages,
            // captured by the per-step gate axis instead.) The matrix loop's last scene (regression-banner)
            // left the pin on Measure; force LEVEL - the transport's home since the P3 Task 4 re-home -
            // so the ready CTA scene renders the stage that actually hosts it. The force must run AFTER
            // each theme flip (matching the matrix loop's ordering): forceThemeForTest re-renders through
            // the LIVE path, where a Blocked Level pin falls back to first-unmet and the scene would
            // capture the wrong stage (the latent pre-Task-4 behaviour, harmless while the bar hosted Start).
            // (The Task-6 standalone capture-card scenes died here: capturing/midcapture-failed now ride
            // the full appearance matrix above.)
            for (const bool dk : { true, false }) {
                forceThemeForTest (dk);
                forceWizardStepForTest (WizardStep::Level);
                const juce::String mtag = dk ? "dark" : "light";
                startStop.setEnabled (true);                                    // enabled primary CTA (M3)
                resized();
                probeScene ("hig-" + mtag + "-normal-startready");
            }

            eb::SystemA11y::setForTest (false, false, false);       // restore; the next theme tick re-reads the OS
            forceThemeForTest (wasDark);
            // P2 (Task 8): the calibrate-advanced scene forced the disclosure open; restore the launch policy
            // (open iff any advanced FIR setting is non-default) so the live UI reflects the real settings.
            // Same pure helper as the launch seed (P2.9 T7) - one rule, no duplicated expression to drift.
            calibrateStage_.setAdvancedOpen (CalibrateStage::advancedFirNonDefault (
                settings.complexPhase(), settings.firLength(), settings.outputTrimDb()));
            driveConnectWarningsForTest (false, false);   // T10: clear the forced cable-warning scene
            // T8: the calibrate-empty/unity scenes cleared the slot pair + set the unity flag; restore
            // the launch seeding (same guards as startup) and the un-accepted default.
            setUnityAcceptedForTest (false);
            if (! leftCal.hasCal() && settings.leftCalPath().isNotEmpty())
                leftCal.loadFromFile (juce::File (settings.leftCalPath()));
            if (! rightCal.hasCal() && settings.rightCalPath().isNotEmpty())
                rightCal.loadFromFile (juce::File (settings.rightCalPath()));
            // P3 (Task 8) restores - belt-and-braces: the final refreshWizardView recomposes every
            // Measure feed from live truth anyway, but leave NO forced scene state behind on any path.
            driveLevelClipForTest (false);
            driveDeviceErrorForTest ({});
            measureStage_.setVerdictModels ({}, {}, false);
            measureStage_.setWaitHint ({});
            measureStage_.setCaptureModels (eb::CaptureCardModel::waiting ("LEFT EAR",  settings.combineMode()),
                                            eb::CaptureCardModel::waiting ("RIGHT EAR", settings.combineMode()));
            pinnedStep_ = pinnedWas;                                // restore the pre-sweep navigation pin
            refreshWizardView();
            }   // if (outDir.isNotEmpty())
        }
    }
   #endif // JUCE_DEBUG || EB_ENABLE_HIG_HARNESS
    // A device removed mid-run (unplug / sleep / gain-DIP re-enumerate) latches deviceDied_ from its
    // audioDeviceStopped() callback. Tear it down and surface it, instead of leaving the engine sitting
    // in a false "Running - clean" while the cable records silence.
    if (engine.status() == EngineStatus::Running && engine.consumeDeviceDied()) {
        statusErrorMsg_ = "EARS or audio cable disconnected" + kDash + "measurement stopped.";
        logLine (eb::DiagnosticLog::Level::Error, "Device lost mid-run: " + statusErrorMsg_);
        // §3.1 mid-capture regression: the Measure stage OWNS the moment - the active capture converts
        // to an explicit Failed card (sticky until the next Start), never a silent "Listening..." fallback.
        if (captureEar_ >= 0) {
            failedCaptureEar_ = captureEar_;
            logLine (eb::DiagnosticLog::Level::Error,
                     juce::String ("Capture interrupted mid-sweep (ear=") + (captureEar_ == 1 ? "R" : "L") + ")");
        }
        engine.onDeviceLost();              // closes devices, flips status -> Error
        // Mirror the Stop path's grade teardown (audit #42): a matchAlign/grade job in flight for the
        // just-captured sweep must not land its verdict + green quality dots INTO the Error state.
        gradeRunGen_.fetch_add (1);
        gradeInFlight_.store (false);
        gradePollerL_.reset(); gradePollerR_.reset();
        // (P3 Task 4: device-lost resets to the Start face in syncTransport, via updateStartGate below.)
        // #3 heal (same strand as the Stop path): a mid-run FIR change whose build no-oped on the reconfig
        // gate must be re-posted now the engine is out of Running, or the Start gate stays closed forever.
        if (engine.requestedGeneration() != engine.builtGeneration())
            rebuildFirsAsync();
        updateStartGate();                  // -> updateStatusLine() renders statusErrorMsg_ in the Error branch
        return;
    }
    const auto lv = engine.levels();
    hwMicRunPeak_ = juce::jmax (hwMicRunPeak_, lv.inL, lv.inR);   // #16: unconditional run peak for the hw-Dirac detect
    meterL.setLevel  (lv.inL,     lv.clipL);
    meterR.setLevel  (lv.inR,     lv.clipR);
    meterOut.setLevel (lv.outMono, lv.clipOut);
    // Level SOFT-gate latch (§3.2): the moment BOTH input channels reach the meters' green target band
    // ([-18,-12] dBFS) while Running, latch it — the user set a good level once this session. It survives
    // Stop (the knob didn't move) and is reset only on an input-device change (onInputChosen). Read the
    // SAME linear values the meters render, converted with the meters' own band constants (single source).
    if (engine.status() == EngineStatus::Running && ! levelLatched_) {
        const auto inBand = [] (float lin) {
            const float db = (lin <= 1.0e-6f) ? -120.0f : 20.0f * std::log10 (lin);
            return db >= LevelMeter::kTargetLoDb && db <= LevelMeter::kTargetHiDb;
        };
        if (inBand (lv.inL) && inBand (lv.inR))
            levelLatched_ = true;
    }
    // §2 timeout-hint clock: armed = Running + reference loaded + nothing graded yet + no sweep engaged.
    // Resets the instant a sweep engages or a grade lands; kArmedNoSweepHintSeconds gates the hint copy.
    {
        const bool anyGraded = eb::earIsGraded (engine.refMonState (0)) || eb::earIsGraded (engine.refMonState (1));
        const bool sweepEngaged = sweepActiveTicks_ >= kSweepActiveHoldTicks || sweepActiveReleaseTicks_ > 0;
        const bool armedWaiting = engine.status() == EngineStatus::Running && engine.referenceLoaded()
                               && ! anyGraded && ! sweepEngaged;
        armedNoSweepTicks_ = armedWaiting ? armedNoSweepTicks_ + 1 : 0;
    }
    measureStage_.feedLiveLevels (lv.inL, lv.clipL, lv.inR, lv.clipR, lv.outMono, lv.clipOut);
    // Debounce the live silent-input check (read by updateStatusLine): count consecutive below-floor
    // ticks so brief pre-/inter-sweep gaps don't flicker the status; reset the instant signal returns.
    const bool blockSilent = lv.inL < HealthMonitor::kLowLevelLinear && lv.inR < HealthMonitor::kLowLevelLinear;
    silentTicks_ = blockSilent ? (silentTicks_ + 1) : 0;
    // Too-quiet (low-SNR) guidance: the input is PRESENT (not the silent case above) yet the run has
    // never reached a healthy capture level. Debounced like the silent check so startup and the gaps
    // between Dirac's L/R sweeps don't flicker it; clears the instant the level gets healthy (the
    // engine latch is monotonic per run). This catches the present-but-quiet "tin-can" capture that
    // otherwise reads as "clean".
    const bool tooQuiet = ! blockSilent && ! engine.reachedGoodLevel();
    lowLevelTicks_ = tooQuiet ? (lowLevelTicks_ + 1) : 0;
    // Low-SNR (sweep-to-noise) guidance: the engine raised LowSnr at the just-finished sweep's edge.
    // It's a per-sweep latch (sticky for the run), so a short debounce only steadies the first render;
    // it never decays mid-run (the flag stays set), which is intended -- a noisy sweep stays flagged
    // until the next Start re-scopes. Mirrors the lowLevelTicks_ debounce pattern.
    lowSnrTicks_ = any (engine.health().flags & HealthFlag::LowSnr) ? (lowSnrTicks_ + 1) : 0;
    {   // Honest capture progress (P3 Task 6): track WHICH ear is being captured and for how long (GUI
        // ticks). Same live-ear rule as updateActiveEarIndicator below (autoActiveEar + raw blockSilent).
        const bool autoMode = settings.combineMode() == CombineMode::AutoPerEar;
        const int  ear = (engine.status() == EngineStatus::Running && autoMode && ! blockSilent)
                           ? engine.autoActiveEar() : -1;
        if (ear != captureEar_) { captureEar_ = ear; captureTicks_ = 0; }
        else if (ear >= 0)      ++captureTicks_;
    }
    updateActiveEarIndicator (blockSilent);   // AutoPerEar: highlight the earcup being captured now
    pollReferenceGrade();                      // Plan 5: drain a completed-sweep grade request (off-thread grading)
    pollChainConfig();                         // 48k-everywhere chain-config check (warn + veto green; pre-Start too)
    if (engine.status() == EngineStatus::Running) updateStatusLine();

    // ---- Diagnostic log: reference-monitor transitions ON CHANGE + a ~30 s heartbeat (Task 3) ----
    // Log the detector internals only when a TRACKED value actually changes (state / referenceLoaded / a
    // bucketed coherence step / the guidance verdict), so a steady run doesn't flood the log at 30 Hz. The
    // input peak is NOT a change trigger: it changes constantly during a sweep (1 dB buckets at 30 Hz), which
    // produced ~4000 lines per measurement. It stays an INFO field on the logged line (so each line still
    // shows the level) and in the de-duplicated heartbeat -- just not in the trigger. Coherence is bucketed
    // (0.05) so dither alone never logs.
    {
        const int   refState   = engine.refMonState();
        const bool  refLoaded  = engine.referenceLoaded();
        const float peak       = engine.lastInputBlockPeak();
        const float coher      = engine.lastMatchCoherence();
        const auto  fl         = engine.health().flags;
        const bool  mismatch   = any (fl & HealthFlag::RefMismatch);
        const bool  lowQuality = any (fl & HealthFlag::RefLowQuality);
        const int   coherBucket = juce::roundToInt (coher * 20.0f);   // 0.05 steps
        if (refState != lastRefMonStateLogged_ || refLoaded != lastRefLoadedLogged_
            || coherBucket != lastCoherBucketLogged_) {
            logLine (eb::DiagnosticLog::Level::Info,
                     juce::String ("RefMon change: state=") + refMonStateName (refState)
                   + " referenceLoaded=" + (refLoaded ? "yes" : "no")
                   + " inputPeak=" + linToDbStr (peak) + " dBFS"   // INFO field only (not a change trigger)
                   + " matchCoherence=" + juce::String (coher, 2)
                   + " verdict=" + (mismatch ? "mismatch" : (lowQuality ? "lowQuality" : "ok")));
            lastRefMonStateLogged_      = refState;
            lastRefLoadedLogged_        = refLoaded;
            lastCoherBucketLogged_      = coherBucket;
        }
    }
    // ~30 s heartbeat, DE-DUPLICATED: a steady-state snapshot, but only when the line actually CHANGES,
    // plus a sparse keep-alive (~every kHeartbeatKeepalive beats) so a long idle still shows the app is
    // alive. An idle "status=Stopped ... <-120 dBFS" repeated every 30 s otherwise clogs the rolling
    // window with ~200 identical lines and pushes real data out (user-reported).
    if (++heartbeatTick_ >= kHeartbeatTicks) {
        heartbeatTick_ = 0;
        const char* st = engine.status() == EngineStatus::Running ? "Running"
                       : engine.status() == EngineStatus::Error   ? "Error" : "Stopped";
        juce::String content = juce::String ("Heartbeat: status=") + st
               + " refMon=" + refMonStateName (engine.refMonState())
               + " referenceLoaded=" + (engine.referenceLoaded() ? "yes" : "no")
               + " inputPeak=" + linToDbStr (engine.lastInputBlockPeak()) + " dBFS";
        if (content != lastHeartbeatContent_ || heartbeatsSuppressed_ >= kHeartbeatKeepalive) {
            logLine (eb::DiagnosticLog::Level::Info, content);
            lastHeartbeatContent_ = content;
            heartbeatsSuppressed_ = 0;
        } else {
            ++heartbeatsSuppressed_;   // identical to the last logged beat -> suppress (idle de-dup)
        }
    }

    // Raw-input (ADC) clipping warning. Re-arm from the edge-triggered, self-clearing recent-clip
    // signal (NOT the sticky ClipInput flag, which never clears mid-run and would pin the warning on
    // forever after one clip). consumeRecentInputClip() drains any clip that occurred between polls --
    // so a one-block overload at the coupler resonance is never missed -- yet goes false once clipping
    // stops, letting the hold decay ~8 s after the user lowers the gain.
    if (engine.consumeRecentInputClip()) inputClipHold_ = kInputClipHoldTicks;
    else if (inputClipHold_ > 0)         --inputClipHold_;
    const bool showClip = inputClipHold_ > 0;
    if (showClip != inputClipHint.isVisible()) { inputClipHint.setVisible (showClip); resized(); }

    // L/R wiring check: show the verdict once it lands, or time out after ~5 s of no tone.
    if (verifyTicks > 0) {
        if (engine.lrVerifyComplete()) {
            const auto res = engine.lrVerifyResult();
            engine.endLrVerify();
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            const bool ok = (res == LrResult::Pass);
            verifyResultLabel.setColour (juce::Label::textColourId, ok ? Theme::ok() : Theme::warn());
            verifyResultLabel.setText (
                res == LrResult::Pass      ? juce::String ("L/R wiring OK (left earcup drives the left mic)")
              : res == LrResult::Swapped   ? juce::String ("Channels SWAPPED (left earcup drives the right mic)")
              : res == LrResult::Ambiguous ? "Ambiguous" + kDash + "both mics responded (acoustic leak?)"
                                           : "No signal" + kDash + "play a tone into the LEFT earcup",
                juce::dontSendNotification);
        } else if (++verifyTicks > 150) {   // ~5 s at 30 Hz
            engine.endLrVerify();
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setColour (juce::Label::textColourId, Theme::warn());
            verifyResultLabel.setText ("No signal detected" + kDash + "play a tone into the LEFT earcup", juce::dontSendNotification);
        }
    }

    // Tick wiring (Task 4): recompute the wizard view from live truth EVERY tick — snapshot -> the pure
    // machine -> spine_.setState (label writes are set-if-changed) + stageHost_.showStage only on a real
    // change (its early-return guards focus). This is the proper home for the recompute; the event-path
    // updateStartGate() also calls it for immediate feedback (idempotent + cheap).
    refreshWizardView();

    // Follow live system light/dark changes (~2 s cadence; cheap, no allocation on the hot path).
    if (++themeTick >= 60) {
        themeTick = 0;
        const bool a11yChanged = SystemA11y::refresh();   // HIG: re-read Reduce Motion / Increase Contrast / Reduce Transparency
        if (theme.syncMode()) {
            applyTextColours();
            applyTitleBarTheme();
            // HIG/render fix: ComboBox/PopupMenu labels cache their colour, so a LIVE OS theme switch left the
            // combo text invisible (light-on-light) until relaunch. sendLookAndFeelChange re-reads every child's
            // colours from the updated LookAndFeel so the combos repaint correctly on the switch.
            if (auto* top = getTopLevelComponent()) { top->sendLookAndFeelChange(); top->repaint(); }
            else { sendLookAndFeelChange(); repaint(); }
        }
        else if (a11yChanged) {   // an Increase-Contrast / Reduce-Transparency toggle (no theme change) -> repaint to apply
            if (auto* top = getTopLevelComponent()) top->repaint(); else repaint();
        }
    }
}

// Title-bar height - ONE constant shared by paint() and resized() (#25). The wizard cutover moved the
// per-ear status stack + dots OUT of the title bar (into MeasureStage), so the bar now carries only brand
// + Start + update link and shrinks to 56 (spec § resized: kBarH shrinks to 56).
static constexpr int kBarH = 56;

void MainComponent::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());   // the stage background (StageHost/stages paint their own bg on top too)
    const int barH = kBarH;
    const int spineW = spine_.preferredWidth();
    auto bar = getLocalBounds().removeFromTop (barH);

    // Title bar fill.
    g.setColour (Theme::barBg());
    g.fillRect (bar);
    // Spine column background behind the spine (the spine paints its own bg + right hairline; this keeps
    // the graphite column solid behind it, matching the old fixed-rail look).
    g.setColour (Theme::rail());
    g.fillRect (juce::Rectangle<int> (0, barH, spineW, getHeight() - barH));

    // Separators: under the title bar + down the spine's right edge.
    g.setColour (Theme::sep());
    g.fillRect (0, barH - 1, getWidth(), 1);
    g.fillRect (spineW - 1, barH, 1, getHeight() - barH);

    // Brand headphones glyph (monochrome — accent is reserved for the action/selection).
    // Same construction as installer/assets/icon.svg (headband arc + short side stems + filled
    // rounded-rect earpads) so the in-app logo matches the app/window icon. Ratios are the icon's
    // (band r=226; stems 76, earpads 126x256 rx60, pad-top 40 below band centre, stroke 54).
    const float R   = 8.0f;                                  // headband radius
    const float gx  = 24.0f;                                 // glyph centre x
    const float gy  = (float) bar.getCentreY() - R * 0.16f;  // band centre (centres the whole glyph)
    const float stem = R * 0.34f;
    const float padW = R * 0.56f, padH = R * 1.13f, padRx = R * 0.27f;
    const float padTop = gy + R * 0.18f;
    g.setColour (Theme::text());
    juce::Path hp;
    hp.startNewSubPath (gx - R, gy + stem);
    hp.lineTo (gx - R, gy);
    hp.addCentredArc (gx, gy, R, R, 0.0f,
                      -juce::MathConstants<float>::halfPi,
                       juce::MathConstants<float>::halfPi, false);
    hp.lineTo (gx + R, gy + stem);
    g.strokePath (hp, juce::PathStrokeType (juce::jmax (2.0f, R * 0.24f),
                                            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (gx - R - padW * 0.5f, padTop, padW, padH, padRx);
    g.fillRoundedRectangle (gx + R - padW * 0.5f, padTop, padW, padH, padRx);
    // (The Levels card backdrop moved into LevelStage::paint; the per-ear status stack + dots moved into
    //  MeasureStage — the title bar no longer carries either.)
}

void MainComponent::resized() {
    auto area = getLocalBounds();

    // --- Title bar (brand + Start + update link only; the status stack moved to MeasureStage) ---
    auto bar = area.removeFromTop (kBarH);   // shared with paint() (#25); 56 now the stack left the bar
    {
        // P3 Task 4: the engine transport moved into LevelStage (§3.3), so the bar keeps only the
        // brand glyph gutter + update link + format cluster - the update link packs the right edge now.
        auto x = bar.reduced (16, 0);
        x.removeFromLeft (44);                                   // the painted brand glyph's gutter
        if (updateLink.isVisible()) {
            const int w = juce::jmin (230, x.getWidth());
            updateLink.setBounds (x.removeFromRight (w).withSizeKeepingCentre (w, 22));
            x.removeFromRight (14);
        }
        const int fw = juce::jmin (240, juce::jmax (0, x.getWidth()));
        fmtCluster_.setBounds (x.removeFromRight (fw).withSizeKeepingCentre (fw, 20));
    }

    // --- Version footnote (pinned bottom-right, below both columns) ---
    versionLabel.setBounds (area.removeFromBottom (22).reduced (16, 2));

    // --- Spine column (persistent step rail) + its footer diagnostics strip ---
    // The spine fills the graphite column on the left; a small footer strip at the bottom of the column
    // hosts the diagnostics affordances (§5.5: two log buttons + the auto-update toggle beneath — plain,
    // no panel yet). These are parented to `this` (WizardSpine stays untouched) and drawn over the rail.
    auto spineCol = area.removeFromLeft (spine_.preferredWidth());
    {
        constexpr int footPad = 12, btnH = 24, rowGap = 8, toggleH = 24;
        const int footH = footPad + btnH + rowGap + toggleH + footPad;
        auto foot = spineCol.removeFromBottom (footH).reduced (footPad, 0);
        foot.removeFromTop (footPad);
        auto btnRow = foot.removeFromTop (btnH);
        openLogButton.setBounds   (btnRow.removeFromLeft (btnRow.getWidth() / 2 - 4));
        btnRow.removeFromLeft (8);
        exportLogButton.setBounds (btnRow);
        foot.removeFromTop (rowGap);
        autoUpdateToggle.setBounds (foot.removeFromTop (toggleH));
    }
    spine_.setBounds (spineCol);

    // --- Stage host (fills the rest; shows exactly one stage) ---
    stageHost_.setBounds (area);

    // RESTORED CONTRACT: "resized() relays everything." Several call sites toggle a stage-hosted
    // surface's visibility/text and then call resized() EXPECTING a full relayout — but stageHost_'s
    // bounds are usually unchanged (same size), so JUCE never re-runs the child stages' resized() on
    // its own, and those surfaces would render 0x0 until the next window resize. So drive every stage's
    // layout unconditionally here. The three dependent call sites:
    //   - inputClipHint (updateStatus ~L2637 — raw-input clip warning, LevelStage)
    //   - diracCableHint / diracFixButton (updateDiracCableHint ~L740, ConnectStage)
    //   - preflightInfo (Start handler ~L1017 — the info line claims a row only when non-empty, ConnectStage)
    connectStage_.resized();
    calibrateStage_.resized();
    levelStage_.resized();
    measureStage_.resized();
}

} // namespace eb
