#include "gui/MainComponent.h"
#include "gui/ClipStatus.h"
#include "gui/RawRailStatus.h"
#include "gui/StartGate.h"   // eb::startReady (Task 3 / #3 advanced override)
#include "gui/StartNotes.h"  // eb::buildStartNotes (Task 4 / #8 calm bit-depth note)
#include "platform/DiracCompat.h"
#include "cal/CalibrationPairValidator.h"   // eb::validateCalibrationPair (P0-07)
#include "audio/CalibrationGeneration.h"    // eb::CalibrationGeneration (generation lifecycle)
#include "audio/RefMonitor.h"               // eb::RefMonState / gradeMeasurement / refMonBlocksGreen (Plan 5)
#include "audio/HardwareDiracDetect.h"      // eb::sweepWasInternal / hardwareDiracSuggestion (hardware-Dirac)
#include "platform/OutputActivity.h"        // eb::outputRenderPeakForName (is Dirac's PC output rendering?)
#include "gui/SnrStatus.h"                  // eb::kMinSweepSnrDb (sweep-to-noise SNR threshold; match-window SNR fix)
#include "gui/LiveInputStatus.h"            // eb::liveInputStatus (live per-channel in-sweep readout)
#include "audio/LoopbackReference.h"        // eb::captureLoopback / validateReferenceCapture / readDiracDeviceType (Plan 5)
#include "platform/EndpointFormat.h"        // eb::endpointMixSampleRateForName (the WASAPI mix-format rate)
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

MainComponent::MainComponent() {
    setLookAndFeel (&theme);
    firPool = std::make_unique<juce::ThreadPool> (1);

    // --- Diagnostic log: open it FIRST so the whole ctor (and every later handler) can write to it ---
    // %TEMP%/EarsBridge/logs, rotating + size-capped (see DiagnosticLog). Logging is message-thread only.
    log_ = std::make_unique<eb::DiagnosticLog> (
        juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("EarsBridge").getChildFile ("logs"));
    // Launch banner: app version, build flavour, and the OS. The serial backstop is already armed
    // (loggedSerial_ is empty until a cal loads, so redactSerial is a no-op here).
   #ifdef NDEBUG
    const char* buildFlavour = "Release";
   #else
    const char* buildFlavour = "Debug";
   #endif
    logLine (eb::DiagnosticLog::Level::Info,
             juce::String ("==== EARS Bridge launch ==== v") + EB_VERSION_STRING
           + " build=" + buildFlavour
           + " os=" + juce::SystemStats::getOperatingSystemName());

    // --- Title bar brand ---
    brandLabel.setText ("EARS Bridge", juce::dontSendNotification);
    brandLabel.setColour (juce::Label::textColourId, Theme::text());
    brandLabel.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));
    addAndMakeVisible (brandLabel);

    // Current-version footnote, pinned bottom-right. Reads the single version source of truth.
    versionLabel.setText ("v" EB_VERSION_STRING, juce::dontSendNotification);
    versionLabel.setColour (juce::Label::textColourId, Theme::textFaint());
    versionLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    versionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (versionLabel);

    // --- Transport (gated Start + status note) ---
    startStop.getProperties().set ("primary", true);
    startStop.onClick = [this] { onStartStop(); };
    addAndMakeVisible (startStop);
    statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    statusLine.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLine.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLine);
    // Second status line (Task 5): the RIGHT-ear verdict when both per-ear lines show; blank otherwise.
    // Same typography/justification as statusLine; the per-branch colour is set in updateStatusLine.
    statusLineR.setColour (juce::Label::textColourId, Theme::textDim());
    statusLineR.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLineR.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLineR);
    addAndMakeVisible (gradeDotsL_);
    addAndMakeVisible (gradeDotsR_);

    // Update link: hidden until a newer release is found; opens the release page in the browser.
    updateLink.setColour (juce::HyperlinkButton::textColourId, Theme::accent());
    updateLink.setFont (juce::Font (juce::FontOptions (12.0f)), false, juce::Justification::centredRight);
    addChildComponent (updateLink);

    // --- Left rail viewport (scrolls the whole config stack so a tall Advanced section is reachable) ---
    // Every rail child below is parented to railContent (not `this`); the Viewport scrolls it vertically.
    railViewport.setViewedComponent (&railContent, false);   // false: we own railContent (a member)
    railViewport.setScrollBarsShown (true, false);            // vertical only; the rail width is fixed
    railViewport.setScrollBarThickness (10);
    addAndMakeVisible (railViewport);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    railContent.addAndMakeVisible (inputPicker);
    inputGainHint.setText ("Leave the EARS gain switch alone (changing it drops the jig from Windows). "
                           "Set levels in Dirac: Master output, then Mic gain.",
                           juce::dontSendNotification);
    inputGainHint.setColour (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setFont (juce::Font (juce::FontOptions (11.5f)));
    inputGainHint.setJustificationType (juce::Justification::topLeft);
    inputGainHint.setMinimumHorizontalScale (1.0f);
    railContent.addAndMakeVisible (inputGainHint);

    // --- Combine selector ---
    styleEyebrow (combineLabel, "COMBINE MODE");
    railContent.addAndMakeVisible (combineLabel);
    combineModel = combineModeOrder();
    for (size_t i = 0; i < combineModel.size(); ++i) {
        auto& m = combineModel[i];
        juce::String label;
        switch (m.mode) {
            case CombineMode::LeftOnly:     label = "Left ear only";          break;
            case CombineMode::RightOnly:    label = "Right ear only";         break;
            case CombineMode::Average:      label = "Average (L+R)/2";        break;
            case CombineMode::Sum:          label = "Sum L+R";                break;
            case CombineMode::AutoPerEar:   label = "Auto per-ear (Dirac)";   break;
        }
        if (m.recommended)     label += "   (recommended)";
        if (m.clipRiskWarning) label += "   (+6 dB)";
        combineBox.addItem (label, (int) i + 1);
    }
    combineBox.onChange = [this] { onCombineChosen(); };
    railContent.addAndMakeVisible (combineBox);
    combineHint.setColour (juce::Label::textColourId, Theme::textDim());
    combineHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    combineHint.setJustificationType (juce::Justification::topLeft);
    combineHint.setMinimumHorizontalScale (1.0f);
    railContent.addAndMakeVisible (combineHint);

    // --- Output picker + Dirac hint + preflight ---
    outputPicker.onDeviceChosen = [this] (const DeviceId& d) { onOutputChosen (d); };
    railContent.addAndMakeVisible (outputPicker);
    outputHint.setText ("In Dirac Live, choose this device's capture side as the recording input.",
                        juce::dontSendNotification);
    outputHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    outputHint.setJustificationType (juce::Justification::topLeft);
    railContent.addAndMakeVisible (outputHint);
    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    railContent.addAndMakeVisible (preflightLabel);
    // Calm, neutral fact line (NOT a warning): e.g. "Output: 32-bit float (shared mode) - normal."
    // The full honest explanation lives in its tooltip so the short line always fits one rail line.
    preflightInfo.setColour (juce::Label::textColourId, Theme::textDim());
    preflightInfo.setFont (juce::Font (juce::FontOptions (12.0f)));
    preflightInfo.setTooltip ("WASAPI shared mode always delivers 32-bit float; your bit-depth is a "
                              "stored preference and doesn't affect quality - this is expected.");
    railContent.addAndMakeVisible (preflightInfo);

    // Standard-VB-CABLE-vs-Dirac compatibility hint + one-click fix (hidden unless that cable is chosen).
    diracCableHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    diracCableHint.setJustificationType (juce::Justification::topLeft);
    diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
    railContent.addChildComponent (diracCableHint);
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
    railContent.addChildComponent (diracFixButton);

    // --- Rate + depth ---
    styleEyebrow (rateLabel, "RATE");
    railContent.addAndMakeVisible (rateLabel);
    rateBox.onChange = [this] { onRateChosen(); };
    railContent.addAndMakeVisible (rateBox);
    rateWarn.setColour (juce::Label::textColourId, Theme::warn());
    rateWarn.setFont (juce::Font (juce::FontOptions (12.0f)));
    railContent.addAndMakeVisible (rateWarn);
    styleEyebrow (bitLabel, "PREFERRED DEPTH");
    railContent.addAndMakeVisible (bitLabel);
    bitBox.onChange = [this] { onBitDepthChosen(); };
    railContent.addAndMakeVisible (bitBox);

    // --- Advanced disclosure ---
    advancedToggle.setButtonText ("Advanced");
    advancedToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Advanced=") + (advancedToggle.getToggleState() ? "on" : "off"));
        resized();
    };
    railContent.addAndMakeVisible (advancedToggle);
    complexPhaseToggle.setButtonText ("Complex (with-phase) FIR");
    complexPhaseToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Complex (with-phase) FIR=") + (complexPhaseToggle.getToggleState() ? "on" : "off"));
        settings.setComplexPhase (complexPhaseToggle.getToggleState());
        rebuildFirsAsync();
    };
    railContent.addChildComponent (complexPhaseToggle);
    autoUpdateToggle.setToggleState (settings.autoCheckUpdates(), juce::dontSendNotification);
    autoUpdateToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Automatically check for updates=") + (autoUpdateToggle.getToggleState() ? "on" : "off"));
        settings.setAutoCheckUpdates (autoUpdateToggle.getToggleState());
        settings.flush();
        if (! autoUpdateToggle.getToggleState() && updateLink.isVisible()) {
            updateLink.setVisible (false);
            resized();
        }
    };
    railContent.addChildComponent (autoUpdateToggle);
    // #3: advanced override toggle. Restore its persisted state; on click, persist + re-run the gate.
    overrideToggle.setToggleState (settings.advancedOverride(), juce::dontSendNotification);
    overrideToggle.onClick = [this] {
        logLine (eb::DiagnosticLog::Level::Debug,
                 juce::String ("Toggle: Allow non-Dirac use=") + (overrideToggle.getToggleState() ? "on" : "off"));
        settings.setAdvancedOverride (overrideToggle.getToggleState());
        settings.flush();
        updateStartGate();   // recompute Start enabled-ness + the status line for the new policy
    };
    railContent.addChildComponent (overrideToggle);
    // Hardware-Dirac toggle: ON -> grading runs OFF the loopback (a hardware box generates its own sweep, so the
    // loopback captures no reference). Persist + tell the engine (publishes GradingOffHardware + suppresses the
    // grade) + suppress Learn-reference (nothing to learn). The auto-detect only SUGGESTS this toggle.
    hwDiracToggle.setToggleState (settings.diracHardwareProcessor(), juce::dontSendNotification);
    hwDiracToggle.onClick = [this] {
        const bool on = hwDiracToggle.getToggleState();
        logLine (eb::DiagnosticLog::Level::Info, juce::String ("Toggle: Dirac hardware processor=") + (on ? "on" : "off"));
        settings.setDiracHardwareProcessor (on);
        settings.flush();
        engine.setDiracHardwareProcessor (on);   // publish GradingOffHardware / clear
        learnRefButton.setEnabled (! on);        // a hardware box has no PC-render reference to learn
    };
    railContent.addChildComponent (hwDiracToggle);
    if (settings.diracHardwareProcessor()) { engine.setDiracHardwareProcessor (true); learnRefButton.setEnabled (false); }
    styleEyebrow (firLenLabel, "FIR LENGTH");
    railContent.addChildComponent (firLenLabel);
    firLenBox.addItem ("Auto (scales with rate)", kFirLenAutoId);
    for (int n : { 4096, 8192, 16384, 32768 }) firLenBox.addItem (juce::String (n), n);
    firLenBox.onChange = [this] {
        const int id = firLenBox.getSelectedId();
        settings.setFirLength (id == kFirLenAutoId ? 0 : id);
        rebuildFirsAsync();
    };
    railContent.addChildComponent (firLenBox);
    styleEyebrow (trimLabel, "OUTPUT TRIM (dB)");
    railContent.addChildComponent (trimLabel);
    trimSlider.setRange (-24.0, 0.0, 0.1);
    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    trimSlider.onValueChange = [this] {
        settings.setOutputTrimDb (trimSlider.getValue());
        engine.setOutputTrimDb (trimSlider.getValue());   // apply live (the graph reads it lock-free)
    };
    railContent.addChildComponent (trimSlider);

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
            verifyResultLabel.setText ("Listening - play a tone in the LEFT earcup...", juce::dontSendNotification);
        } else {
            verifyResultLabel.setColour (juce::Label::textColourId, Theme::warn());
            verifyResultLabel.setText (err, juce::dontSendNotification);
        }
    };
    railContent.addChildComponent (verifyButton);
    verifyResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    verifyResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    railContent.addChildComponent (verifyResultLabel);

    // Reference-Based Measurement Monitor (Plan 5): learn the loopback reference. The capture itself is a
    // Windows WASAPI loopback (on-device) and must run with Dirac's Processor in Windows Audio (shared)
    // mode; we detect that read-only and inform. Only meaningful while Stopped (the loopback can't run
    // alongside the live ASIO measurement).
    learnRefButton.onClick = [this] { onLearnReference(); };
    railContent.addAndMakeVisible (learnRefButton);   // REQUIRED measurement step -> main flow, not under Advanced
    learnRefResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    railContent.addAndMakeVisible (learnRefResultLabel);

    // Diagnostic-log export (Task 3). "Open log folder" reveals %TEMP%/EarsBridge/logs; "Export log..."
    // zips the whole logs dir to a user-chosen path. Both are message-thread-only affordances.
    openLogButton.onClick = [this] {
        if (log_) {
            logLine (eb::DiagnosticLog::Level::Debug, "Button: Open log folder clicked");
            logLine (eb::DiagnosticLog::Level::Info, "User opened the log folder.");
            log_->directory().revealToUser();
        }
    };
    railContent.addChildComponent (openLogButton);
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
    railContent.addChildComponent (exportLogButton);

    // --- Right pane: cal cards + Levels ---
    styleEyebrow (calEyebrow, "CALIBRATION");
    addAndMakeVisible (calEyebrow);
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f);  updateStartGate(); syncPlotScales(); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); updateStartGate(); syncPlotScales(); };
    // Removing a slot resets that ear to unity AND bumps a fresh (now-incomplete) generation, so the
    // Start gate (calibrationApplied()) closes instead of staying satisfied by the prior valid build.
    leftCal.onCalCleared  = [this] { settings.setLeftCalPath  ({}); engine.clearLeftCalFir();  logLine (eb::DiagnosticLog::Level::Info, "Cal cleared: LEFT");  rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    rightCal.onCalCleared = [this] { settings.setRightCalPath ({}); engine.clearRightCalFir(); logLine (eb::DiagnosticLog::Level::Info, "Cal cleared: RIGHT"); rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);
    styleEyebrow (levelsEyebrow, "LEVELS");
    addAndMakeVisible (levelsEyebrow);
    levelsHint.setText ("Set Dirac's Master output so the L and R meters reach the green band.", juce::dontSendNotification);
    levelsHint.setColour (juce::Label::textColourId, Theme::textDim());
    levelsHint.setFont (juce::Font (juce::FontOptions (11.5f)));
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
    inputClipHint.setText ("EARS mic input near full scale - lower Dirac's Master output (try ~-12.5 dB). "
                           "Don't touch the EARS gain switch. Dirac won't flag this: the clip is at the "
                           "mic, before Dirac records.",
                           juce::dontSendNotification);
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
            if (auto in  = inputPicker.selectedDevice())  engine.setInput  (*in);
            if (auto out = outputPicker.selectedDevice()) engine.setOutput (*out);
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
    if (auto in  = inputPicker.selectedDevice())  { engine.setInput  (*in);  if (in->key() != settings.inputKey()) settings.setInputKey (in->key()); }
    if (auto out = outputPicker.selectedDevice()) engine.setOutput (*out);
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
    if (! loadedReference_.empty())
        logLine (eb::DiagnosticLog::Level::Info,
                 "Reference: reloaded a stored loopback reference ("
               + juce::String (loadedReference_.size() / loadedReferenceRate_, 1) + " s)");

    updateStartGate();
    syncPlotScales();
    setSize (900, 700);
    startTimerHz (30);

    // Background update check: runs on every launch, gated only by the user's opt-out toggle.
    // It is async + non-blocking, so a newer release surfaces the title-bar link each time the
    // app starts. (No throttle: GitHub's unauthenticated rate limit is 60/h — far above any
    // realistic launch rate — and a failed/offline check simply shows nothing and retries next time.)
    if (settings.autoCheckUpdates()) {
        updateChecker.start (juce::String (EB_VERSION_STRING),
            [this] (UpdateInfo info) {
                if (info.updateAvailable) {
                    updateLink.setButtonText ("Update available - v" + info.latestVersion);
                    updateLink.setURL (juce::URL (info.releaseUrl));
                    updateLink.setVisible (true);
                    resized();
                }
            });
    }
}

