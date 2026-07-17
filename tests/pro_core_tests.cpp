#include "SongWizard.h"
#include "BeatEngine.h"
#include <array>
#include <cassert>
#include <cmath>

int main()
{
    SongWizard wizard;
    const auto song = wizard.createClassicRap(16, 8, 3);
    assert(song.sections.size() == 8);
    assert(song.sections.front().type == SectionType::Intro && song.sections.front().bars == 4);
    assert(song.sections[1].type == SectionType::Verse && song.sections[1].bars == 16);
    assert(song.sections[2].type == SectionType::Hook && song.sections[2].bars == 8);
    assert(song.sections.back().type == SectionType::Outro && song.sections.back().bars == 4);

    ssbb::BeatEngine beat;
    beat.loadWestCoastBounce();
    beat.prepare(48000.0, 512);
    std::array<float, 48000> left {}, right {};
    float* outputs[] { left.data(), right.data() };
    beat.render(outputs, 2, static_cast<int>(left.size()));
    bool nonSilent = false;
    for (float sample : left) { assert(std::isfinite(sample)); nonSilent = nonSilent || std::abs(sample) > 0.0001f; }
    assert(nonSilent);
    return 0;
}
