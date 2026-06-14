#include "gui/MainComponent.h"

namespace eb {

MainComponent::MainComponent() {
    setLookAndFeel (&theme);
    firPool = std::make_unique<juce::ThreadPool> (1);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    addAndMakeVisible (inputPicker);

    // --- Cal slots ---
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);

    // --- Combine selector (ordered by rigor) ---
    combineLabel.setText ("COMBINE MODE", juce::dontSendNotification);
    combineLabel.setColour (juce::Label::textColourId, Theme::textDim());
    combineLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    addAndMakeVisible (combineLabel);

    combineModel = combineModeOrder();
    for (size_t i = 0; i < combineModel.size(); ++i) {
        auto& m = combineModel[i];
        juce::String label;
        switch (m.mode) {
            case CombineMode::TwoPassLeft:  label = "Two-pass: Left ear only";  break;
            case CombineMode::TwoPassRight: label = "Two-pass: Right ear only"; break;
            case CombineMode::Average:      label = "Average (L+R)/2";          break;
            case CombineMode::Sum:          label = "Sum L+R";                  break;
        }
        if (m.recommended)     label += "   [Recommended]";
        if (m.clipRiskWarning) label += "   (+6 dB clip risk)";
        combineBox.addItem (label, (int) i + 1);
    }
    combineBox.onChange = [this] { onCombineChosen(); };
    addAndMakeVisible (combineBox);

    combineHint.setText ("Two-pass single-ear mirrors miniDSP's official method: "
                         "route Dirac playback to one earcup per pass.",
                         juce::dontSendNotification);
    combineHint.setColour (juce::Label::textColourId, Theme::textDim());
    combineHint.setFont (juce::Font (juce::FontOptions (10.5f)));
    combineHint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (combineHint);

    // --- Output picker + hint + preflight ---
    outputPicker.onDeviceChosen = [this] (const DeviceId& d) { onOutputChosen (d); };
    addAndMakeVisible (outputPicker);

    outputHint.setText ("In Dirac Live, pick this device's CAPTURE side as the Recording device.",
                        juce::dontSendNotification);
    outputHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setFont (juce::Font (juce::FontOptions (10.5f)));
    addAndMakeVisible (outputHint);

    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightLabel.setFont (juce::Font (juce::FontOptions (10.5f)));
    addAndMakeVisible (preflightLabel);

    // --- Rate + bit depth ---
    rateLabel.setText ("SAMPLE RATE", juce::dontSendNotification);
    rateLabel.setColour (juce::Label::textColourId, Theme::textDim());
    rateLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    addAndMakeVisible (rateLabel);
    rateBox.onChange = [this] { onRateChosen(); };
    addAndMakeVisible (rateBox);
    rateWarn.setColour (juce::Label::textColourId, Theme::warn());
    rateWarn.setFont (juce::Font (juce::FontOptions (10.5f)));
    addAndMakeVisible (rateWarn);

    bitLabel.setText ("OUTPUT BIT DEPTH", juce::dontSendNotification);
    bitLabel.setColour (juce::Label::textColourId, Theme::textDim());
    bitLabel.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    addAndMakeVisible (bitLabel);
    bitBox.onChange = [this] { onBitDepthChosen(); };
    addAndMakeVisible (bitBox);

    // --- Meters ---
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    addAndMakeVisible (meterOut);
    cleanLight.setColour (juce::Label::textColourId, Theme::ok());
    cleanLight.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    cleanLight.setText ("clean", juce::dontSendNotification);
    addAndMakeVisible (cleanLight);

    // --- Transport ---
    startStop.onClick = [this] { onStartStop(); };
    addAndMakeVisible (startStop);
    statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    statusLine.setFont (juce::Font (juce::FontOptions (11.0f)));
    statusLine.setText ("Stopped", juce::dontSendNotification);
    addAndMakeVisible (statusLine);

    // --- Advanced ---
    addAndMakeVisible (advancedToggle);
    advancedToggle.onClick = [this] { resized(); };

