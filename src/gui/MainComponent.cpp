#include "gui/MainComponent.h"
#include "gui/ClipStatus.h"
#include "gui/RawRailStatus.h"
#include "gui/StartGate.h"   // eb::startReady (Task 3 / #3 advanced override)
#include "gui/StartNotes.h"  // eb::buildStartNotes (Task 4 / #8 calm bit-depth note)
#include "platform/DiracCompat.h"
#include "cal/CalibrationPairValidator.h"   // eb::validateCalibrationPair (P0-07)
#include "audio/CalibrationGeneration.h"    // eb::CalibrationGeneration (generation lifecycle)
#include "audio/RefMonitor.h"               // eb::RefMonState / gradeMeasurement / refMonBlocksGreen (Plan 5)
#include "audio/LoopbackReference.h"        // eb::captureLoopback / validateReferenceCapture / readDiracDeviceType (Plan 5)
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

    // Update link: hidden until a newer release is found; opens the release page in the browser.
    updateLink.setColour (juce::HyperlinkButton::textColourId, Theme::accent());
    updateLink.setFont (juce::Font (juce::FontOptions (12.0f)), false, juce::Justification::centredRight);
    addChildComponent (updateLink);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    addAndMakeVisible (inputPicker);
    inputGainHint.setText ("Keep the EARS gain switch at its factory 18 dB - only lower it if the input clips.",
                           juce::dontSendNotification);
    inputGainHint.setColour (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setFont (juce::Font (juce::FontOptions (11.5f)));
    inputGainHint.setJustificationType (juce::Justification::topLeft);
    inputGainHint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (inputGainHint);

    // --- Combine selector ---
    styleEyebrow (combineLabel, "COMBINE MODE");
    addAndMakeVisible (combineLabel);
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
    addAndMakeVisible (combineBox);
    combineHint.setColour (juce::Label::textColourId, Theme::textDim());
    combineHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    combineHint.setJustificationType (juce::Justification::topLeft);
    combineHint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (combineHint);

    // --- Output picker + Dirac hint + preflight ---
    outputPicker.onDeviceChosen = [this] (const DeviceId& d) { onOutputChosen (d); };
    addAndMakeVisible (outputPicker);
    outputHint.setText ("In Dirac Live, choose this device's capture side as the recording input.",
                        juce::dontSendNotification);
    outputHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    outputHint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (outputHint);
    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (preflightLabel);
    // Calm, neutral fact line (NOT a warning): e.g. "Output: 32-bit float (shared mode) - normal."
    // The full honest explanation lives in its tooltip so the short line always fits one rail line.
    preflightInfo.setColour (juce::Label::textColourId, Theme::textDim());
    preflightInfo.setFont (juce::Font (juce::FontOptions (12.0f)));
    preflightInfo.setTooltip ("WASAPI shared mode always delivers 32-bit float; your bit-depth is a "
                              "stored preference and doesn't affect quality - this is expected.");
    addAndMakeVisible (preflightInfo);

    // Standard-VB-CABLE-vs-Dirac compatibility hint + one-click fix (hidden unless that cable is chosen).
    diracCableHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    diracCableHint.setJustificationType (juce::Justification::topLeft);
    diracCableHint.setColour (juce::Label::textColourId, Theme::warn());
    addChildComponent (diracCableHint);
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
    addChildComponent (diracFixButton);

    // --- Rate + depth ---
    styleEyebrow (rateLabel, "RATE");
    addAndMakeVisible (rateLabel);
    rateBox.onChange = [this] { onRateChosen(); };
    addAndMakeVisible (rateBox);
    rateWarn.setColour (juce::Label::textColourId, Theme::warn());
    rateWarn.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (rateWarn);
    styleEyebrow (bitLabel, "PREFERRED DEPTH");
    addAndMakeVisible (bitLabel);
    bitBox.onChange = [this] { onBitDepthChosen(); };
    addAndMakeVisible (bitBox);

    // --- Advanced disclosure ---
    advancedToggle.setButtonText ("Advanced");
    advancedToggle.onClick = [this] { resized(); };
    addAndMakeVisible (advancedToggle);
    complexPhaseToggle.setButtonText ("Complex (with-phase) FIR");
    complexPhaseToggle.onClick = [this] {
        settings.setComplexPhase (complexPhaseToggle.getToggleState());
        rebuildFirsAsync();
    };
    addChildComponent (complexPhaseToggle);
    autoUpdateToggle.setToggleState (settings.autoCheckUpdates(), juce::dontSendNotification);
    autoUpdateToggle.onClick = [this] {
        settings.setAutoCheckUpdates (autoUpdateToggle.getToggleState());
        settings.flush();
        if (! autoUpdateToggle.getToggleState() && updateLink.isVisible()) {
            updateLink.setVisible (false);
            resized();
        }
    };
    addChildComponent (autoUpdateToggle);
    // #3: advanced override toggle. Restore its persisted state; on click, persist + re-run the gate.
    overrideToggle.setToggleState (settings.advancedOverride(), juce::dontSendNotification);
    overrideToggle.onClick = [this] {
        settings.setAdvancedOverride (overrideToggle.getToggleState());
        settings.flush();
        updateStartGate();   // recompute Start enabled-ness + the status line for the new policy
    };
    addChildComponent (overrideToggle);
    styleEyebrow (firLenLabel, "FIR LENGTH");
    addChildComponent (firLenLabel);
    firLenBox.addItem ("Auto (scales with rate)", kFirLenAutoId);
    for (int n : { 4096, 8192, 16384, 32768 }) firLenBox.addItem (juce::String (n), n);
    firLenBox.onChange = [this] {
        const int id = firLenBox.getSelectedId();
        settings.setFirLength (id == kFirLenAutoId ? 0 : id);
        rebuildFirsAsync();
    };
    addChildComponent (firLenBox);
    styleEyebrow (trimLabel, "OUTPUT TRIM (dB)");
    addChildComponent (trimLabel);
    trimSlider.setRange (-24.0, 0.0, 0.1);
    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    trimSlider.onValueChange = [this] {
        settings.setOutputTrimDb (trimSlider.getValue());
        engine.setOutputTrimDb (trimSlider.getValue());   // apply live (the graph reads it lock-free)
    };
    addChildComponent (trimSlider);

    // L/R wiring check: play a tone into the LEFT earcup, then the engine reports which mic responded.
    verifyButton.onClick = [this] {
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
    addChildComponent (verifyButton);
    verifyResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    verifyResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    addChildComponent (verifyResultLabel);

    // Reference-Based Measurement Monitor (Plan 5): learn the loopback reference. The capture itself is a
    // Windows WASAPI loopback (on-device) and must run with Dirac's Processor in Windows Audio (shared)
    // mode; we detect that read-only and inform. Only meaningful while Stopped (the loopback can't run
    // alongside the live ASIO measurement).
    learnRefButton.onClick = [this] { onLearnReference(); };
    addChildComponent (learnRefButton);
    learnRefResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    addChildComponent (learnRefResultLabel);

    // --- Right pane: cal cards + Levels ---
    styleEyebrow (calEyebrow, "CALIBRATION");
    addAndMakeVisible (calEyebrow);
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f);  updateStartGate(); syncPlotScales(); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); updateStartGate(); syncPlotScales(); };
    // Removing a slot resets that ear to unity AND bumps a fresh (now-incomplete) generation, so the
    // Start gate (calibrationApplied()) closes instead of staying satisfied by the prior valid build.
    leftCal.onCalCleared  = [this] { settings.setLeftCalPath  ({}); engine.clearLeftCalFir();  rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    rightCal.onCalCleared = [this] { settings.setRightCalPath ({}); engine.clearRightCalFir(); rebuildFirsAsync(); updateStartGate(); syncPlotScales(); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);
    styleEyebrow (levelsEyebrow, "LEVELS");
    addAndMakeVisible (levelsEyebrow);
    levelsHint.setText ("Set your amp so the L and R meters reach the green band.", juce::dontSendNotification);
    levelsHint.setColour (juce::Label::textColourId, Theme::textDim());
    levelsHint.setFont (juce::Font (juce::FontOptions (11.5f)));
    levelsHint.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (levelsHint);
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
    inputClipHint.setText ("Input near full scale - if the meter hits the top, lower the EARS gain "
                           "switch a step and/or Dirac's playback level, then re-measure.",
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
}

void MainComponent::onBitDepthChosen() {
    const int idx = bitBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) bitModel.size()) return;
    settings.setOutputBitDepth (bitModel[(size_t) idx]);
    engine.setOutputBitDepth (bitModel[(size_t) idx]);
}

