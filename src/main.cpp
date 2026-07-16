// main.cpp — application entry point for Sing Song Bing Bong
// The AudioEngine is owned by the Application object and outlives the window,
// ensuring the device manager is cleaned up after the UI is destroyed.
#include <juce_gui_extra/juce_gui_extra.h>

#include "BootstrapConfig.h"
#include "MainComponent.h"
#include "AudioEngine.h"

namespace
{

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(ssbb::AudioEngine& engine)
        : juce::DocumentWindow(ssbb::bootstrap::kDisplayName,
                               juce::Desktop::getInstance().getDefaultLookAndFeel()
                                   .findColour(juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentOwned(new ssbb::MainComponent(engine), true);
        centreWithSize(700, 560);
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
        // Engine must be alive before the window (which starts the timer)
        // and must outlive the window (which touches the engine in its destructor).
        engine_     = std::make_unique<ssbb::AudioEngine>();
        mainWindow_ = std::make_unique<MainWindow>(*engine_);
    }

    void shutdown() override
    {
        // Destroy window first (stops timer, detaches UI from engine),
        // then tear down the engine (stops device, removes callback).
        mainWindow_.reset();
        engine_.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<ssbb::AudioEngine> engine_;
    std::unique_ptr<MainWindow>        mainWindow_;
};

} // namespace

START_JUCE_APPLICATION(Application)