    complexPhaseToggle.onClick = [this] {
        settings.setComplexPhase (complexPhaseToggle.getToggleState());
        rebuildFirsAsync();
    };
    addChildComponent (complexPhaseToggle);

    firLenLabel.setText ("FIR LENGTH", juce::dontSendNotification);
    firLenLabel.setColour (juce::Label::textColourId, Theme::textDim());
    firLenLabel.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")));
    addChildComponent (firLenLabel);
    // "Auto" (item id kFirLenAutoId) means taps scale with the rate via numTapsForRate();
    // the explicit overrides keep item id == tap count. 32768 stays valid for 192 kHz.
    firLenBox.addItem ("Auto (scales with rate)", kFirLenAutoId);
    for (int n : { 4096, 8192, 16384, 32768 }) firLenBox.addItem (juce::String (n), n);
    firLenBox.onChange = [this] {
        const int id = firLenBox.getSelectedId();
        settings.setFirLength (id == kFirLenAutoId ? 0 : id);   // 0 = Auto sentinel in Settings
        rebuildFirsAsync();
    };
    addChildComponent (firLenBox);

    trimLabel.setText ("OUTPUT TRIM (dB)", juce::dontSendNotification);
    trimLabel.setColour (juce::Label::textColourId, Theme::textDim());
    trimLabel.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")));
    addChildComponent (trimLabel);
    trimSlider.setRange (-24.0, 0.0, 0.1);
    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    trimSlider.onValueChange = [this] { settings.setOutputTrimDb (trimSlider.getValue()); };
    addChildComponent (trimSlider);

    // --- Restore persisted state ---
    combineBox.setSelectedId ((int) settings.combineMode() + 1, juce::dontSendNotification);
    complexPhaseToggle.setToggleState (settings.complexPhase(), juce::dontSendNotification);
    firLenBox.setSelectedId (settings.firLength() > 0 ? settings.firLength() : kFirLenAutoId,
                             juce::dontSendNotification);   // 0 (Auto) -> the Auto item
    trimSlider.setValue (settings.outputTrimDb(), juce::dontSendNotification);

    refreshDeviceLists();
    rebuildRateMenu();
    rebuildBitDepthMenu();

    if (settings.leftCalPath().isNotEmpty())
        leftCal.loadFromFile (juce::File (settings.leftCalPath()));
    if (settings.rightCalPath().isNotEmpty())
        rightCal.loadFromFile (juce::File (settings.rightCalPath()));

    setSize (980, 720);
    startTimerHz (30);   // poll levels()/health()
}

MainComponent::~MainComponent() {
    stopTimer();
    if (firPool) firPool->removeAllJobs (true, 2000);
    engine.stop();
    setLookAndFeel (nullptr);
}

double MainComponent::activeRate() const { return settings.sampleRate(); }

void MainComponent::refreshDeviceLists() {
    inputPicker.setDevices  (engine.inputDevices(),  settings.inputKey());
    outputPicker.setDevices (engine.outputDevices(), settings.outputKey());
}

void MainComponent::onInputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;   // configure only while Stopped
    engine.setInput (d);
    settings.setInputKey (d.key());
    settings.setInputModel (d.model);

    // Snap the selected rate to a native rate if the new device can't do the old one.
    auto rates = engine.supportedSampleRates (d);
    if (! rates.empty()) {
        bool ok = false;
        for (double r : rates) if (std::abs (r - settings.sampleRate()) < 0.5) ok = true;
        if (! ok) { settings.setSampleRate (rates.front()); engine.setSampleRate (rates.front()); }
    }
    rebuildRateMenu();
    rebuildBitDepthMenu();
    rebuildFirsAsync();   // taps depend on the rate, which may have just changed
    updateStatusLine();
}

void MainComponent::onOutputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    engine.setOutput (d);
    settings.setOutputKey (d.key());
    // Preflight note for the recommended Dirac flow.
    preflightLabel.setText (d.isVirtualSink ? juce::String()
                                            : "Selected output is not a known virtual sink.",
                            juce::dontSendNotification);
    rebuildBitDepthMenu();
    updateStatusLine();
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
    rateWarn.setText (warn ? "This rate is not native to the selected input - it will be resampled."
                           : juce::String(),
                      juce::dontSendNotification);
}

