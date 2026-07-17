#pragma once
#include <string>
#include <vector>

enum class SectionType { Intro, Verse, Hook, Bridge, Outro };

struct SongSection
{
    SectionType type;
    int bars = 16;
    std::string lyrics;
    std::vector<float> audio;
};
