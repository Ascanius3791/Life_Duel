// The battle engine for the static (GitHub Pages) build: compiled to
// WebAssembly with Emscripten and driven by battle/web/engine-worker.js, which
// answers the same /api/... requests as the life_battle server. The answers
// come from the same C++ code (gol_core + api_json.cpp), so a battle in the
// browser is cell for cell the battle the server would run.
//
// Every function returns a JSON string that stays valid until the next call.
#include "api_json.hpp"

#include <algorithm>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <utility>

namespace {

std::string result;

const char* answer(std::string json) {
    result = std::move(json);
    return result.c_str();
}

const char* error(const std::string& message) {
    return answer("{\"error\":\"" + web::jsonEscape(message) + "\"}");
}

int clampInt(double v, int lo, int hi) {
    if (!(v >= lo)) return lo;  // also NaN
    if (v > hi) return hi;
    return static_cast<int>(v);
}

// Running battles, most recently used first. Same limit as the server.
struct Session {
    std::string id;
    gol::Battle battle;
};
std::list<Session> sessions;
std::uint64_t nextId = 1;
constexpr std::size_t kMaxSessions = 64;

Session* findSession(const std::string& id) {
    const auto it = std::find_if(sessions.begin(), sessions.end(), [&](const Session& s) { return s.id == id; });
    if (it == sessions.end()) return nullptr;
    sessions.splice(sessions.begin(), sessions, it);
    return &sessions.front();
}

}  // namespace

extern "C" {

const char* engine_config() { return answer(duel::configJson()); }

// Same parameter ranges as GET /api/battlefield.
const char* engine_battlefield(const char* preset, double w, double h, double count, double seed) {
    return answer(duel::battlefieldJson(preset, clampInt(w, 50, 1000), clampInt(h, 20, 600),
                                        clampInt(count, 0, 200000),
                                        static_cast<std::uint64_t>(std::min(std::max(seed, 0.0), 4503599627370496.0))));
}

// Same as POST /api/battle: `board` is the run-length encoded starting board.
const char* engine_battle(double w, double h, double maxgen, const char* variant, double home, double solo,
                          const char* board) {
    const gol::BattleConfig cfg = duel::battleConfig(clampInt(w, 50, 1000), clampInt(h, 20, 600),
                                                     clampInt(maxgen, 0, 10000000), variant, home == 0, solo == 1);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(cfg.width) * cfg.height);
    if (!gol::decodeBoard(board, cells)) return error("board does not match the size");
    sessions.push_front({std::to_string(nextId++), gol::Battle(cfg, std::move(cells))});
    while (sessions.size() > kMaxSessions) sessions.pop_back();
    return answer(duel::frameJson(sessions.front().id, sessions.front().battle, true));
}

// Same as GET /api/step: `ms` > 0 advances for that long, otherwise `n` generations.
const char* engine_step(const char* id, double n, double ms, int includeBoard) {
    Session* s = findSession(id);
    if (!s) return error("unknown battle");
    if (ms > 0) s->battle.advanceFor(std::min(ms, 2000.0));
    else s->battle.advance(clampInt(n, 0, 100000));
    return answer(duel::frameJson(s->id, s->battle, includeBoard != 0));
}

}  // extern "C"
