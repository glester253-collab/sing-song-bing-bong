#pragma once
#include "SongStructure.h"

class SongWizard
{
public:
    SongStructure createClassicRap(int verseBars = 16, int hookBars = 8, int numVerses = 3);
    SongStructure createCustom(const std::vector<SongSection>& layout);
};
