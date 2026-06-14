#include <juce_gui_extra/juce_gui_extra.h>
#include "gui/MainComponent.h"

class EarsBridgeApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "EARS Bridge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override {
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override { window = nullptr; }

private:
    class MainWindow : public juce::DocumentWindow {
    public:
        MainWindow()
            : juce::DocumentWindow ("EARS Bridge",
                                    juce::Colour (0xff141619),
                                    juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar (true);
            setContentOwned (new eb::MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (EarsBridgeApp)
