#pragma once
// A league pak team's alternate uniforms (pack.json "uniform_sets", content_pack_tp.cpp), which the Uniform Picker
// (uniform_picker.cpp) offers after home and away: a league team sits on an NFL block, so the NFL extra sets
// (<app folder>/uniforms/<franchise>/) don't apply to it.
#include <array>
#include <string>
#include <vector>

namespace blitz {
    struct LeagueUniformSet {
        std::string id, name;
        bool dark = true;
        std::array<std::string, 10> files;  // files/added names by part (uniform_picker.cpp part_names); "" keeps the away part
    };
    // By the team's index in the store (the pak's order); nullptr without a league pak or alternates.
    const std::vector<LeagueUniformSet>* league_uniform_sets(int team);
}
