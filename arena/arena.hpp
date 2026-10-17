#pragma once

// Building blocks for training Life Duel bots: armies made of arsenal pieces,
// legal random armies and mutations, and fast head-to-head matches on a fixed
// grey battlefield.

#include "gol/battle.hpp"
#include "gol/patterns.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace arena {

using Rng = std::mt19937_64;

// One bought arsenal item. Coordinates are those of the red player (left zone):
// the top-left corner of the piece after rotating by `rot` quarter turns
// clockwise and then optionally mirroring, exactly like the browser does.
struct Piece {
    int item = 0;  // index into gol::battleArsenal()
    int x = 0, y = 0;
    int rot = 0;
    bool mirror = false;
};

struct Army {
    std::vector<Piece> pieces;
    std::string origin;  // how the army was found, e.g. "random" or "iter 7"
};

struct Settings {
    int width = 260;
    int height = 128;
    std::string preset = "random_mirror";
    int count = 900;
    std::uint64_t fieldSeed = 1;
    int budget = 120;
    int maxGenerations = 2000;
    gol::Variant variant = gol::Variant::BirthOnly;
};

// Outcome of one battle, scored from Red's point of view in [-1, 1]:
// 0.7 for the result (win +, loss -, draw 0) plus 0.3 times the final margin
// (red - blue) / (red + blue). The margin rewards near-wins and punishes
// heavy losses, so the search can tell "almost good" from "hopeless".
struct MatchResult {
    gol::Winner winner = gol::Winner::None;
    gol::EndReason reason = gol::EndReason::None;
    int generation = 0;
    double redScore = 0, blueScore = 0;
    double score = 0;
};

class Arena {
public:
    explicit Arena(Settings settings);

    const Settings& settings() const { return settings_; }
    const gol::Zones& zones() const { return zones_; }
    const std::vector<std::uint8_t>& battlefield() const { return grey_; }
    // How Blue's copy of an army is obtained so that both sides are equal.
    enum class Symmetry { Mirror, Rotation, None };
    Symmetry symmetry() const { return symmetry_; }

    // Shape of an arsenal item in a given orientation.
    const gol::Pattern& shape(int item, int rot, bool mirror) const;
    const gol::Pattern& shape(const Piece& p) const { return shape(p.item, p.rot, p.mirror); }
    int itemCount() const { return static_cast<int>(arsenal_.size()); }
    int cost(int item) const { return arsenal_[item].cost; }
    const std::string& itemId(int item) const { return arsenal_[item].patternId; }
    int findItem(const std::string& patternId) const;  // -1 if unknown

    int cost(const Army& army) const;
    // Inside the red zone and within budget.
    bool legal(const Army& army) const;
    // Moves pieces back into the zone and drops random pieces until the budget fits.
    void repair(Army& army, Rng& rng) const;

    Army randomArmy(Rng& rng) const;
    Army mutate(const Army& army, Rng& rng) const;
    // Pieces above a random cut line from `a`, below it from `b`.
    Army crossover(const Army& a, const Army& b, Rng& rng) const;

    // The piece as Blue places it (mirrored or rotated into the right zone).
    Piece toBlue(const Piece& p) const;
    // Full starting board: grey battlefield, `red` in the left zone, `blue`'s copy in the right zone.
    std::vector<std::uint8_t> board(const Army& red, const Army& blue) const;

    MatchResult play(const Army& red, const Army& blue) const;
    // Score of `a` against `b`. On symmetric battlefields one battle decides it
    // (the swapped battle is its exact mirror image); otherwise both sides are
    // played and averaged.
    double payoff(const Army& a, const Army& b) const;

private:
    void clampInto(Piece& p) const;
    Piece randomPiece(int item, Rng& rng) const;

    Settings settings_;
    gol::Zones zones_;
    std::vector<std::uint8_t> grey_;
    Symmetry symmetry_;
    std::vector<gol::ArsenalItem> arsenal_;
    std::vector<gol::Pattern> shapes_;  // [item][rot][mirror]
};

// Text form of an army: "gosper_gun@3,10,r1,m0;block@40,60,r0,m0".
std::string formatPieces(const Arena& arena, const Army& army);
bool parsePieces(const Arena& arena, const std::string& text, Army& army);

// Approximate maximin (Nash) mixture of a symmetric zero-sum game with payoff
// matrix m[i][j] (= -m[j][i]), found by regret matching+ self-play.
std::vector<double> solveNash(const std::vector<std::vector<double>>& m, int iterations = 20000);

// Runs fn(i) for i in [0, n) on `threads` worker threads.
void parallelFor(int n, int threads, const std::function<void(int)>& fn);

const char* variantName(gol::Variant v);
bool parseVariant(const std::string& s, gol::Variant& v);

}  // namespace arena