MainComponent::~MainComponent() {
    stopTimer();
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
    log_->write (level, eb::DiagnosticLog::redactSerial (msg, loggedSerial_));
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

void MainComponent::onInputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    engine.setInput (d);
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
    settings.setOutputKey (d.key());
    preflightLabel.setText (d.isVirtualSink ? juce::String()
                                            : "Selected output is not a known virtual cable.",
                            juce::dontSendNotification);
    preflightInfo.setText ({}, juce::dontSendNotification);   // a fresh pick clears any prior-run fact line
    rebuildBitDepthMenu();
    updateDiracCableHint();
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
        diracCableHint.setText ("The Hi-Fi Cable connects to Dirac but won't carry audio through it "
                                "(no sample-rate converter). Use the standard CABLE Input instead.",
                                juce::dontSendNotification);
        diracFixButton.setVisible (false);
    } else if (kind == VK::StdVbCable) {
        if (eb::diracSharedModeEnabled()) {
            diracCableHint.setColour (juce::Label::textColourId, Theme::ok());
            diracCableHint.setText ("Dirac is set to shared mode, so this cable works. If Dirac is open, "
                                    "fully close and reopen it once.", juce::dontSendNotification);
            diracFixButton.setVisible (false);
        } else {
            diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
            diracCableHint.setText ("Dirac records this standard cable in exclusive mode, which it can't do "
                                    "(error 600007). Click below to set Dirac to shared mode:",
                                    juce::dontSendNotification);
            diracFixButton.setVisible (true);
        }
    } else if (kind == VK::OtherVirtual) {
        diracCableHint.setColour (juce::Label::textColourId, Theme::textDim());
        diracCableHint.setText ("If Dirac can't open this cable (error 600007), set Dirac to shared mode "
                                "or use the standard VB-CABLE.", juce::dontSendNotification);
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
    rateWarn.setText (warn ? "Not native - will be resampled." : juce::String(),
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
    rateWarn.setText (rateModel[(size_t) idx].resampleWarning ? "Not native - will be resampled."
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
            h = "Use this with Dirac. Records only the earcup Dirac is sweeping - clean per-ear "
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
    settings.setLeftCalPath (f.getFullPathName());
    // Track the serial so logLine() can scrub it everywhere (the backstop). NEVER log the value itself —
    // only that a serial is present and its length, per the brief.
    juce::String serial;
    if (auto c = leftCal.calFile()) serial = c->serial;
    if (serial.isNotEmpty()) loggedSerial_ = serial;
    logLine (eb::DiagnosticLog::Level::Info,
             "Cal load: LEFT serial present (" + juce::String (serial.length()) + " chars)");
    rebuildFirsAsync();
}
void MainComponent::onRightCalLoaded (const juce::File& f) {
    settings.setRightCalPath (f.getFullPathName());
    juce::String serial;
    if (auto c = rightCal.calFile()) serial = c->serial;
    if (serial.isNotEmpty()) loggedSerial_ = serial;
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

    firPool->removeAllJobs (false, 0);                  // request the previous job stop; the genId check below is the real guard
    firPool->addJob ([safe, genId, sr, taps, mode, left, right]() mutable {
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
    });
}

void MainComponent::onStartStop() {
    if (engine.status() == EngineStatus::Running) {
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Stop clicked");
        engine.stop();
        startStop.setButtonText ("Start");
        // Task 4: re-arm BOTH per-ear grade pollers on Stop so a stale match from this run can't carry into
        // the next (each ear again needs two consecutive matched polls before its first grade).
        gradePollTick_ = 0; gradePollerL_.reset(); gradePollerR_.reset();
        // Re-arm the hardware-Dirac auto-detect too: a fresh run must re-observe the output silence + mic sweep.
        maxOutputRenderPeak_ = 0.0f; hwOutputReadable_ = false; hwDetectTick_ = 0;
        // Reset the live in-sweep readout state too, so a held level / sweep-active hold / live text from this
        // run can't bleed into the idle line after Stop (matches the Start-path reset).
        liveHeldLDb_ = liveHeldRDb_ = -120.0f;
        sweepActiveTicks_ = 0; sweepActiveReleaseTicks_ = 0; liveTextTick_ = 0; liveWasActive_ = false;
        liveHeldPrimary_.clear();
        logLine (eb::DiagnosticLog::Level::Info, "Stop: measurement stopped by the user.");
    } else {
        logLine (eb::DiagnosticLog::Level::Debug, "Button: Start clicked");
        if (verifyTicks > 0) {   // a pending L/R check holds the capture device; clear its GUI state
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setText ({}, juce::dontSendNotification);
        }
        juce::String err;
        if (engine.start (err)) {
            startStop.setButtonText ("Stop");
            inputClipHold_ = 0; silentTicks_ = 0; lowLevelTicks_ = 0; lowSnrTicks_ = 0; statusErrorMsg_.clear();   // no prior-run state bleed
            // Live in-sweep readout: start a fresh run at the silent floor with the sweep-active attack/release
            // debounce, the text cadence, and the held live-line text reset, so a prior run's held level /
            // sweep-active state / live text can't bleed in.
            liveHeldLDb_ = liveHeldRDb_ = -120.0f;
            sweepActiveTicks_ = 0; sweepActiveReleaseTicks_ = 0; liveTextTick_ = 0; liveWasActive_ = false;
            liveHeldPrimary_.clear();
            // Task 4 match-poll debounce: a fresh run starts un-matched and un-graded, so the first sustained
            // match (two consecutive matched polls) grades exactly once.
            gradePollTick_ = 0; gradePollerL_.reset(); gradePollerR_.reset(); lastListenTextLogged_.clear();
            maxOutputRenderPeak_ = 0.0f; hwOutputReadable_ = false; hwDetectTick_ = 0;   // fresh hardware-Dirac auto-detect
            gradeDotsL_.clear(); gradeDotsR_.clear(); smootherL_.reset(); smootherR_.reset();   // fresh quality dots each run
            // Surface a silent format downgrade: WASAPI shared mode can grant a different rate/depth
            // than the user selected, which would otherwise resample with no indication. The split:
            // genuine cautions (a real resample, an unverifiable rail) go on preflightLabel (yellow);
            // the 32-bit-float fact is NORMAL, so it goes on the neutral preflightInfo line (#8).
            const auto notes = eb::buildStartNotes (engine.grantedSampleRate(), settings.sampleRate(),
                                                    engine.grantedOutputBitDepth(), settings.outputBitDepth(),
                                                    engine.rawRail());
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

void MainComponent::updateStartGate() {
    const bool running  = engine.status() == EngineStatus::Running;
    const bool haveDevs = inputPicker.selectedDevice().has_value()
                       && outputPicker.selectedDevice().has_value();
    // Start requires a VALID, fully-built, atomically-applied generation (requested==built==applied,
    // valid) -- not merely two parsed files. This is the stale/incomplete/invalid guard (P0-02/P0-07).
    const bool haveCals = engine.calibrationApplied();
    // D7/R17: with real EARS + virtual cable, block non-AutoPerEar so the user can't record a
    // summed/single-ear signal into Dirac.
    const bool wrongMode = isRealEarsWithCable() && settings.combineMode() != CombineMode::AutoPerEar;
    // P1-09: a real EARS into an output that is NOT a verified virtual sink would record into a
    // device Dirac never sees. Block Start until the user picks the virtual audio cable.
    const bool physicalOutput = isRealEarsInput()
                             && outputPicker.selectedDevice().has_value()
                             && ! outputPicker.selectedDevice()->isVirtualSink;
    // #3: the advanced override relaxes ONLY the two policy gates (wrongMode, physicalOutput)
    // for non-Dirac use cases. It NEVER bypasses haveDevs/haveCals (devices + applied calibration).
    const bool ready    = eb::startReady (haveDevs, haveCals, wrongMode, physicalOutput,
                                          settings.advancedOverride());
    startStop.setEnabled (running || ready);
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
                       + " override="       + (settings.advancedOverride() ? "1" : "0") + ")");
            }
        }
    }
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateCalProblems();
    updateControlsEnabled();
    updateDiracMicGainHint();   // a cal load/clear changed the auto-headroom -> refresh the Mic-gain number
    updateStatusLine();
}

void MainComponent::updateDiracMicGainHint() {
    // EARS Bridge attenuates its own output by the auto makeup-headroom (see ProcessingGraph::recomputeHeadroom).
    // Surface that number so the user adds about the same positive Mic gain in Dirac and Dirac records at a
    // healthy level. The number changes only when a cal loads, so this is cheap to call from updateStartGate.
    const float n = engine.headroomAttenuationDb();   // >= 0 dB; 0 when no attenuation (unity / cut / flat cal)
    juce::String text = (n >= 0.5f)
        ? "EARS Bridge attenuates its output ~" + juce::String (juce::roundToInt (n))
          + " dB for headroom - add about +" + juce::String (juce::roundToInt (n))
          + " dB on Dirac's Mic gain (watch Dirac's input meter)."
        : "Set Dirac's Mic gain so Dirac records at a healthy level (watch Dirac's input meter).";
    if (text != diracMicGainHint.getText())
        diracMicGainHint.setText (text, juce::dontSendNotification);
}

void MainComponent::updateCalProblems() {
    // Surface a wrong cal file LOUDLY on the offending card so a left/right swap is impossible to miss
    // (the status line buries it under device/level warnings). Two independent sources:
    //   1. PER-SLOT single-file side check -- fires with just ONE slot loaded, the moment a file's
    //      detected side (CalFile::side, from filename + content) contradicts the slot it's in. This
    //      is what catches "R_HPN in the LEFT slot" before the second file is ever loaded.
    //   2. PAIR diagnostic (serial mismatch / bad type / a swap the validator catches) -- only
    //      meaningful once BOTH files are loaded and the current build has caught up (a stale
    //      diagnostic from the PREVIOUS generation must not flash).
    juce::String leftProblem, rightProblem;

    // (1) Per-slot side check (independent of the other slot).
    if (leftCal.hasCal()
        && eb::calSideMismatched (true, leftCal.calFile()->side, eb::CalSide::Left))
        leftProblem = "This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.";
    if (rightCal.hasCal()
        && eb::calSideMismatched (true, rightCal.calFile()->side, eb::CalSide::Right))
        rightProblem = "This looks like the LEFT cal, but it's in the RIGHT slot - swap the files.";

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
                leftProblem = "This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.";
            if (rightSlotSwapped && rightProblem.isEmpty())
                rightProblem = "This looks like the LEFT cal, but it's in the RIGHT slot - swap the files.";
        } else {
            // Serial mismatch / HEQ / unknown-type: not ear-specific. Show on both, but never clobber
            // a more specific per-slot swap message.
            if (leftProblem.isEmpty())  leftProblem  = diag;
            if (rightProblem.isEmpty()) rightProblem = diag;
        }
    }

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
    learnRefButton.setEnabled     (! frozen);   // the loopback learn can't run alongside the live measurement
}

