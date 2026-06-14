#include <juce_gui_extra/juce_gui_extra.h>

class EarsBridgeApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "EARS Bridge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String&) override {
        window = std::make_unique<juce::DocumentWindow>(
            "EARS Bridge", juce::Colours::black,
            juce::DocumentWindow::allButtons);
        window->setContentOwned (new juce::Component(), true);
        window->centreWithSize (640, 400);
        window->setVisible (true);
    }
    void shutdown() override { window = nullptr; }

private:
    std::unique_ptr<juce::DocumentWindow> window;
};

START_JUCE_APPLICATION (EarsBridgeApp)
