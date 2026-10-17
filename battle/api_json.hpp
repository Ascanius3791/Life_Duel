#pragma once
// The JSON answers of the battle API, shared by the life_battle server and the
// WebAssembly engine that the static (GitHub Pages) build runs in the browser,
// so both speak exactly the same protocol.
#include "gol/battle.hpp"

#include <cstdint>
#include <string>

namespace web {
std::string jsonEscape(const std::string& text);
}

namespace duel {

// GET /api/config: arsenal, battlefield presets and board sizes.
std::string configJson();

// GET /api/battlefield: the grey cells of a preset, run-length encoded.
std::string battlefieldJson(const std::string& preset, int width, int height, int count, std::uint64_t seed);

// The settings of POST /api/battle. maxGenerations 0 means no limit.
gol::BattleConfig battleConfig(int width, int height, int maxGenerations, const std::string& variant);

// A battle's state as /api/battle and /api/step return it. The final frame
// also carries the (sampled) red/blue/grey history.
std::string frameJson(const std::string& id, const gol::Battle& battle, bool includeBoard);

}  // namespace duel
