#include "SongWizard.h"
#include <algorithm>

SongStructure SongWizard::createClassicRap(int verseBars, int hookBars, int numVerses)
{
    SongStructure result;
    verseBars = std::clamp(verseBars, 4, 64); hookBars = std::clamp(hookBars, 4, 32); numVerses = std::clamp(numVerses, 1, 8);
    result.sections.push_back({ SectionType::Intro, 4, {}, {} });
    for (int i = 0; i < numVerses; ++i) { result.sections.push_back({ SectionType::Verse, verseBars, {}, {} }); result.sections.push_back({ SectionType::Hook, hookBars, {}, {} }); }
    result.sections.push_back({ SectionType::Outro, 4, {}, {} });
    return result;
}
SongStructure SongWizard::createCustom(const std::vector<SongSection>& layout) { return { layout }; }