void MainComponent::rebuildBitDepthMenu() {
    auto sel = outputPicker.selectedDevice();
    // Offer the device's full supported output depths: EARS Pro -> {16,24,32}, EARS -> {24}.
    // 24-bit is the default; 32-bit is offered only where the sink accepts it (e.g. BlackHole)
    // and carries no measurement benefit beyond 24-bit. No clamp to {16,24}.
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
        // Persisted depth not offered by this device: default to 24-bit when available
        // (ample for measurement), else fall back to the first supported depth.
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
    rateWarn.setText (rateModel[(size_t) idx].resampleWarning
                          ? "This rate is not native to the selected input - it will be resampled."
                          : juce::String(),
                      juce::dontSendNotification);
    rebuildFirsAsync();   // taps scale with rate
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
    // Snapshot the inputs needed to design the FIRs, then run the (heavy) design off
    // the message thread and hand the result to the engine via setLeft/RightCalFir
    // (hot-swappable). A single-thread pool serialises rebuilds so a rapid rate change
    // can't race two designs onto the engine out of order.
    const double sr = activeRate();
    const int taps = (settings.firLength() > 0) ? settings.firLength() : numTapsForRate (sr);
    const auto mode = settings.complexPhase() ? FirMode::ComplexWithPhase
                                              : FirMode::MinPhaseMagnitude;
    auto left  = leftCal.calFile();
    auto right = rightCal.calFile();

    // Component::SafePointer (created HERE on the message thread) lets the result-marshalling
    // detect a destroyed MainComponent. The destructor's firPool->removeAllJobs(true, ...) waits
    // for a RUNNING design, but a callAsync already posted by a just-finished job would otherwise
    // fire on a dead `this` -> use-after-free on engine. Capture `safe` (not `this`) into the job
    // and each callAsync, and null-check before touching engine.
    juce::Component::SafePointer<MainComponent> safe (this);

    firPool->removeAllJobs (false, 0);   // drop a stale pending rebuild; let a running one finish
    firPool->addJob ([safe, sr, taps, mode, left, right] {
        FirDesignParams p;
        p.sampleRate = sr;
        p.numTaps    = taps;
        p.mode       = mode;
        p.invert     = true;
        if (left) {
            auto ir = FirDesigner::design (*left, p);
            juce::MessageManager::callAsync ([safe, ir = std::move (ir)]() mutable {
                if (auto* mc = safe.getComponent()) mc->engine.setLeftCalFir (std::move (ir));
            });
        }
        if (right) {
            auto ir = FirDesigner::design (*right, p);
            juce::MessageManager::callAsync ([safe, ir = std::move (ir)]() mutable {
                if (auto* mc = safe.getComponent()) mc->engine.setRightCalFir (std::move (ir));
            });
        }
    });
}

void MainComponent::onStartStop() {
    if (engine.status() == EngineStatus::Running) {
        engine.stop();
        startStop.setButtonText ("Start");
    } else {
        juce::String err;
        if (engine.start (err)) {
            startStop.setButtonText ("Stop");
        } else {
            preflightLabel.setText ("Start failed: " + err, juce::dontSendNotification);
        }
    }
    updateStatusLine();
}

