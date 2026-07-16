#include "BootstrapConfig.h"

#include <iostream>
#include <string_view>

namespace
{
void expect(bool condition, std::string_view message, bool& success)
{
    if (! condition)
    {
        std::cerr << message << '\n';
        success = false;
    }
}
} // namespace

int main()
{
    using namespace std::string_view_literals;

    bool success = true;

    expect(std::string_view{ssbb::bootstrap::kDisplayName} == "Sing Song Bing Bong"sv,
           "Unexpected application display name.",
           success);
    expect(std::string_view{ssbb::bootstrap::kVersion} == "0.1.0"sv,
           "Unexpected project version.",
           success);
    expect(std::string_view{ssbb::bootstrap::kJuceVersion} == "8.0.14"sv,
           "Unexpected JUCE pin.",
           success);
    expect(std::string_view{ssbb::bootstrap::kJuceCommit} == "2cdfca8feb300fb424002ba2c2751569e5bacb64"sv,
           "Unexpected JUCE commit pin.",
           success);
    expect(std::string_view{ssbb::bootstrap::kJuceLicense}.find("AGPLv3") != std::string_view::npos,
           "Expected the documented JUCE license path to mention AGPLv3.",
           success);

    return success ? 0 : 1;
}
