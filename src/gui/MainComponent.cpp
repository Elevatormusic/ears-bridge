#include "gui/MainComponent.h"
#include "gui/ClipStatus.h"
#include "gui/RawRailStatus.h"
#include "platform/DiracCompat.h"
#include "cal/CalibrationPairValidator.h"   // eb::validateCalibrationPair (P0-07)
#include "audio/CalibrationGeneration.h"    // eb::CalibrationGeneration (generation lifecycle)
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
    styleEyebrow (bitLabel, "DEPTH");
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
            inputClipHold_ = 0; silentTicks_ = 0; lowLevelTicks_ = 0; statusErrorMsg_.clear();   // no prior-run state bleed
            // Surface a silent format downgrade: WASAPI shared mode can grant a different rate/depth
            // than the user selected, which would otherwise resample with no indication.
            juce::StringArray notes;
            const double gr = engine.grantedSampleRate();
            if (std::abs (gr - settings.sampleRate()) > 0.5)
                notes.add ("Running at " + juce::String (gr / 1000.0, 1) + " kHz, not the selected "
                           + juce::String (settings.sampleRate() / 1000.0, 1) + " kHz (resampled).");
            const int gb = engine.grantedOutputBitDepth();
            if (gb > 0 && gb != settings.outputBitDepth())
                notes.add ("Cable opened " + juce::String (gb) + "-bit, not the selected "
                           + juce::String (settings.outputBitDepth()) + "-bit.");
            const auto railNote = eb::rawRailNote (engine.rawRail());
            if (railNote.isNotEmpty()) notes.add (railNote);
            preflightLabel.setText (notes.joinIntoString (" "), juce::dontSendNotification);
        } else {
            preflightLabel.setText ("Start failed: " + err, juce::dontSendNotification);
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
    const bool ready    = haveDevs && haveCals && ! wrongMode && ! physicalOutput;
    startStop.setEnabled (running || ready);
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateControlsEnabled();
    updateStatusLine();
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
    styleEyebrow (bitLabel,      "DEPTH");
    styleEyebrow (firLenLabel,   "FIR LENGTH");
    styleEyebrow (trimLabel,     "OUTPUT TRIM (dB)");
    styleEyebrow (calEyebrow,    "CALIBRATION");
    styleEyebrow (levelsEyebrow, "LEVELS");
    // levelsHint colour/text is owned by the timer (updateActiveEarIndicator), so it is not set here.
    combineHint.setColour    (juce::Label::textColourId, Theme::textDim());
    inputGainHint.setColour  (juce::Label::textColourId, Theme::textDim());
    outputHint.setColour     (juce::Label::textColourId, Theme::textDim());
    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
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
        } else if (h.session == SessionPhase::Complete) {
            // Sweep finished clean: an honest sweep-scoped all-clear (no dropouts / clipping seen during
            // the captured sweep), with the OS-resampled caveat when the input ran through OS SRC.
            juce::String msg = "Sweep captured - no clipping or dropouts detected";
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
               && ! outputPicker.selectedDevice()->isVirtualSink) {
        // P1-09: a real EARS into a physical output can't reach Dirac. Start is disabled;
        // point the user at the virtual cable.
        statusLine.setText ("Select the virtual audio cable as the output to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::warn());
    } else if (isRealEarsWithCable()
               && settings.combineMode() != CombineMode::AutoPerEar) {
        // D7 / R17: non-Auto combine mode selected with a real EARS + virtual cable.
        // Start is disabled; tell the user exactly why and what to change.
        statusLine.setText ("Set Combine Mode to Auto per-ear (Dirac) to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::warn());
    } else if (engine.calibrationApplied()) {
        // Ready: a valid generation is applied. No redundant label — the enabled Start button is the affordance.
        statusLine.setText ({}, juce::dontSendNotification);
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
    updateActiveEarIndicator (blockSilent);   // AutoPerEar: highlight the earcup being captured now
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
        autoUpdateToggle.setVisible (adv);
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
            autoUpdateToggle.setBounds (rr.removeFromTop (26));
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