void MainComponent::syncPlotScales() {
    // Both ear curves share one dB axis so they're directly comparable.
    float top = 6.0f;
    if (auto l = leftCal.calFile())  top = std::max (top, fitTopDb (*l));
    if (auto r = rightCal.calFile()) top = std::max (top, fitTopDb (*r));
    leftCal.setPlotRange (top);
    rightCal.setPlotRange (top);
}

void MainComponent::applyTextColours() {
    // Re-set every theme-dependent label colour (the Theme statics return the active mode);
    // paint-based components (meters, cards, plots) pick the mode up on repaint.
    brandLabel.setColour (juce::Label::textColourId, Theme::text());
    versionLabel.setColour (juce::Label::textColourId, Theme::textFaint());
    styleEyebrow (combineLabel,  "COMBINE MODE");
    styleEyebrow (rateLabel,     "RATE");
    styleEyebrow (bitLabel,      "PREFERRED DEPTH");
    styleEyebrow (firLenLabel,   "FIR LENGTH");
    styleEyebrow (trimLabel,     "OUTPUT TRIM (dB)");
    styleEyebrow (calEyebrow,    "CALIBRATION");
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
    updateStatusLine();
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

void MainComponent::updateStatusLine() {
    const auto st = engine.status();
    // Set-if-changed model (Bug B): every branch computes the FINAL (text, colour, tooltip) for statusLine and
    // statusLineR, then commits once through setLabelIfChanged / setTooltipIfChanged. We DON'T pre-clear the
    // lines here — clearing-then-conditionally-resetting is what made the notes blink. Defaults below are the
    // "blank second line / no tooltip" most branches want; a branch overrides them, and the single commit at
    // the end writes only what actually changed.
    juce::String sText, s2Text, sTip, s2Tip;
    juce::Colour sCol = Theme::textDim(), s2Col = Theme::warn();
    // When a branch drives the per-ear lines directly (renderPerEarStatusLines), it commits both labels itself
    // and sets perEarDriven=true so the generic commit at the end skips statusLine/statusLineR.
    bool perEarDriven = false;
    if (st == EngineStatus::Running) {
        const auto h = engine.health();
        // The 48k-everywhere config VETO is a hard error: a wrong-rate chain silently invalidates the
        // measurement, so it must take precedence over the live readout below (you cannot trust a level
        // shown over a mis-rated chain). Evaluated here so the live branch can defer to it.
        const bool rateVeto = chainVerdict_.checked && ! chainVerdict_.all48k;
        // Advance the live-readout state (peak-hold/decay + the attack/release sweep-active debounce) EVERY
        // tick, even if a hard error will win this render, so the held level + debounce stay continuous. It
        // writes NO labels itself; live.owns says whether the live line should take over (sweep-active+held).
        const LiveReadout live = updateLiveInputReadout();
        // Bug A inputs: a captured grade (any ear GradedClean/GradedSuspect/ReferenceStale, with a per-ear
        // reference loaded) must DEFER the pre-sweep "waiting" text and show a persistent "captured" instead.
        // The hard warns below (output-clip / low-SNR) still take precedence so we never say "safe to run the
        // next" over a clipped or noisy sweep.
        auto isGraded = [this] (int ear) {
            const auto s = (eb::RefMonState) engine.refMonState (ear);
            return s == eb::RefMonState::GradedClean
                || s == eb::RefMonState::GradedMarginal
                || s == eb::RefMonState::GradedSuspect
                || s == eb::RefMonState::ReferenceStale;
        };
        const bool hasGrade = engine.referenceLoaded() && (isGraded (0) || isGraded (1));
        // An invalidating condition is reported the instant it latches, regardless of phase.
        if (! h.cleanCapture) {
            sText = eb::invalidMeasurementMessage (h.flags);
            sCol  = Theme::danger();
        } else if (! rateVeto && live.owns) {
            // LIVE in-sweep readout: while Dirac is actually sweeping (debounced+release-held sweep-active),
            // show the live sweep PEAK / clip, OVERRIDING the stale per-ear verdicts and the "waiting"/
            // "captured" text below — the user needs to see the level and catch a clip AS IT HAPPENS. The two
            // hard errors above (invalid capture, the 48k veto) still win (they short-circuit before this
            // branch). The held live text persists between the ~7 Hz render ticks (live.primary carries the
            // last rendered text every tick now) so the single line doesn't blink (Bug B-1). statusLineR is
            // left at its empty default — the live readout is single-line.
            sText = live.primary;
            sCol  = live.colour;
        } else if (any (h.flags & HealthFlag::ClipOutput)) {
            // The output hit full scale (e.g. Sum's uncompensated +6 dB drove past the clamp). The clamp stops
            // a cable over, but it distorts the sweep -- flag it, don't pass it as "clean". HOISTED above the
            // captured/waiting branches (Bug A): a CLIPPED captured sweep must read the clip warning, never
            // "safe to run the next" and never the pre-sweep "waiting".
            sText = "Output clipping - lower the level or avoid Sum";
            sCol  = Theme::warn();
        } else if (lowSnrTicks_ >= kLowSnrHoldTicks) {
            // SNR guidance, BEFORE the captured/waiting branches so a noisy sweep never reads as "clean" or
            // "safe to run the next" (clean = no dropouts/clipping, which a low-SNR sweep can still pass). The
            // engine raised LowSnr at the sweep's edge off a TRUSTED floor only (honest silence otherwise). The
            // status line shares the title bar and is narrow, so keep the inline text SHORT and complete (no
            // clipping); the full guidance lives in the tooltip. snrNote's long form is the wording of record.
            const int snrDb = juce::roundToInt (engine.completedSweepSnrDb());
            sText = "Low SNR: sweep only " + juce::String (snrDb)
                  + " dB over the room noise - re-measure";
            sCol  = Theme::warn();
            sTip  = "The Dirac sweep was only " + juce::String (snrDb)
                  + " dB above the room-noise floor. Quieten the room (close windows, "
                    "stop fans/AC) or raise the level, then re-measure for a cleaner "
                    "correction.";
        } else if (rateVeto) {
            // 48k-everywhere VETO. ABOVE the captured/waiting + reference-monitor branches so a wrong-rate (or
            // unreadable) chain can NEVER read "verified"/"safe to run the next" — the reference monitor
            // REQUIRES 48k on all three endpoints, and a 44.1k Dirac output silently invalidates the
            // measurement. The hard errors above (invalid capture / output-clip / low-SNR) still win — those
            // are signal failures the user must see first; this is the config layer. chainVerdict_.summary is a
            // single short title-bar line naming the off-48k / unreadable endpoint(s).
            sText = chainVerdict_.summary;
            sCol  = Theme::warn();
        } else if (hasGrade) {
            // Bug A: a grade exists this run and the sweep isn't sounding (clip / low-SNR / 48k veto already
            // short-circuited above) -> show a PERSISTENT success confirmation that does NOT revert to
            // "waiting".
            //   statusLine  = the headline "Sweep captured - safe to run the next sweep" (ok colour).
            //   statusLineR = the per-ear DETAIL via renderEarStatusLine (keeps the INFO/unratified gate +
            //                 tooltip), so the actionable verdict (IR-SNR/THD, re-learn, peak) stays on the
            //                 lines. Both persist (set-if-changed) and never flip back to "waiting".
            // We set statusLine here and let renderEarStatusLine commit statusLineR, so perEarDriven=true keeps
            // the generic end-of-function commit from clobbering either line.
            setLabelIfChanged (statusLine, "Sweep captured - safe to run the next sweep", Theme::ok());
            setTooltipIfChanged (statusLine, {});
            renderEarStatusLine (1, "R", statusLineR);        // R-ear per-ear detail on the second line
            perEarDriven = true;
        } else if (h.session == SessionPhase::Idle || h.session == SessionPhase::Preflight) {
            // Before the sweep arms: validity isn't scoped to anything yet, so don't claim "clean". With a
            // reference loaded the grade no longer depends on the (level-blind) arm — the engine listens via
            // the rolling ring + the off-thread reference MATCH (Task 4) — so show the cosmetic activity state:
            // "Sweep in progress..." while signal is present, "Listening..." while quiet. Non-adopters (no
            // reference learned, no verdict yet) keep the unchanged wording.
            // hasGrade above already intercepted the graded case, so by here NO ear has graded this run (the
            // genuine pre-first-sweep wait) — only the cosmetic Listening<->Sweep-in-progress activity remains.
            const bool refEngaged = engine.referenceLoaded()
                                 && (eb::RefMonState) engine.refMonState() != eb::RefMonState::GradedClean
                                 && (eb::RefMonState) engine.refMonState() != eb::RefMonState::GradedMarginal
                                 && (eb::RefMonState) engine.refMonState() != eb::RefMonState::GradedSuspect
                                 && (eb::RefMonState) engine.refMonState() != eb::RefMonState::ReferenceStale;
            // Cosmetic activity flag drives ONLY this text (never the grade): present -> the sweep is
            // sounding now; quiet -> waiting for it. Log each Listening<->Sweep-in-progress transition.
            const bool active = engine.gradeSignalPresent();
            const juce::String refText = active ? "Sweep in progress..." : "Listening for the Dirac sweep...";
            if (refEngaged && refText != lastListenTextLogged_) {
                logLine (eb::DiagnosticLog::Level::Info, "RefMon status: " + refText);
                lastListenTextLogged_ = refText;
            }
            sText = refEngaged ? refText : juce::String ("Running - waiting for the Dirac sweep...");
            sCol  = Theme::textDim();
        } else if (engine.referenceLoaded()) {
            // Per-Ear Per-Channel Grading (Task 5): a per-ear reference IS loaded, so the verdict is PER
            // EARCUP. Dirac hard-pans its sweeps; each earcup is graded against the channel that drove it,
            // so the two verdicts are independent and we show TWO lines (L on statusLine, R on statusLineR).
            // This REPLACES both the old single reference-monitor branch and the green "captured (earcup)"
            // branch for reference adopters — those collapsed everything to ONE line and one earcup. Every
            // hard/global guard above (invalid capture, Idle/Preflight pre-sweep activity, output-clip,
            // combined low-SNR, the 48k veto) still has precedence and short-circuits before we get here,
            // leaving statusLineR blank under them. renderPerEarStatusLines mirrors the unratified-INFO gate
            // (kIrThresholdsRatified) so a clean ~13 dB ESS shows the numbers as INFO, never a false warn.
            // (This branch is now reached only when referenceLoaded() but NO ear has graded yet — the graded
            // case is intercepted by the hasGrade branch above.) It commits both lines via set-if-changed.
            renderPerEarStatusLines();
            perEarDriven = true;
        } else if (h.session == SessionPhase::Complete) {
            // Sweep finished clean: an honest sweep-scoped all-clear (no dropouts / clipping seen during
            // the captured sweep), with the OS-resampled caveat when the input ran through OS SRC.
            const auto refState = (eb::RefMonState) engine.refMonState();
            juce::String msg = "Sweep captured - no clipping or dropouts detected";
            // The sweepSNR gate (ACTIVE) decides the headline: only GradedClean reads "verified" (green);
            // GradedMarginal = amber (usable, re-measure), GradedSuspect = red (noisy/distorted) — neither may
            // read as verified. "captured earcup" = Auto per-ear grades a SINGLE earcup per measurement (honesty).
            sCol = Theme::ok();
            if (refState == eb::RefMonState::GradingOffHardware) {
                // Hardware Dirac committed: no loopback reference -> grading off. Calm/neutral, not a warning.
                msg  = "Sweep captured - reference grading off (hardware Dirac); per-ear calibration still active";
                sCol = Theme::textDim();
            } else if (engine.autoDetectedHardwareDirac() && ! settings.diracHardwareProcessor()) {
                // Auto-detect SUGGESTION (never auto-applies): the box was detected; point to the Advanced toggle.
                msg  = "Sweep captured - looks like a hardware Dirac processor; turn it on in Advanced to silence the grade";
                sCol = Theme::textDim();
            } else if (refState == eb::RefMonState::GradedClean) {
                const int snr = juce::roundToInt (engine.refIrSnrDb());
                const int thd = juce::roundToInt (engine.refThdPercent());
                msg = "Sweep captured + verified against the reference (captured earcup) - IR-SNR "
                    + juce::String (snr) + " dB, THD " + juce::String (thd) + "% (calibration pending)";
            } else if (refState == eb::RefMonState::GradedMarginal) {
                msg  = "Sweep captured - marginal SNR; usable, consider re-measuring (captured earcup)";
                sCol = Theme::warn();
            } else if (refState == eb::RefMonState::GradedSuspect) {
                msg  = "Sweep captured but flagged - noisy or distorted, re-measure (captured earcup)";
                sCol = Theme::danger();
            }
            if (any (h.flags & HealthFlag::OsResampled)) msg += " (OS-resampled - approximate)";
            sText = msg;
        } else if (silentTicks_ >= kSilentHoldTicks) {
            // "clean" only means no dropouts -- a connected-but-silent EARS reads clean too. After ~2 s
            // of genuinely below-floor input (debounced in timerCallback so normal gaps don't flicker),
            // say so, since ambient room noise alone (~-30 dB) sits well above the -50 dB floor.
            sText = "Running - no input signal (check the EARS)";
            sCol  = Theme::warn();
        } else if (lowLevelTicks_ >= kLowLevelHoldTicks) {
            // Signal present but the capture never reached a healthy level: a measurement this quiet has
            // poor SNR and reads "tin-can", yet without this it would show "clean" (clean = no dropouts).
            // Point the user at the meter target band, not an absolute dB they can't read.
            sText = "Running - level low: turn your amp up to the green band";
            sCol  = Theme::warn();
        } else {
            // In-sweep (SweepActive), no warning latched: clean so far.
            sText = "Capturing the Dirac sweep - clean so far";
            sCol  = Theme::ok();
        }
    } else if (st == EngineStatus::Error) {
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
            sText = "Advanced override on - not the standard Dirac path.";
            sCol  = Theme::textDim();
        } else if (bothSideUnknown) {
            sText = "Couldn't confirm left/right from these files - double-check the slots.";
            sCol  = Theme::textDim();
        } else if (chainVerdict_.checked && ! chainVerdict_.all48k) {
            // 48k-everywhere warning surfaced PRE-START: devices + cals are ready and no higher-precedence
            // gate (device/mode/override/side) is pending, but the chain isn't all-48k. Warn now so the user
            // fixes the rate BEFORE running a sweep the reference monitor would have to invalidate.
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
            sText = "Preparing calibration...";
            sCol  = Theme::textDim();
        }
    } else {
        sText = "Load both ear calibrations to start";
        sCol  = Theme::textDim();
    }

    // SECONDARY chain-config advisory (input-channels warn / 16-bit / non-2ch). Append it as a calm INFO
    // tail ONLY when the RATE is fine (all48k) and the chosen statusLine state is CALM (ok-green or dim, never
    // the red error or an amber warn). This must NOT override a hard error or the rate warn, and must NOT veto
    // green: a verified line still shows, with the advisory hanging off it as a quiet note. We fold it into the
    // statusLine text BEFORE the single commit (set-if-changed), so the appended line is computed once and
    // stays put — no clear-then-reset re-append every tick (Bug B). For perEarDriven branches the per-ear
    // renderer owns statusLine; we apply the same idempotent append to whatever it committed, guarded so the
    // advisory is never double-appended on a subsequent tick.
    const bool wantAdvisory = chainVerdict_.checked && chainVerdict_.all48k
                           && chainVerdict_.advisory.isNotEmpty();
    if (! perEarDriven) {
        if (wantAdvisory && ((sCol == Theme::ok()) || (sCol == Theme::textDim())))
            sText = sText.isNotEmpty() ? (sText + " - " + chainVerdict_.advisory) : chainVerdict_.advisory;
        // SINGLE COMMIT (Bug B: set-if-changed, no clear-then-reset). Each branch wrote the desired
        // statusLine/statusLineR (text, colour, tooltip) into locals; commit once, writing each label only
        // when it actually changed so the lines persist and the notes don't flicker.
        setLabelIfChanged (statusLine,  sText,  sCol);
        setTooltipIfChanged (statusLine,  sTip);
        setLabelIfChanged (statusLineR, s2Text, s2Col);
        setTooltipIfChanged (statusLineR, s2Tip);
    } else if (wantAdvisory) {
        // perEarDriven: the per-ear renderer already committed statusLine via set-if-changed. Append the
        // advisory to it idempotently (only when calm and not already suffixed), again set-if-changed.
        const auto curCol = statusLine.findColour (juce::Label::textColourId);
        const bool calm   = (curCol == Theme::ok()) || (curCol == Theme::textDim());
        const auto cur    = statusLine.getText();
        const auto tail   = " - " + chainVerdict_.advisory;
        if (calm && ! cur.endsWith (tail))
            setLabelIfChanged (statusLine, cur.isNotEmpty() ? (cur + tail) : chainVerdict_.advisory, curCol);
    }
}

