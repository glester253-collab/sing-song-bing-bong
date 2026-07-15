#include <juce_gui_extra/juce_gui_extra.h>

#include "BootstrapConfig.h"

namespace
{
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : juce::DocumentWindow(ssbb::bootstrap::kDisplayName,
                               juce::Desktop::getInstance().getDefaultLookAndFeel()
                                   .findColour(juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        setContentOwned(new juce::Component(), true);
        centreWithSize(640, 360);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class Application final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return ssbb::bootstrap::kDisplayName;
    }

    const juce::String getApplicationVersion() override
    {
        return ssbb::bootstrap::kVersion;
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace

START_JUCE_APPLICATION(Application)