void MainComponent::onCombineChosen() {
    const int idx = combineBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) combineModel.size()) return;
    const auto mode = combineModel[(size_t) idx].mode;
    settings.setCombineMode (mode);
    engine.setCombineMode (mode);
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
    rebuildFirsAsync();
}
void MainComponent::onRightCalLoaded (const juce::File& f) {
    settings.setRightCalPath (f.getFullPathName());
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
        engine.stop();
        startStop.setButtonText ("Start");
    } else {
        if (verifyTicks > 0) {   // a pending L/R check holds the capture device; clear its GUI state
            verifyTicks = 0;
            verifyButton.setButtonText ("Check L/R wiring");
            verifyResultLabel.setText ({}, juce::dontSendNotification);
        }
        juce::String err;
        if (engine.start (err)) {
            startStop.setButtonText ("Stop");
            inputClipHold_ = 0; silentTicks_ = 0; lowLevelTicks_ = 0; lowSnrTicks_ = 0; statusErrorMsg_.clear();   // no prior-run state bleed
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
        } else {
            preflightLabel.setText ("Start failed: " + err, juce::dontSendNotification);
            preflightInfo.setText ({}, juce::dontSendNotification);
            resized();
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
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateCalProblems();
    updateControlsEnabled();
    updateStatusLine();
}

void MainComponent::updateCalProblems() {
    // A rejected pair (swap / serial mismatch / bad type) only shows in the status line today, which
    // gets buried under device/level warnings. Surface it LOUDLY on the offending cal card so a
    // left/right swap is impossible to miss. Only meaningful once both files are loaded AND the
    // current build has caught up (a stale diagnostic from the PREVIOUS generation must not flash).
    const bool building = engine.requestedGeneration() != engine.builtGeneration();
    const auto diag     = building ? juce::String() : engine.calibrationDiagnostic();
    const bool reject   = leftCal.hasCal() && rightCal.hasCal()
                       && ! engine.calibrationApplied() && diag.isNotEmpty();
    if (! reject) { leftCal.setProblem ({}); rightCal.setProblem ({}); return; }

    // Which ear does the diagnostic implicate? A swap names exactly one slot; map it to a plain
    // "this looks like the X cal, but it's in the Y slot" worded for that card. Serial/type problems
    // implicate both files, so the message goes on both. (Diagnostic strings come from
    // validateCalibrationPair: "Left calibration slot holds a file declaring the RIGHT side", etc.)
    const bool leftSlotSwapped  = diag.containsIgnoreCase ("Left calibration slot holds");
    const bool rightSlotSwapped = diag.containsIgnoreCase ("Right calibration slot holds");
    if (leftSlotSwapped || rightSlotSwapped) {
        if (leftSlotSwapped)
            leftCal.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
        else
            leftCal.setProblem ({});
        if (rightSlotSwapped)
            rightCal.setProblem ("This looks like the LEFT cal, but it's in the RIGHT slot - swap the files.");
        else
            rightCal.setProblem ({});
    } else {
        // Serial mismatch / HEQ / unknown-type: not ear-specific. Show the full reason on both cards.
        leftCal.setProblem (diag);
        rightCal.setProblem (diag);
    }
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
    combineHint.setColour    (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setColour  (juce::Label::textColourId, Theme::textDim());
    outputHint.setColour     (juce::Label::textColourId, Theme::textDim());
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

void MainComponent::updateStatusLine() {
    const auto st = engine.status();
    statusLine.setTooltip ({});   // only the low-SNR branch sets a tooltip; clear it on every other path
    if (st == EngineStatus::Running) {
        const auto h = engine.health();
        // An invalidating condition is reported the instant it latches, regardless of phase.
        if (! h.cleanCapture) {
            statusLine.setText (eb::invalidMeasurementMessage (h.flags), juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::danger());
        } else if (h.session == SessionPhase::Idle || h.session == SessionPhase::Preflight) {
            // Before the sweep arms: validity isn't scoped to anything yet, so don't claim "clean" and
            // suppress the in-sweep warnings below (output-clip / silent / low-level), which are only
            // meaningful once Dirac is actually driving the sweep.
            statusLine.setText ("Running - waiting for the Dirac sweep...", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
        } else if (any (h.flags & HealthFlag::ClipOutput)) {
            // The output hit full scale (e.g. Sum's uncompensated +6 dB drove past the clamp). The
            // clamp stops a cable over, but it distorts the sweep -- flag it, don't pass it as "clean".
            statusLine.setText ("Output clipping - lower the level or avoid Sum", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else if (lowSnrTicks_ >= kLowSnrHoldTicks) {
            // SNR guidance, BEFORE the green "Sweep captured" branch so a noisy sweep never reads as
            // "clean" (clean = no dropouts/clipping, which a low-SNR sweep can still pass). The engine
            // raised LowSnr at the sweep's edge off a TRUSTED floor only (honest silence otherwise).
            // The status line shares the title bar and is narrow, so keep the inline text SHORT and
            // complete (no clipping); the full guidance ("quieten the room or raise the level, then
            // re-measure") lives in the tooltip. snrNote's long form is the wording source of record.
            const int snrDb = juce::roundToInt (engine.completedSweepSnrDb());
            statusLine.setText ("Low SNR: sweep only " + juce::String (snrDb)
                                + " dB over the room noise - re-measure", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
            statusLine.setTooltip ("The Dirac sweep was only " + juce::String (snrDb)
                                   + " dB above the room-noise floor. Quieten the room (close windows, "
                                     "stop fans/AC) or raise the level, then re-measure for a cleaner "
                                     "correction.");
        } else if ((engine.referenceLoaded()
                    || (eb::RefMonState) engine.refMonState() != eb::RefMonState::NotLearned)
                   && refMonBlocksGreen ((eb::RefMonState) engine.refMonState())
                   // Fix 2 (review): while the IR-SNR/THD cutoffs are UNRATIFIED, GradedSuspect must NOT
                   // be treated as a green-blocking WARN — a clean deconvolved ESS reads only ~13 dB and
                   // would false-warn. Drop it OUT of this warn branch so it falls through to the green
                   // "captured" branch and shows the numbers as INFO instead. (The pure module still
                   // computes lowQuality for the ratification campaign; only the presentation is gated.)
                   && ! (! eb::kIrThresholdsRatified
                         && (eb::RefMonState) engine.refMonState() == eb::RefMonState::GradedSuspect)) {
            // Reference-Based Measurement Monitor — surfaced ABOVE the green "captured" branch so a NOT-
            // graded / mismatched / suspect measurement can NEVER read as a clean green capture. The
            // grading is GUIDANCE (it never invalidated cleanCapture above); the state machine decides the
            // wording. GradedClean is the only state that falls THROUGH to the green branch below (it is
            // NOT green-blocking, so refMonBlocksGreen excludes it here). This branch only engages once the
            // reference monitor is ENGAGED (a reference loaded, or a verdict already published) — when no
            // reference was ever learned (the NotLearned default) it falls through to the existing ladder,
            // so users who never opt into the monitor keep the unchanged green "Sweep captured".
            const auto state = (eb::RefMonState) engine.refMonState();
            if (state == eb::RefMonState::ReferenceStale) {
                statusLine.setText ("Reference doesn't match your sweep - re-learn", juce::dontSendNotification);
                statusLine.setColour (juce::Label::textColourId, Theme::warn());
                statusLine.setTooltip ("The deconvolution didn't match the learned reference (the gate "
                                       "failed). Re-learn the reference in Dirac's Windows Audio mode "
                                       "(Advanced -> Learn reference), then measure again.");
            } else if (state == eb::RefMonState::GradedSuspect) {
                // Reached ONLY when the cutoffs ARE ratified (the unratified case is excluded above and
                // shows the numbers as info in the green branch). A real below-cutoff measurement: warn.
                const int snr = juce::roundToInt (engine.refIrSnrDb());
                const int thd = juce::roundToInt (engine.refThdPercent());
                statusLine.setText ("Measurement quality low - " + juce::String (snr) + " dB IR-SNR",
                                    juce::dontSendNotification);
                statusLine.setColour (juce::Label::textColourId, Theme::warn());
                statusLine.setTooltip ("Graded against the reference: IR-SNR " + juce::String (snr)
                                       + " dB, distortion " + juce::String (thd)
                                       + "%. Below the clean cutoff - quieten the room or raise the level "
                                         "and re-measure.");
            } else {
                // NotLearned / Learned / NotGraded: a reference path exists but nothing clean is graded.
                statusLine.setText ("Not graded - learn a reference (Advanced)", juce::dontSendNotification);
                statusLine.setColour (juce::Label::textColourId, Theme::textDim());
            }
        } else if (h.session == SessionPhase::Complete) {
            // Sweep finished clean: an honest sweep-scoped all-clear (no dropouts / clipping seen during
            // the captured sweep), with the OS-resampled caveat when the input ran through OS SRC.
            const auto refState = (eb::RefMonState) engine.refMonState();
            const bool graded   = refState == eb::RefMonState::GradedClean
                               || refState == eb::RefMonState::GradedSuspect;   // both = matched + measured
            juce::String msg = "Sweep captured - no clipping or dropouts detected";
            // Fix 2: a matched measurement reads "verified". While the cutoffs are unratified we append the
            // IR-SNR/THD as INFO (neutral, in the ok-coloured line) rather than warning — a clean
            // measurement (~13 dB) must never read as a warning, but the numbers stay visible for tuning.
            if (refState == eb::RefMonState::GradedClean)
                msg = "Sweep captured + verified against the reference";
            else if (graded && ! eb::kIrThresholdsRatified) {
                const int snr = juce::roundToInt (engine.refIrSnrDb());
                const int thd = juce::roundToInt (engine.refThdPercent());
                msg = "Sweep captured + verified - IR-SNR " + juce::String (snr) + " dB, THD "
                    + juce::String (thd) + "% (calibration pending)";
            }
            if (any (h.flags & HealthFlag::OsResampled)) msg += " (OS-resampled - approximate)";
            statusLine.setText (msg, juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
        } else if (silentTicks_ >= kSilentHoldTicks) {
            // "clean" only means no dropouts -- a connected-but-silent EARS reads clean too. After ~2 s
            // of genuinely below-floor input (debounced in timerCallback so normal gaps don't flicker),
            // say so, since ambient room noise alone (~-30 dB) sits well above the -50 dB floor.
            statusLine.setText ("Running - no input signal (check the EARS)", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else if (lowLevelTicks_ >= kLowLevelHoldTicks) {
            // Signal present but the capture never reached a healthy level: a measurement this quiet has
            // poor SNR and reads "tin-can", yet without this it would show "clean" (clean = no dropouts).
            // Point the user at the meter target band, not an absolute dB they can't read.
            statusLine.setText ("Running - level low: turn your amp up to the green band", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else {
            // In-sweep (SweepActive), no warning latched: clean so far.
            statusLine.setText ("Capturing the Dirac sweep - clean so far", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
        }
    } else if (st == EngineStatus::Error) {
        // Render the specific reason (e.g. a device disconnect) so it survives a later re-render that
        // would otherwise clobber a directly-set label back to a bare "Error".
        statusLine.setText (statusErrorMsg_.isNotEmpty() ? statusErrorMsg_ : juce::String ("Error"),
                            juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::danger());
    } else if (! (inputPicker.selectedDevice().has_value()
                  && outputPicker.selectedDevice().has_value())) {
        statusLine.setText ("Select an input and output device", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    } else if (isRealEarsInput()
               && outputPicker.selectedDevice().has_value()
               && ! outputPicker.selectedDevice()->isVirtualSink
               && ! settings.advancedOverride()) {
        // P1-09: a real EARS into a physical output can't reach Dirac. Start is disabled;
        // point the user at the virtual cable. Skipped when the advanced override is on:
        // the user has explicitly opted into a non-Dirac configuration.
        statusLine.setText ("Select the virtual audio cable as the output to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::warn());
    } else if (isRealEarsWithCable()
               && settings.combineMode() != CombineMode::AutoPerEar
               && ! settings.advancedOverride()) {
        // D7 / R17: non-Auto combine mode selected with a real EARS + virtual cable.
        // Start is disabled; tell the user exactly why and what to change. Skipped when the
        // advanced override is on (the user has opted into a non-Dirac use).
        statusLine.setText ("Set Combine Mode to Auto per-ear (Dirac) to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::warn());
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
            statusLine.setText ("Advanced override on - not the standard Dirac path.",
                                juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
        } else if (bothSideUnknown) {
            statusLine.setText ("Couldn't confirm left/right from these files - double-check the slots.",
                                juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
        } else {
            statusLine.setText ({}, juce::dontSendNotification);
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
            statusLine.setText (diag, juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else {
            statusLine.setText ("Preparing calibration...", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
        }
    } else {
        statusLine.setText ("Load both ear calibrations to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    }
}

// ---- Reference-Based Measurement Monitor (Plan 5): learn + grade ----------------------
void MainComponent::onLearnReference() {
    // The learn capture is a Windows WASAPI loopback (on-device) — it can only succeed while Dirac's
    // Processor plays through "Windows Audio" (shared); in ASIO/exclusive the loopback is silent. We
    // detect the mode READ-ONLY from the settings file (never edit it — auto-switch is deferred to v2)
    // and warn before we even try. The capture + validate runs OFF the message thread (firPool).
    if (engine.status() == EngineStatus::Running) {
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Stop the bridge before learning a reference.", juce::dontSendNotification);
        return;
    }

#if JUCE_WINDOWS
    // Read-only Dirac-mode detection: inform if it's not in Windows Audio (the loopback will be silent).
    const juce::String deviceType = eb::readDiracDeviceType();
    if (deviceType.isNotEmpty() && ! eb::diracDeviceTypeIsWindowsAudio (deviceType)) {
        learnRefResultLabel.setColour (juce::Label::textColourId, Theme::warn());
        learnRefResultLabel.setText ("Dirac is in \"" + deviceType + "\" - switch it to Windows Audio first, "
                                     "then learn.", juce::dontSendNotification);
        return;
    }

    learnRefButton.setEnabled (false);
    learnRefResultLabel.setColour (juce::Label::textColourId, Theme::textDim());
    learnRefResultLabel.setText ("Learning - run a Dirac measurement now (~25 s)...", juce::dontSendNotification);

    // Fix 3 (review): do NOT hard-code "CABLE". Loopback-capture the render endpoint DIRAC PLAYS TO — which
    // is the device the user selected in the OUTPUT picker (the virtual cable feeding Dirac). Pass that
    // device's name as the loopback target so a differently-named cable (Voicemeeter, a renamed VB-CABLE,
    // a second cable) still works. Fall back to "CABLE" only when no output is selected, with a clear note.
    juce::String renderTarget = "CABLE";
    if (auto out = outputPicker.selectedDevice(); out.has_value() && out->name.isNotEmpty())
        renderTarget = out->name;

    juce::Component::SafePointer<MainComponent> safe (this);
    const double rate = activeRate();
    firPool->addJob ([safe, rate, renderTarget]() mutable {
        // Capture the loopback for the full L-then-R sweep sequence (~26 s), then validate the PURE core.
        // renderTarget is the OUTPUT picker's selected device name (the endpoint Dirac plays to).
        auto cap = eb::captureLoopback (renderTarget, 26.0, rate);   // the selected output render endpoint
        juce::String resultMsg; bool ok = false;
        std::vector<float> samples; double capRate = rate;
        if (! cap.ok) {
            resultMsg = "Capture failed: " + cap.reason;
        } else {
            const auto v = eb::validateReferenceCapture (cap.samples.data(), (int) cap.samples.size(), cap.rate);
            if (! v.ok) { resultMsg = "Rejected: " + v.reason; }
            else        { ok = true; samples = std::move (cap.samples); capRate = cap.rate;
                          resultMsg = "Reference learned (" + juce::String (samples.size() / capRate, 1) + " s)."; }
        }
        juce::MessageManager::callAsync ([safe, ok, resultMsg, samples = std::move (samples), capRate]() mutable {
            auto* mc = safe.getComponent();
            if (! mc) return;
            mc->learnRefButton.setEnabled (true);
            mc->learnRefResultLabel.setColour (juce::Label::textColourId, ok ? Theme::ok() : Theme::warn());
            mc->learnRefResultLabel.setText (resultMsg, juce::dontSendNotification);
            if (ok) {
                // Store the reference + metadata on disk and mark it loaded so the engine grades against it.
                auto md  = eb::makeReferenceMetadata (samples.data(), (int) samples.size(), capRate);
                auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("EarsBridge");
                dir.createDirectory();
                auto refFile = dir.getChildFile ("reference.f32");
                refFile.replaceWithData (samples.data(), samples.size() * sizeof (float));
                mc->referenceStatePath_ = refFile.getFullPathName();
                // Hold the reference IN MEMORY so pollReferenceGrade can deconvolve a measurement
                // against it OFFLINE (it pairs this with the engine's captured response buffer).
                mc->loadedReference_     = samples;     // copy (samples is moved-in but still readable here)
                mc->loadedReferenceRate_ = capRate;
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
    // Timer-driven: a sweep COMPLETED with a reference loaded (the engine set pendingGrade_ at the edge).
    // Run the OFFLINE deconvolution + gradeMeasurement on the worker (NOT the audio thread, NOT the message
    // thread — gradeMeasurement is two FFT passes), then publish the verdict via the engine atomics so
    // updateStatusLine reflects it. Both halves are now live: the learned reference is held in
    // loadedReference_ and the per-sweep mic response is buffered by the capture thread into the engine's
    // pre-allocated response buffer (copied out here via copyGradingResponse).
    // Check the act-on-it preconditions BEFORE draining pendingGrade_, so a grade isn't silently consumed
    // while a prior one is still running (it stays pending and the next poll picks it up).
    if (gradeInFlight_.load()) return;                                    // a prior grade is still running
    if (loadedReference_.empty() || ! engine.referenceLoaded()) return;   // nothing to grade against
    if (! engine.consumePendingGrade()) return;                          // no completed-sweep grade to run

    // Copy the ready response OUT of the engine into our own buffer (the allocation lives here, not the
    // engine). Sized to the reference length so deconvolve works on equal-length segments; copyGradingResponse
    // returns the number of valid samples it actually captured this sweep.
    std::vector<float> response (loadedReference_.size(), 0.0f);
    const int got = engine.copyGradingResponse (response.data(), (int) response.size());
    if (got <= 0) return;   // no fresh response ready (shouldn't happen when pendingGrade fired, but be safe)

    gradeInFlight_.store (true);
    juce::Component::SafePointer<MainComponent> safe (this);
    auto reference = loadedReference_;                 // copy for the worker (message-thread snapshot)
    const double rate = loadedReferenceRate_;
    const int n = juce::jmin ((int) reference.size(), (int) response.size());
    firPool->addJob ([safe, reference = std::move (reference), response = std::move (response), rate, n]() mutable {
        // OFFLINE on the worker: match-gate FIRST, then quality (gradeMeasurement enforces the order).
        auto g = eb::gradeMeasurement (reference.data(), response.data(), n, rate);
        const int   state   = (int) g.state;
        const bool  mismatch = (g.state == eb::RefMonState::ReferenceStale);
        const float irSnr   = g.quality.irSnrDb;
        const float thd     = g.quality.thdPercent;
        const bool  lowQ    = g.quality.lowQuality;
        juce::MessageManager::callAsync ([safe, state, irSnr, thd, mismatch, lowQ]() {
            auto* mc = safe.getComponent();
            if (! mc) return;
            // Publish the verdict snapshot (the SNR lesson: trio published together); raise the guidance
            // flag matching the verdict. NEITHER flag invalidates the capture (they are guidance only).
            mc->engine.publishReferenceGrade (state, irSnr, thd, mismatch, lowQ);
            mc->gradeInFlight_.store (false);
        });
    });
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
        text = "Set your amp so the L and R meters reach the green band.";
        col  = Theme::textDim();
    }
    // setColour no-ops when unchanged; setText only on a real change -> no per-tick repaint churn. The
    // timer is the sole owner of levelsHint's text+colour (so it is not touched in applyTextColours).
    levelsHint.setColour (juce::Label::textColourId, col);
    if (text != levelsHint.getText())
        levelsHint.setText (text, juce::dontSendNotification);
}

void MainComponent::timerCallback() {
    // A device removed mid-run (unplug / sleep / gain-DIP re-enumerate) latches deviceDied_ from its
    // audioDeviceStopped() callback. Tear it down and surface it, instead of leaving the engine sitting
    // in a false "Running - clean" while the cable records silence.
    if (engine.status() == EngineStatus::Running && engine.consumeDeviceDied()) {
        statusErrorMsg_ = "EARS or audio cable disconnected - measurement stopped.";
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
    if (engine.status() == EngineStatus::Running) updateStatusLine();

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

void MainComponent::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
    const int barH = 56, railW = 262;
    auto bar = getLocalBounds().removeFromTop (barH);

    // Title bar + left rail backdrops.
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
        statusLine.setBounds (x.withSizeKeepingCentre (x.getWidth(), 22));
    }

    // --- Version footnote (pinned bottom-right, below both panes) ---
    versionLabel.setBounds (area.removeFromBottom (22).reduced (16, 2));

    // --- Left configuration rail ---
    auto rail = area.removeFromLeft (262);
    auto pane = area;
    {
        auto rr = rail.reduced (16);
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

        advancedToggle.setBounds (rr.removeFromTop (26));
        const bool adv = advancedToggle.getToggleState();
        complexPhaseToggle.setVisible (adv);
        firLenLabel.setVisible (adv); firLenBox.setVisible (adv);
        trimLabel.setVisible (adv);   trimSlider.setVisible (adv);
        verifyButton.setVisible (adv); verifyResultLabel.setVisible (adv);
        learnRefButton.setVisible (adv); learnRefResultLabel.setVisible (adv);
        autoUpdateToggle.setVisible (adv);
        overrideToggle.setVisible (adv);   // #3: only reachable with Advanced expanded
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
            learnRefButton.setBounds (rr.removeFromTop (30));
            rr.removeFromTop (4);
            learnRefResultLabel.setBounds (rr.removeFromTop (16));
            rr.removeFromTop (10);
            autoUpdateToggle.setBounds (rr.removeFromTop (26));
            rr.removeFromTop (6);
            overrideToggle.setBounds (rr.removeFromTop (26));
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

        if (inputClipHint.isVisible()) {
            pp.removeFromTop (8);
            inputClipHint.setBounds (pp.removeFromTop (34));
        }
    }
}

} // namespace eb