// Per-Ear Per-Channel Grading (Task 5): drive BOTH per-ear status lines. Called ONLY from the Running +
// reference-loaded path in updateStatusLine — the hard/global ladder above it (device error, 48k veto,
// pre-sweep activity, output-clip, combined low-SNR) has precedence and short-circuits before this. Pure
// DISPLAY: it reads the ear-indexed engine getters and never touches grading, the match-gate, or thresholds.
void MainComponent::renderPerEarStatusLines() {
    renderEarStatusLine (0, "L", statusLine);
    renderEarStatusLine (1, "R", statusLineR);
}

// Build ONE ear's status line from THAT ear's published refMonState + IR-SNR / THD / sweep-SNR (ear 0 = L,
// 1 = R). The wording mirrors the (now-replaced) single-line ladder but scoped to one earcup:
//   GradedClean   -> ok-colour "L: verified - IR-SNR N dB, THD N% (calibration pending)"   (INFO numbers);
//   GradedSuspect -> while the cutoffs are UNRATIFIED, the SAME neutral/ok INFO line, NOT a warn (a clean
//                    deconvolved ESS reads only ~13 dB and would false-warn) — mirrors kIrThresholdsRatified;
//                    once ratified, a real below-cutoff measurement reads warn "L: low quality - N dB IR-SNR";
//   ReferenceStale -> warn "L: re-learn the reference" (the match-gate failed -> nothing to grade);
//   Learned / NotGraded / NotLearned -> dim "L: waiting for the sweep" (this ear hasn't graded yet this run).
// Per-ear low SNR (this ear's sweep ran too close to the room floor) appends a short " (low SNR)". Lines are
// kept SHORT for the title-bar width. NO grading logic here — display only.
void MainComponent::renderEarStatusLine (int ear, const char* prefix, juce::Label& label) {
    const auto  state    = (eb::RefMonState) engine.refMonState (ear);
    const juce::String p = juce::String (prefix) + ": ";
    // This ear's sweep-to-room-noise SNR is published (>0) only when it graded with a valid SNR; flag low
    // only then — a 0 (not graded / SNR not assessable) must NOT read as "low SNR".
    const float sweepSnr = engine.refSweepSnrDb (ear);
    const bool  lowSnr   = sweepSnr > 0.0f && sweepSnr < eb::kMinSweepSnrDb;
    const juce::String snrTail = lowSnr ? juce::String (" (low SNR)") : juce::String();

    juce::String text;
    juce::Colour col;
    juce::String tip;

    if (state == eb::RefMonState::GradingOffHardware) {
        // Hardware Dirac: no loopback reference to grade against. CALM/neutral - NOT a warning, NOT green.
        // Falls through the if-chain to the normal label application; the peak tail skips (not a graded state).
        text = p + "grading off - hardware Dirac; per-ear calibration still active";
        col  = Theme::textDim();
    } else if (state == eb::RefMonState::GradedClean) {
        const int snr = juce::roundToInt (engine.refIrSnrDb    (ear));
        const int thd = juce::roundToInt (engine.refThdPercent (ear));
        text = p + "verified - IR-SNR " + juce::String (snr) + " dB, THD "
             + juce::String (thd) + "% (calibration pending)";
        col  = Theme::ok();
    } else if (state == eb::RefMonState::GradedMarginal || state == eb::RefMonState::GradedSuspect) {
        // The sweepSNR gate (ACTIVE): GradedMarginal = orange (marginal noise, still usable); GradedSuspect =
        // red (noisy capture or gross >10% distortion). Reconstruct THIS ear's bands from its published metrics
        // so the note matches the headline verdict exactly (qualityNote). The IR-SNR/THD raw numbers remain
        // calibration-pending, but the sweepSNR gate itself ships live.
        const int snr = juce::roundToInt (engine.refIrSnrDb    (ear));
        const int thd = juce::roundToInt (engine.refThdPercent (ear));
        eb::QualityVerdict v;
        v.state        = state;
        v.sweepSnrBand = eb::classifySweepSnr (sweepSnr, sweepSnr != 0.0f);
        v.irSnrBand    = eb::classifyIrSnr (engine.refIrSnrDb (ear));
        v.thdBand      = eb::classifyThd (engine.refThdPercent (ear));
        const juce::String note = eb::qualityNote (v);
        text = p + (note.isNotEmpty() ? note : juce::String ("re-measure"));
        col  = (state == eb::RefMonState::GradedMarginal) ? Theme::warn() : Theme::danger();
        tip  = "Graded against the reference: sweep-SNR "
             + (sweepSnr != 0.0f ? juce::String (sweepSnr, 0) + " dB" : juce::String ("n/a"))
             + ", IR-SNR " + juce::String (snr) + " dB, distortion " + juce::String (thd) + "%.";
    } else if (state == eb::RefMonState::ReferenceStale) {
        // The match-gate failed for this ear -> there is nothing to grade; re-learn the reference.
        text = p + "re-learn the reference";
        col  = Theme::warn();
        tip  = "This earcup's sweep didn't match the learned reference channel (the gate failed). Re-learn "
               "the reference in Dirac's Windows Audio mode (Advanced -> Learn reference), then measure again.";
    } else {
        // Learned / NotGraded / NotLearned: this ear hasn't produced a gradeable sweep yet this run.
        text = p + "waiting for the sweep";
        col  = Theme::textDim();
    }

    // GAIN-STAGING READOUT (this ear's RAW input sweep peak in dBFS). The grade ring is fed the RAW pre-processing
    // mic capture, so the published peak IS how hot the EARS float ran for THIS earcup. It is NOT clamped at 0, so a
    // float that overshot full-scale reads POSITIVE (e.g. +1.6). Only meaningful once this ear graded (the engine
    // resets it to the -120 floor otherwise), so gate on the two GRADED states. The correction is ONE shared output
    // level, but reported PER EAR since one earcup can be hotter than the other.
    //   peak >= 0 dBFS      -> WARN: clip, lower the output by ceil(peak)+3 dB (round up + a 3 dB margin). This WARN
    //                          OVERRIDES the IR-SNR/THD INFO on this ear's line — a clipped capture is the actionable
    //                          thing, and it must win over the neutral numbers above.
    //   peak in [-1, 0)     -> caution: right at clipping, ease the output down (still a warn-coloured override).
    //   peak in [-12, -1)   -> fine: append a SHORT neutral " (peak -N dBFS)" to the existing verdict line.
    //   peak < -18 dBFS     -> low: append " (peak -N dBFS - low)" (the existing "level low" guidance owns the warn).
    //   peak in [-18, -12)  -> nothing appended (a healthy-enough level; keep the line short).
    const bool graded   = state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedMarginal
                       || state == eb::RefMonState::GradedSuspect;
    const float peakDb   = engine.referenceSweepPeakDb (ear);
    const bool peakKnown = graded && peakDb > -119.0f;   // a real published peak (not the silent/never-graded floor)
    juce::String peakTail;   // a short neutral info tail appended to the verdict line (fine / low cases)
    if (peakKnown) {
        if (peakDb >= 0.0f) {
            // Clipped: quantify the overshoot and how much to lower the output. cut = ceil(peak) + 3 dB margin.
            const int peakWhole = juce::roundToInt (std::ceil (peakDb));   // +1.6 -> 2 (headroom to clear full scale)
            const int cutDb     = peakWhole + 3;                           // + a 3 dB margin (the spec's ceil(peak)+3)
            text = p + "clipped +" + juce::String (peakDb, 1) + " dBFS - lower the output ~"
                 + juce::String (cutDb) + " dB";
            col  = Theme::warn();
            tip  = "This earcup's sweep peaked at +" + juce::String (peakDb, 1) + " dBFS - it overshot full scale and "
                   "clipped. Lower Dirac's output (or the system level) by about " + juce::String (cutDb)
                 + " dB, then re-measure. The correction is one shared output level; the hotter earcup sets the cut.";
        } else if (peakDb >= -1.0f) {
            // Right at clipping: no headroom left. Ease the output down before it overshoots.
            text = p + "peaked " + juce::String (peakDb, 1) + " dBFS - right at clipping, ease the output down";
            col  = Theme::warn();
            tip  = "This earcup's sweep peaked at " + juce::String (peakDb, 1) + " dBFS - right at full scale with no "
                   "headroom. Ease the output down a few dB so the next sweep can't clip.";
        } else if (peakDb < -1.0f && peakDb >= -12.0f) {
            // A healthy peak: a short neutral note so the user sees their headroom, no warn.
            peakTail = " (peak " + juce::String (peakDb, 0) + " dBFS)";
        } else if (peakDb < -18.0f) {
            // Low level: the existing "level low" guidance owns the warn; just note the peak (don't duplicate a warn).
            peakTail = " (peak " + juce::String (peakDb, 0) + " dBFS - low)";
        }
    }

    // Commit via set-if-changed (Bug B): the per-ear verdict persists tick-to-tick and only repaints when the
    // ear's state/metrics actually change, so it never flickers.
    setLabelIfChanged (label, text + snrTail + peakTail, col);
    setTooltipIfChanged (label, tip);
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
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("EarsBridge");
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
            loadedReferenceL_.assign (fL, fL + nL);
            loadedReferenceR_.assign (fR, fR + nR);
            loadedReferenceRateL_ = 48000.0;         // v1: 48k-only (the capture asserted 48k)
            loadedReferenceRateR_ = 48000.0;
            // LEFT alias kept ONLY for the startup duration log (the grade path uses loadedReferenceL_/R_ directly).
            loadedReference_     = loadedReferenceL_;
            loadedReferenceRate_ = loadedReferenceRateL_;
            referenceStatePath_  = refL.getFullPathName();
            engine.setReferenceLoaded (true);
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
        learnRefResultLabel.setText ("Cancelling...", juce::dontSendNotification);
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

    // Snapshot the Dirac config before every learn (the loopback can only capture in Windows Audio mode).
    logDiracSnapshot ("before learn");

#if JUCE_WINDOWS
    // Read-only Dirac-mode detection: inform if it's not in Windows Audio (the loopback will be silent).
    const juce::String deviceType = eb::readDiracDeviceType();
    if (deviceType.isNotEmpty() && ! eb::diracDeviceTypeIsWindowsAudio (deviceType)) {
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Dirac is in \"" + deviceType + "\" - set it to Windows Audio AND turn off "
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
            ? ("Learning from \"" + diracDevice + "\" - run a Dirac measurement now (stops automatically when the sweep ends)...")
            : juce::String ("Learning from the default playback device - run a Dirac measurement now (stops automatically when the sweep ends)..."),
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
                    resultMsg = "Reference learned - both ears captured (L "
                                + juce::String (samplesL.size() / capRate, 1) + " s, R "
                                + juce::String (samplesR.size() / capRate, 1) + " s) - see tip to resume listening.";
                }
            }
        }
        juce::MessageManager::callAsync ([safe, ok, cancelled, resultMsg,
                                          samplesL = std::move (samplesL), samplesR = std::move (samplesR),
                                          capRate]() mutable {
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
                auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("EarsBridge");
                dir.createDirectory();
                auto refL = dir.getChildFile ("reference_L.f32");
                auto refR = dir.getChildFile ("reference_R.f32");
                refL.replaceWithData (samplesL.data(), samplesL.size() * sizeof (float));
                refR.replaceWithData (samplesR.data(), samplesR.size() * sizeof (float));
                auto refOld = dir.getChildFile ("reference.f32");
                if (refOld.existsAsFile()) refOld.deleteFile();   // obsolete mono ref -> remove on a successful re-learn
                mc->referenceStatePath_ = refL.getFullPathName();
                // Hold both channels IN MEMORY so the per-ear grade path (gradeOneEar) can deconvolve each
                // earcup against its own reference channel (ref_L / ref_R). loadedReference_/Rate_ is a LEFT
                // alias kept ONLY for the startup duration log — the grade reads loadedReferenceL_/R_ directly.
                mc->loadedReferenceL_     = samplesL;   // copy (moved-in but still readable here)
                mc->loadedReferenceR_     = samplesR;
                mc->loadedReferenceRateL_ = capRate;
                mc->loadedReferenceRateR_ = capRate;
                mc->loadedReference_      = mc->loadedReferenceL_;
                mc->loadedReferenceRate_  = capRate;
                mc->engine.setReferenceLoaded (true);
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
        const bool micHeard  = juce::jmax (engine.maxSweepPeakL(), engine.maxSweepPeakR()) > kHwMicSweepFloor;
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
    if (gradeOneEar (0, gradePollerL_, loadedReferenceL_, loadedReferenceRateL_)) return;
    gradeOneEar (1, gradePollerR_, loadedReferenceR_, loadedReferenceRateR_);
}

bool MainComponent::gradeOneEar (int ear, eb::ReferenceGradePoller& poller,
                                 const std::vector<float>& reference, double referenceRate) {
    // Grade ONE earcup against ITS OWN reference channel. Returns true iff it posted an off-thread grade job this
    // tick (so the caller can serialize the two ears under the single gradeInFlight_ guard). ear 0 = LEFT, 1 = RIGHT.
    if (reference.empty()) return false;                                 // that ear has no reference -> skip it
    if (! engine.gradingResponseReady (ear)) return false;              // no fresh ring window published for this ear

    const char* earTag = (ear == 1) ? "R" : "L";

    // Copy the FULL rolling-ring WINDOW for THIS ear out of the engine into our own buffer (the allocation lives
    // here, not the engine). The window is leading silence/room noise + the sweep somewhere inside it; the worker
    // aligns the reference inside it via cross-correlation, so we must NOT truncate it to the reference length.
    std::vector<float> window ((size_t) juce::jmax (1, (int) std::lround (engine.gradingResponseRate() * 28.0)), 0.0f);
    const int got = engine.snapshotGradeRing (ear, window.data(), (int) window.size());
    if (got <= 0) return false;
    window.resize ((size_t) got);

    // Fix 2 (honesty): guard the capture rate against THIS ear's reference rate. A measurement at 44.1k matched
    // against a 48k reference is the SAME chirp stretched ~8.8% — it can still clear the provisional match cutoffs
    // and read green "verified", which is a lie. Both rates are known here, so if they differ by more than a tiny
    // tolerance, publish ReferenceStale FOR THIS EAR and do NOT grade it.
    const double respRate = engine.gradingResponseRate();
    if (referenceRate > 0.0 && respRate > 0.0 && std::abs (respRate - referenceRate) > 1.0) {
        poller.reset();   // a rate change is not a sweep -> re-arm so a later same-rate sweep can grade
        engine.setLastMatchCoherence (ear, 0.0f);
        engine.publishReferenceGrade (ear, (int) eb::RefMonState::ReferenceStale, 0.0f, 0.0f, /*mismatch*/ true, false);
        return false;
    }

    // The DECISION (match-gate FIRST + the two-consecutive-matched-polls STABLE debounce) lives in the headless
    // eb::ReferenceGradePoller so it can be self-tested without the GUI/hardware. decide() runs the cross-
    // correlation match on the message thread (cheap) and returns whether THIS poll should grade; it does NOT run
    // the heavy deconvolve+grade (that stays off-thread below). Publish THIS ear's coherence either way so the
    // diagnostic log + the RefMon-change line see it. A SILENT ring fails the gate here -> matched=false ->
    // didGrade=false -> nothing published (the silent-ear honesty gate).
    const auto d = poller.decide (window.data(), (int) window.size(),
                                  reference.data(), (int) reference.size());
    engine.setLastMatchCoherence (ear, d.coherence);
    logLine (eb::DiagnosticLog::Level::Debug,
             juce::String ("GradePoll: ear=") + earTag
             + " coher=" + juce::String (d.coherence, 3)
             + " mainLobe=" + juce::String (d.mainLobe, 3)
             + " matched=" + juce::String (d.matched ? "1" : "0")
             + " align=" + juce::String (d.alignOffset)
             + " winLen=" + juce::String ((int) window.size())
             + " didGrade=" + juce::String (d.didGrade ? "1" : "0"));
    if (! d.didGrade) return false;                                      // need a stable, not-yet-graded match

    gradeInFlight_.store (true);
    juce::Component::SafePointer<MainComponent> safe (this);
    auto refCopy = reference;                          // copy for the worker (message-thread snapshot)
    const double rate = referenceRate;
    const int refLen = (int) refCopy.size();
    const int alignOffset = d.alignOffset;             // where decide() located the sweep (Fix 1: reuse it)
    const float coherence = d.coherence;               // match-gate coherence — captured for the ratification log line
    firPool->addJob ([safe, ear, refCopy = std::move (refCopy), window = std::move (window), rate, refLen, alignOffset, coherence]() mutable {
        // OFFLINE on the worker: the pure grade for the window decide() said to grade. gradeWindow() grades at the
        // SAME offset decide() located (Fix 1 — gate and grade agree; no second xcorr), re-runs the match-gate
        // FIRST there, then quality — a non-sweep segment fails the gate -> stale.
        const auto g = eb::ReferenceGradePoller::gradeWindow (window.data(), (int) window.size(),
                                                              refCopy.data(), refLen, rate, alignOffset);
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
        juce::MessageManager::callAsync ([safe, ear, state, irSnr, thd, mismatch, lowQ, sweepSnr, snrValid, sweepPeak,
                                          snrBand, irBand, thdBand, coherence, alignOffset, refLen, rate]() {
            auto* mc = safe.getComponent();
            if (! mc) return;
            // Publish THIS ear's verdict snapshot (the SNR lesson: trio published together); raise the guidance
            // flag matching the verdict. NEITHER flag invalidates the capture (they are guidance only).
            mc->engine.publishReferenceGrade (ear, state, irSnr, thd, mismatch, lowQ);
            // 3-color quality dots: smooth THIS ear's bands across consecutive grades (anti-flicker) and show
            // them under that ear's status line. The raw dB/% is shown beside each dot (never colour alone).
            {
                auto& sm = (ear == 1) ? mc->smootherR_ : mc->smootherL_;
                auto& dv = (ear == 1) ? mc->gradeDotsR_ : mc->gradeDotsL_;
                const auto bands = sm.update (sweepSnr, snrValid, irSnr, thd);
                dv.setMetrics (bands.sweepSnr, snrValid ? juce::String (juce::roundToInt (sweepSnr)) + " dB" : juce::String(),
                               bands.irSnr,    juce::String (juce::roundToInt (irSnr)) + " dB",
                               bands.thd,      juce::String (thd, thd < 1.0f ? 2 : 1) + "%");
            }
            // Match-window sweep-SNR fix: publish THIS ear's sweep SNR and raise the GUIDANCE LowSnr flag when the
            // sweep ran too close to the room floor. Match-gate already passed (we only get here when g.didGrade),
            // so a non-sweep / silent ear never reaches this -> a silent ear NEVER raises LowSnr. GUIDANCE only.
            if (snrValid) {
                mc->engine.publishCompletedSweepSnrDb (ear, sweepSnr);
                if (sweepSnr < eb::kMinSweepSnrDb)
                    mc->engine.raiseLowSnr();
            }
            // Gain-staging readout: publish THIS ear's RAW input sweep peak (dBFS, clip reads positive) so the
            // per-ear status line can quantify the overshoot and suggest how much to lower the output. Always
            // published (no valid/invalid gate); GUIDANCE only — it does NOT invalidate the grade.
            mc->engine.publishCompletedSweepPeakDb (ear, sweepPeak);
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
            mc->gradeInFlight_.store (false);
        });
    });
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

    juce::String text;
    juce::Colour col;
    if (running && autoMode) {
        if (live) {
            text = (ear == 0) ? "Auto per-ear - capturing the LEFT earcup"
                              : "Auto per-ear - capturing the RIGHT earcup";
            col  = Theme::text();   // readable primary text; the meter's accent dot carries the colour cue
        } else {
            text = "Auto per-ear - waiting for the next sweep...";
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
    if (const auto in = inputPicker.selectedDevice())
        cfg.input = eb::readEndpointFormat (in->name, /*isInput*/ true);
    if (const auto out = outputPicker.selectedDevice())
        cfg.cable = eb::readEndpointFormat (out->name, /*isInput*/ false);   // the cable's CAPTURE-feeding render endpoint
    if (const auto diracOut = eb::readDiracOutputDeviceName(); diracOut.isNotEmpty())
        cfg.diracOutput = eb::readEndpointFormat (diracOut, /*isInput*/ false);

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
    // A device removed mid-run (unplug / sleep / gain-DIP re-enumerate) latches deviceDied_ from its
    // audioDeviceStopped() callback. Tear it down and surface it, instead of leaving the engine sitting
    // in a false "Running - clean" while the cable records silence.
    if (engine.status() == EngineStatus::Running && engine.consumeDeviceDied()) {
        statusErrorMsg_ = "EARS or audio cable disconnected - measurement stopped.";
        logLine (eb::DiagnosticLog::Level::Error, "Device lost mid-run: " + statusErrorMsg_);
        engine.onDeviceLost();              // closes devices, flips status -> Error
        startStop.setButtonText ("Start");
        updateStartGate();                  // -> updateStatusLine() renders statusErrorMsg_ in the Error branch
        return;
    }
    const auto lv = engine.levels();
    meterL.setLevel  (lv.inL,     lv.clipL);
    meterR.setLevel  (lv.inR,     lv.clipR);
    meterOut.setLevel (lv.outMono, lv.clipOut);
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
              : res == LrResult::Ambiguous ? juce::String ("Ambiguous - both mics responded (acoustic leak?)")
                                           : juce::String ("No signal - play a tone into the LEFT earcup"),
                juce::dontSendNotification);
        } else if (++verifyTicks > 150) {   // ~5 s at 30 Hz
            engine.endLrVerify();
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setColour (juce::Label::textColourId, Theme::warn());
            verifyResultLabel.setText ("No signal detected - play a tone into the LEFT earcup", juce::dontSendNotification);
        }
    }

    // Follow live system light/dark changes (~2 s cadence; cheap, no allocation on the hot path).
    if (++themeTick >= 60) {
        themeTick = 0;
        if (theme.syncMode()) {
            applyTextColours();
            applyTitleBarTheme();
            if (auto* top = getTopLevelComponent()) top->repaint(); else repaint();
        }
    }
}

