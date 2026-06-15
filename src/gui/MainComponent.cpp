#include "gui/MainComponent.h"
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

    // --- Transport (gated Start + status note) ---
    startStop.getProperties().set ("primary", true);
    startStop.onClick = [this] { onStartStop(); };
    addAndMakeVisible (startStop);
    statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    statusLine.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLine.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLine);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    addAndMakeVisible (inputPicker);

    // --- Combine selector ---
    styleEyebrow (combineLabel, "COMBINE MODE");
    addAndMakeVisible (combineLabel);
    combineModel = combineModeOrder();
    for (size_t i = 0; i < combineModel.size(); ++i) {
        auto& m = combineModel[i];
        juce::String label;
        switch (m.mode) {
            case CombineMode::TwoPassLeft:  label = "Two-pass: Left only";   break;
            case CombineMode::TwoPassRight: label = "Two-pass: Right only";  break;
            case CombineMode::Average:      label = "Average (L+R)/2";       break;
            case CombineMode::Sum:          label = "Sum L+R";               break;
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
    trimSlider.onValueChange = [this] { settings.setOutputTrimDb (trimSlider.getValue()); };
    addChildComponent (trimSlider);

    // --- Right pane: cal cards + Levels ---
    styleEyebrow (calEyebrow, "CALIBRATION");
    addAndMakeVisible (calEyebrow);
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f);  updateStartGate(); syncPlotScales(); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); updateStartGate(); syncPlotScales(); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);
    styleEyebrow (levelsEyebrow, "LEVELS");
    addAndMakeVisible (levelsEyebrow);
    meterL.setTitle ("Left input level");
    meterR.setTitle ("Right input level");
    meterOut.setTitle ("Output level");
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    addAndMakeVisible (meterOut);

    // --- Restore persisted state ---
    combineBox.setSelectedId ((int) settings.combineMode() + 1, juce::dontSendNotification);
    complexPhaseToggle.setToggleState (settings.complexPhase(), juce::dontSendNotification);
    firLenBox.setSelectedId (settings.firLength() > 0 ? settings.firLength() : kFirLenAutoId,
                             juce::dontSendNotification);
    trimSlider.setValue (settings.outputTrimDb(), juce::dontSendNotification);
    onCombineChosen();   // seed the combine helper text

    refreshDeviceLists();
    rebuildRateMenu();
    rebuildBitDepthMenu();

    if (settings.leftCalPath().isNotEmpty())
        leftCal.loadFromFile (juce::File (settings.leftCalPath()));
    if (settings.rightCalPath().isNotEmpty())
        rightCal.loadFromFile (juce::File (settings.rightCalPath()));

    updateStartGate();
    syncPlotScales();
    setSize (900, 700);
    startTimerHz (30);
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
    if (engine.status() == EngineStatus::Running) return;
    engine.setInput (d);
    settings.setInputKey (d.key());
    settings.setInputModel (d.model);
    auto rates = engine.supportedSampleRates (d);
    if (! rates.empty()) {
        bool ok = false;
        for (double r : rates) if (std::abs (r - settings.sampleRate()) < 0.5) ok = true;
        if (! ok) { settings.setSampleRate (rates.front()); engine.setSampleRate (rates.front()); }
    }
    rebuildRateMenu();
    rebuildBitDepthMenu();
    rebuildFirsAsync();
    updateStatusLine();
}

void MainComponent::onOutputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    engine.setOutput (d);
    settings.setOutputKey (d.key());
    preflightLabel.setText (d.isVirtualSink ? juce::String()
                                            : "Selected output is not a known virtual cable.",
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
        case CombineMode::Average: h = "Recommended. Averages both ears to one mono signal."; break;
        case CombineMode::Sum:     h = "Sums both ears (+6 dB). Watch for clipping.";          break;
        case CombineMode::TwoPassLeft:
        case CombineMode::TwoPassRight:
            h = "Mirrors miniDSP's official single-ear method: route Dirac playback to one earcup per pass.";
            break;
    }
    combineHint.setText (h, juce::dontSendNotification);
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
    const double sr = activeRate();
    const int taps = (settings.firLength() > 0) ? settings.firLength() : numTapsForRate (sr);
    const auto mode = settings.complexPhase() ? FirMode::ComplexWithPhase
                                              : FirMode::MinPhaseMagnitude;
    auto left  = leftCal.calFile();
    auto right = rightCal.calFile();
    juce::Component::SafePointer<MainComponent> safe (this);

    firPool->removeAllJobs (false, 0);
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
    updateStartGate();
}

