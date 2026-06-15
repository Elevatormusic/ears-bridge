// Console diagnostic for Plan 2: lists devices with detected model + native rates/bit-depths,
// then opens an EARS/EARS-Pro input + a virtual sink and runs a ~5 s passthrough.
// Usage:  eb_diag                 -> list only
//         eb_diag run "<outName>" -> list + 5 s passthrough into the named output
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/AudioEngine.h"
#include "audio/ModelDetect.h"
#include <iostream>

static const char* modelName (eb::EarsModel m) {
    switch (m) { case eb::EarsModel::Ears: return "EARS";
                 case eb::EarsModel::EarsPro: return "EARS Pro";
                 default: return "—"; }
}

int main (int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;   // needed for device subsystem on some OSes
    eb::AudioEngine eng;

    std::cout << "== INPUT DEVICES ==\n";
    for (auto& d : eng.inputDevices()) {
        std::cout << "  [" << modelName (d.model) << "] " << d.name << "\n";
        std::cout << "      rates:";
        for (double r : eng.supportedSampleRates (d)) std::cout << " " << (int) r;
        std::cout << "   bits:";
        for (int b : eng.supportedBitDepths (d)) std::cout << " " << b;
        std::cout << "\n";
    }
    std::cout << "== OUTPUT DEVICES ==\n";
    for (auto& d : eng.outputDevices())
        std::cout << "  " << (d.isVirtualSink ? "[virtual] " : "          ") << d.name << "\n";

    if (argc >= 3 && juce::String (argv[1]) == "run") {
        // Pick the first recognised EARS/EARS-Pro input.
        eb::DeviceId chosenIn;
        for (auto& d : eng.inputDevices())
            if (d.model != eb::EarsModel::Unknown) { chosenIn = d; break; }
        if (chosenIn.name.isEmpty()) { std::cout << "No EARS input found.\n"; return 2; }

        eb::DeviceId chosenOut;
        for (auto& d : eng.outputDevices())
            if (d.name == juce::String (argv[2])) { chosenOut = d; break; }
        if (chosenOut.name.isEmpty()) { std::cout << "Output not found: " << argv[2] << "\n"; return 3; }

        auto rates = eng.supportedSampleRates (chosenIn);
        const double rate = rates.empty() ? 48000.0 : rates.front();
        eng.setInput (chosenIn); eng.setOutput (chosenOut);
        eng.setSampleRate (rate); eng.setOutputBitDepth (24);

        juce::String err;
        if (! eng.start (err)) { std::cout << "start failed: " << err << "\n"; return 4; }
        std::cout << "Passthrough running at " << (int) rate << " Hz; time-resolved health (200 ms steps):\n";
        // Time-resolved trace: sample health every 200 ms so a one-time STARTUP transient
        // (dropped jumps once then flatlines; fifoFill settles) is distinguishable from a
        // CONTINUOUS overrun (dropped grows every step). flags shows WHICH condition latched.
        auto flagStr = [] (eb::HealthFlag f) {
            juce::String s;
            auto add = [&] (eb::HealthFlag bit, const char* n) { if (eb::any (f & bit)) s << n << " "; };
            add (eb::HealthFlag::Xrun, "Xrun");          add (eb::HealthFlag::Dropout, "Dropout");
            add (eb::HealthFlag::FifoStarved, "FifoStarved"); add (eb::HealthFlag::ExcessDrift, "ExcessDrift");
            add (eb::HealthFlag::ClipInput, "ClipIn");    add (eb::HealthFlag::ClipOutput, "ClipOut");
            add (eb::HealthFlag::LowLevel, "LowLevel");
            return s.isEmpty() ? juce::String ("-") : s.trim();
        };
        long long firstDropAtMs = -1; long long prevDropped = 0;
        for (int step = 0; step < 40; ++step) {                 // 40 * 200 ms = 8 s
            juce::Thread::sleep (200);
            auto h = eng.health();
            const long long ms = (step + 1) * 200;
            if (firstDropAtMs < 0 && h.droppedFrames > 0) firstDropAtMs = ms;
            const long long ddelta = h.droppedFrames - prevDropped; prevDropped = h.droppedFrames;
            std::cout << "  t=" << ms << "ms dropped=" << h.droppedFrames << " (+" << ddelta << ")"
                      << " fifoFill=" << h.fifoFill << " ratio=" << h.captureToRenderRatio
                      << " clean=" << (h.cleanCapture ? "yes" : "no")
                      << " flags=[" << flagStr (h.flags) << "]\n";
        }
        auto h = eng.health(); auto lv = eng.levels();
        eng.stop();
        std::cout << "FINAL xruns=" << h.xruns << " dropped=" << h.droppedFrames
                  << " fifoFill=" << h.fifoFill << " clean=" << (h.cleanCapture ? "yes" : "no")
                  << " firstDropAt=" << firstDropAtMs << "ms\n";
        std::cout << "inL=" << lv.inL << " inR=" << lv.inR << " outMono=" << lv.outMono << "\n";
    }
    return 0;
}