// The rail content is transparent: the graphite rail backdrop + the right divider are painted by
// MainComponent (full window height, behind the Viewport) so they stay put while the content scrolls.
void MainComponent::RailContent::paint (juce::Graphics&) {}

void MainComponent::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
    const int barH = 56, railW = 262;
    auto bar = getLocalBounds().removeFromTop (barH);

    // Title bar + left rail backdrops. The rail fill spans the full window height below the bar; the
    // (transparent) Viewport scrolls its content over this static backdrop, so the graphite rail +
    // divider look identical to the old fixed rail at any scroll position.
    g.setColour (Theme::barBg());
    g.fillRect (bar);
    g.setColour (Theme::rail());
    g.fillRect (juce::Rectangle<int> (0, barH, railW, getHeight() - barH));

    // Separators.
    g.setColour (Theme::sep());
    g.fillRect (0, barH - 1, getWidth(), 1);
    g.fillRect (railW - 1, barH, 1, getHeight() - barH);

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

    // Levels card backdrop.
    if (! levelsBounds.isEmpty()) {
        g.setColour (Theme::surface());
        g.fillRoundedRectangle (levelsBounds.toFloat(), 10.0f);
    }
}

int MainComponent::layoutRail (int width) {
    // Lay every rail child out top-down inside railContent's local space (origin 0,0), in a column of
    // the given width reduced by the 16px gutter. Returns the TOTAL content height (last bottom + the
    // bottom gutter) so resized() can size railContent for the Viewport. Pure layout — no resized()
    // call here, so it can't recurse through the Viewport. Spacings mirror the former fixed-rail block.
    constexpr int gutter = 16;
    auto rr = juce::Rectangle<int> (0, 0, width, 100000).reduced (gutter, 0);
    rr.removeFromTop (gutter);   // top gutter (the old rail.reduced(16) inset, applied at y=0)

    inputPicker.setBounds (rr.removeFromTop (62));
    rr.removeFromTop (4);
    inputGainHint.setBounds (rr.removeFromTop (30));
    rr.removeFromTop (12);

    combineLabel.setBounds (rr.removeFromTop (16));
    rr.removeFromTop (6);
    combineBox.setBounds (rr.removeFromTop (40));
    rr.removeFromTop (6);
    combineHint.setBounds (rr.removeFromTop (44));
    rr.removeFromTop (16);

    outputPicker.setBounds (rr.removeFromTop (62));
    rr.removeFromTop (4);
    outputHint.setBounds (rr.removeFromTop (30));
    preflightLabel.setBounds (rr.removeFromTop (14));
    // The neutral fact line sits just below the warnings; it only claims a row when it has text,
    // so an empty info line never pushes the rest of the rail down.
    if (preflightInfo.getText().isNotEmpty())
        preflightInfo.setBounds (rr.removeFromTop (14));
    else
        preflightInfo.setBounds ({});
    if (diracCableHint.isVisible()) {
        rr.removeFromTop (6);
        diracCableHint.setBounds (rr.removeFromTop (48));
        if (diracFixButton.isVisible()) {
            rr.removeFromTop (4);
            diracFixButton.setBounds (rr.removeFromTop (30).removeFromLeft (200));
        }
    }
    rr.removeFromTop (12);

    auto rb = rr.removeFromTop (62);
    auto rcol = rb.removeFromLeft (rb.getWidth() / 2 - 8);
    rb.removeFromLeft (16);
    rateLabel.setBounds (rcol.removeFromTop (16)); rcol.removeFromTop (6);
    rateBox.setBounds (rcol.removeFromTop (40));
    bitLabel.setBounds (rb.removeFromTop (16)); rb.removeFromTop (6);
    bitBox.setBounds (rb.removeFromTop (40));
    rateWarn.setBounds (rr.removeFromTop (14));
    rr.removeFromTop (8);

    // Reference learn: a REQUIRED measurement step (it enables the per-ear quality grade), so it sits in the main
    // config flow above Advanced, not hidden inside it.
    learnRefButton.setBounds (rr.removeFromTop (30));
    rr.removeFromTop (4);
    learnRefResultLabel.setBounds (rr.removeFromTop (16));
    rr.removeFromTop (10);

    advancedToggle.setBounds (rr.removeFromTop (26));
    const bool adv = advancedToggle.getToggleState();
    complexPhaseToggle.setVisible (adv);
    firLenLabel.setVisible (adv); firLenBox.setVisible (adv);
    trimLabel.setVisible (adv);   trimSlider.setVisible (adv);
    verifyButton.setVisible (adv); verifyResultLabel.setVisible (adv);
    openLogButton.setVisible (adv); exportLogButton.setVisible (adv);
    autoUpdateToggle.setVisible (adv);
    overrideToggle.setVisible (adv);   // #3: only reachable with Advanced expanded
    hwDiracToggle.setVisible (adv);    // hardware-Dirac: only under Advanced (a deliberate, uncommon setup)
    if (adv) {
        rr.removeFromTop (4);
        complexPhaseToggle.setBounds (rr.removeFromTop (26));
        rr.removeFromTop (6);
        firLenLabel.setBounds (rr.removeFromTop (16)); rr.removeFromTop (6);
        firLenBox.setBounds (rr.removeFromTop (40));
        rr.removeFromTop (8);
        trimLabel.setBounds (rr.removeFromTop (16)); rr.removeFromTop (4);
        trimSlider.setBounds (rr.removeFromTop (28));
        rr.removeFromTop (10);
        verifyButton.setBounds (rr.removeFromTop (30));
        rr.removeFromTop (4);
        verifyResultLabel.setBounds (rr.removeFromTop (16));
        rr.removeFromTop (10);
        // Diagnostic-log export: the two buttons sit side by side on one rail row.
        {
            auto logRow = rr.removeFromTop (30);
            openLogButton.setBounds   (logRow.removeFromLeft (logRow.getWidth() / 2 - 4));
            logRow.removeFromLeft (8);
            exportLogButton.setBounds (logRow);
        }
        rr.removeFromTop (10);
        autoUpdateToggle.setBounds (rr.removeFromTop (26));
        rr.removeFromTop (6);
        overrideToggle.setBounds (rr.removeFromTop (26));
        hwDiracToggle.setBounds (rr.removeFromTop (26));
    }

    // Total content height = the y just past the last placed control, plus the matching bottom gutter.
    return rr.getY() + gutter;
}

