#include <juce_gui_extra/juce_gui_extra.h>
#include "gui/MainComponent.h"

#if JUCE_WINDOWS
 #include <dwmapi.h>
 #pragma comment(lib, "dwmapi.lib")
#endif

class EarsBridgeApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "EARS Bridge"; }
    const juce::String getApplicationVersion() override { return "0.2.12"; }
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
                                    juce::Desktop::getInstance().isDarkModeActive()
                                        ? juce::Colour (0xff1E1E1E) : juce::Colour (0xffECECEE),
                                    juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar (true);
            setContentOwned (new eb::MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

           #if JUCE_WINDOWS
            // Windows keeps the OS title bar light regardless of the app theme; match it to the
            // graphite UI so the window reads as one piece (macOS follows the system appearance).
            if (auto* hwnd = (HWND) getWindowHandle()) {
                BOOL dark = juce::Desktop::getInstance().isDarkModeActive() ? TRUE : FALSE;
                ::DwmSetWindowAttribute (hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof (dark));
            }
           #endif
        }
        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (EarsBridgeApp)