void MainComponent::updateStatusLine() {
    switch (engine.status()) {
        case EngineStatus::Running:
            statusLine.setText ("Running  -  " + juce::String (activeRate() / 1000.0, 1)
                                + " kHz  -  " + juce::String (settings.outputBitDepth()) + "-bit",
                                juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
            break;
        case EngineStatus::Error:
            statusLine.setText ("Error", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::danger());
            break;
        default:
            statusLine.setText ("Stopped", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
            break;
    }
}

void MainComponent::timerCallback() {
    const auto lv = engine.levels();
    meterL.setLevel  (lv.inL,    lv.clipL);
    meterR.setLevel  (lv.inR,    lv.clipR);
    meterOut.setLevel (lv.outMono, lv.clipOut);

    const auto h = engine.health();
    cleanLight.setText (h.cleanCapture ? "clean" : "DROPOUTS", juce::dontSendNotification);
    cleanLight.setColour (juce::Label::textColourId, h.cleanCapture ? Theme::ok() : Theme::danger());

    if (engine.status() == EngineStatus::Running) {
        statusLine.setText ("Running  -  " + juce::String (activeRate() / 1000.0, 1)
                            + " kHz  -  " + juce::String (settings.outputBitDepth())
                            + "-bit  -  xruns " + juce::String (h.xruns),
                            juce::dontSendNotification);
    }
}

void MainComponent::paint (juce::Graphics& g) { g.fillAll (Theme::bg()); }

void MainComponent::resized() {
    auto r = getLocalBounds().reduced (16);

    // Row 1: input picker.
    inputPicker.setBounds (r.removeFromTop (48));
    r.removeFromTop (10);

    // Row 2: two cal slots side by side.
    auto calRow = r.removeFromTop (170);
    auto leftHalf = calRow.removeFromLeft (calRow.getWidth() / 2 - 6);
    calRow.removeFromLeft (12);
    leftCal.setBounds  (leftHalf);
    rightCal.setBounds (calRow);
    r.removeFromTop (10);

    // Row 3: combine selector + hint.
    combineLabel.setBounds (r.removeFromTop (16));
    combineBox.setBounds (r.removeFromTop (28));
    combineHint.setBounds (r.removeFromTop (28));
    r.removeFromTop (10);

    // Row 4: output picker + hint + preflight.
    outputPicker.setBounds (r.removeFromTop (48));
    outputHint.setBounds (r.removeFromTop (16));
    preflightLabel.setBounds (r.removeFromTop (16));
    r.removeFromTop (10);

    // Row 5: rate + bit depth side by side.
    auto rbRow = r.removeFromTop (60);
    auto rateCol = rbRow.removeFromLeft (rbRow.getWidth() / 2 - 6);
    rbRow.removeFromLeft (12);
    rateLabel.setBounds (rateCol.removeFromTop (16));
    rateBox.setBounds (rateCol.removeFromTop (28));
    rateWarn.setBounds (rateCol.removeFromTop (16));
    bitLabel.setBounds (rbRow.removeFromTop (16));
    bitBox.setBounds (rbRow.removeFromTop (28));
    r.removeFromTop (10);

    // Row 6: meters (fixed-width vertical bars) + clean light.
    auto meterRow = r.removeFromTop (120);
    auto mL = meterRow.removeFromLeft (60); meterRow.removeFromLeft (8);
    auto mR = meterRow.removeFromLeft (60); meterRow.removeFromLeft (8);
    auto mO = meterRow.removeFromLeft (60);
    meterL.setBounds   (mL);
    meterR.setBounds   (mR);
    meterOut.setBounds (mO);
    cleanLight.setBounds (meterRow.removeFromTop (20).reduced (8, 0));
    r.removeFromTop (10);

    // Row 7: transport.
    auto transport = r.removeFromTop (36);
    startStop.setBounds (transport.removeFromLeft (120));
    transport.removeFromLeft (12);
    statusLine.setBounds (transport);
    r.removeFromTop (8);

    // Row 8: advanced disclosure.
    advancedToggle.setBounds (r.removeFromTop (24));
    const bool adv = advancedToggle.getToggleState();
    complexPhaseToggle.setVisible (adv);
    firLenLabel.setVisible (adv); firLenBox.setVisible (adv);
    trimLabel.setVisible (adv);   trimSlider.setVisible (adv);
    if (adv) {
        complexPhaseToggle.setBounds (r.removeFromTop (24));
        firLenLabel.setBounds (r.removeFromTop (16));
        firLenBox.setBounds (r.removeFromTop (26).removeFromLeft (140));
        trimLabel.setBounds (r.removeFromTop (16));
        trimSlider.setBounds (r.removeFromTop (28));
    }
}

} // namespace eb