void MainComponent::resized() {
    auto area = getLocalBounds();

    // --- Title bar ---
    auto bar = area.removeFromTop (56);
    {
        auto x = bar.reduced (16, 0);
        brandLabel.setBounds (x.removeFromLeft (200).withTrimmedLeft (24));
        // Transport button at the right, vertically centred; the status reads INLINE to its left
        // (running health / a gate reason), not stacked underneath.
        startStop.setBounds (x.removeFromRight (120).withSizeKeepingCentre (120, 34));
        x.removeFromRight (14);
        if (updateLink.isVisible()) {
            const int w = juce::jmin (230, x.getWidth());
            updateLink.setBounds (x.removeFromRight (w).withSizeKeepingCentre (w, 22));
            x.removeFromRight (12);
        }
        // Two stacked per-ear status lines (Task 5). Centre a 36px block (two 18px rows) in the title bar so
        // the pair sits where the single line used to. statusLine = upper (L), statusLineR = lower (R). When
        // only one message shows (the hard/global ladder), statusLineR is blanked so the upper line reads as
        // the single status line did. Same right-justification + width as before, so neither line truncates.
        auto stack = x.withSizeKeepingCentre (x.getWidth(), 68);
        statusLine.setBounds  (stack.removeFromTop (18));
        gradeDotsL_.setBounds (stack.removeFromTop (16));
        statusLineR.setBounds (stack.removeFromTop (18));
        gradeDotsR_.setBounds (stack.removeFromTop (16));
    }

    // --- Version footnote (pinned bottom-right, below both panes) ---
    versionLabel.setBounds (area.removeFromBottom (22).reduced (16, 2));

    // --- Left configuration rail (scrollable Viewport) ---
    // The Viewport fills the fixed 262px rail column. railContent is laid out + sized by layoutRail()
    // to its FULL content height, so the (tall, expandable) Advanced stack is always reachable by
    // scrolling — never clipped off the bottom as it was when the rail was a fixed rect.
    auto rail = area.removeFromLeft (262);
    auto pane = area;
    railViewport.setBounds (rail);
    {
        // Content width = the rail width minus the vertical scrollbar when it's shown, so a child laid
        // out to the full width isn't hidden behind the bar. getMaximumVisibleWidth() already accounts
        // for a visible vertical scrollbar.
        const int contentW = railViewport.getMaximumVisibleWidth();
        const int contentH = layoutRail (contentW);
        // Never shorter than the viewport, so the backdrop area is fully covered and there's no
        // dead band below the last control.
        railContent.setSize (contentW, juce::jmax (contentH, railViewport.getHeight()));
        // A first pass laid the children out at contentW assuming NO scrollbar; if adding the content
        // made the scrollbar appear (or vanish), getMaximumVisibleWidth() changed — relayout once at
        // the now-correct width so the children fit. One extra pass converges (the height is stable).
        const int finalW = railViewport.getMaximumVisibleWidth();
        if (finalW != contentW) {
            const int h2 = layoutRail (finalW);
            railContent.setSize (finalW, juce::jmax (h2, railViewport.getHeight()));
        }
    }

    // --- Right content pane ---
    {
        auto pp = pane.reduced (16);
        calEyebrow.setBounds (pp.removeFromTop (16));
        pp.removeFromTop (10);
        leftCal.setBounds (pp.removeFromTop (206));
        pp.removeFromTop (16);
        rightCal.setBounds (pp.removeFromTop (206));
        pp.removeFromTop (20);

        levelsBounds = pp.removeFromTop (104);
        auto lv = levelsBounds.reduced (16, 12);
        {
            // Section eyebrow with the gain-staging caption inline to its right (zero extra vertical).
            auto top = lv.removeFromTop (14);
            levelsEyebrow.setBounds (top.removeFromLeft (60));
            top.removeFromLeft (10);
            levelsHint.setBounds (top);
        }
        lv.removeFromTop (8);
        const int mh = lv.getHeight() / 3;
        meterL.setBounds   (lv.removeFromTop (mh));
        meterR.setBounds   (lv.removeFromTop (mh));
        meterOut.setBounds (lv.removeFromTop (mh));

        // Dirac Mic-gain caption: sits just below the Levels card, above the (conditional) clip hint.
        pp.removeFromTop (8);
        diracMicGainHint.setBounds (pp.removeFromTop (34));

        if (inputClipHint.isVisible()) {
            pp.removeFromTop (8);
            inputClipHint.setBounds (pp.removeFromTop (50));
        }
    }
}

} // namespace eb