void MainComponent::updateStartGate() {
    const bool running = engine.status() == EngineStatus::Running;
    const bool ready   = leftCal.hasCal() && rightCal.hasCal();
    startStop.setEnabled (running || ready);
    updateStatusLine();
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
    styleEyebrow (combineLabel,  "COMBINE MODE");
    styleEyebrow (rateLabel,     "RATE");
    styleEyebrow (bitLabel,      "DEPTH");
    styleEyebrow (firLenLabel,   "FIR LENGTH");
    styleEyebrow (trimLabel,     "OUTPUT TRIM (dB)");
    styleEyebrow (calEyebrow,    "CALIBRATION");
    styleEyebrow (levelsEyebrow, "LEVELS");
    combineHint.setColour    (juce::Label::textColourId, Theme::textDim());
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
        statusLine.setText (h.cleanCapture ? "Running - clean" : "Dropouts detected",
                            juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, h.cleanCapture ? Theme::ok() : Theme::danger());
    } else if (st == EngineStatus::Error) {
        statusLine.setText ("Error", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::danger());
    } else if (leftCal.hasCal() && rightCal.hasCal()) {
        statusLine.setText ("Ready", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    } else {
        statusLine.setText ("Load both ear calibrations to start", juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    }
}

void MainComponent::timerCallback() {
    const auto lv = engine.levels();
    meterL.setLevel  (lv.inL,     lv.clipL);
    meterR.setLevel  (lv.inR,     lv.clipR);
    meterOut.setLevel (lv.outMono, lv.clipOut);
    if (engine.status() == EngineStatus::Running) updateStatusLine();

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
    const float cx = 24.0f, cy = (float) bar.getCentreY(), rad = 8.0f;
    g.setColour (Theme::text());
    juce::Path band;
    band.addCentredArc (cx, cy, rad, rad, 0.0f,
                        -juce::MathConstants<float>::halfPi,
                         juce::MathConstants<float>::halfPi, true);
    g.strokePath (band, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (cx - rad - 1.5f, cy, 3.0f, 8.0f, 1.5f);
    g.fillRoundedRectangle (cx + rad - 1.5f, cy, 3.0f, 8.0f, 1.5f);

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
        auto col = x.removeFromRight (240).withSizeKeepingCentre (240, 49);  // centre the transport group
        startStop.setBounds (col.removeFromTop (32).removeFromRight (120));
        col.removeFromTop (2);
        statusLine.setBounds (col.removeFromTop (15));
    }

    // --- Left configuration rail ---
    auto rail = area.removeFromLeft (262);
    auto pane = area;
    {
        auto rr = rail.reduced (16);
        inputPicker.setBounds (rr.removeFromTop (62));
        rr.removeFromTop (16);

        combineLabel.setBounds (rr.removeFromTop (16));
        rr.removeFromTop (6);
        combineBox.setBounds (rr.removeFromTop (40));
        rr.removeFromTop (6);
        combineHint.setBounds (rr.removeFromTop (34));
        rr.removeFromTop (16);

        outputPicker.setBounds (rr.removeFromTop (62));
        rr.removeFromTop (4);
        outputHint.setBounds (rr.removeFromTop (30));
        preflightLabel.setBounds (rr.removeFromTop (14));
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
        if (adv) {
            rr.removeFromTop (4);
            complexPhaseToggle.setBounds (rr.removeFromTop (26));
            rr.removeFromTop (6);
            firLenLabel.setBounds (rr.removeFromTop (16)); rr.removeFromTop (6);
            firLenBox.setBounds (rr.removeFromTop (40));
            rr.removeFromTop (8);
            trimLabel.setBounds (rr.removeFromTop (16)); rr.removeFromTop (4);
            trimSlider.setBounds (rr.removeFromTop (28));
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
        levelsEyebrow.setBounds (lv.removeFromTop (14));
        lv.removeFromTop (8);
        const int mh = lv.getHeight() / 3;
        meterL.setBounds   (lv.removeFromTop (mh));
        meterR.setBounds   (lv.removeFromTop (mh));
        meterOut.setBounds (lv.removeFromTop (mh));
    }
}

} // namespace eb
