#pragma once

// A league is the population of strategies found so far, all for one fixed
// battlefield and rule set. It is stored as a small text file so that
// training can be stopped and resumed at any time:
//
//   # life_arena league v1
//   width 260
//   ...
//   army <TAB> origin <TAB> gosper_gun@3,10,r1,m0;block@40,60,r0,m0
//
// The payoff matrix is not stored; it is recomputed on load.

#include "arena.hpp"

#include <string>
#include <utility>
#include <vector>

namespace arena {

struct LeagueFile {
    Settings settings;
    std::vector<std::pair<std::string, std::string>> armies;  // origin, pieces
};

bool readLeague(const std::string& path, LeagueFile& out, std::string& error);
bool writeLeague(const std::string& path, const Arena& arena, const std::vector<Army>& armies, std::string& error);

// Machine-readable export for other programs (e.g. a browser bot): settings,
// the Nash mixture and every army's placements for both colours.
bool writeLeagueJson(const std::string& path, const Arena& arena, const std::vector<Army>& armies,
                     const std::vector<double>& meta, const std::vector<std::vector<double>>& payoff,
                     std::string& error);

}  // namespace arena
