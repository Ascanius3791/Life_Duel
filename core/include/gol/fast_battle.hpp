#pragma once

#include "gol/battle.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace gol {

// Bit-parallel implementation of exactly the same rules and end conditions as
// Battle: 64 cells are updated at once per machine word. Meant for bulk
// simulation (bot training, analysis) where no per-cell access is needed
// during the run. The core tests check that both engines agree.
class FastBattle {
public:
    // `initial` holds width*height cells, row by row.
    FastBattle(const BattleConfig& config, const std::vector<std::uint8_t>& initial);

    const BattleConfig& config() const { return config_; }
    int width() const { return config_.width; }
    int height() const { return config_.height; }
    Cell at(int x, int y) const;
    // Materialises the board as width*height cells (slow compared to step()).
    std::vector<std::uint8_t> cells() const;

    void step();
    int advance(int generations);
    // Runs until the battle is decided. Without a generation limit this may never return.
    void runToEnd();

    int generation() const { return generation_; }
    int redCount() const { return red_; }
    int blueCount() const { return blue_; }
    int greyCount() const { return grey_; }

    bool finished() const { return winner_ != Winner::None; }
    bool unlimited() const { return config_.maxGenerations == 0; }
    Winner winner() const { return winner_; }
    EndReason endReason() const { return reason_; }
    int period() const { return period_; }
    double redScore() const { return redScore_; }
    double blueScore() const { return blueScore_; }

private:
    std::size_t index(int y, int word) const { return static_cast<std::size_t>(y) * words_ + word; }
    void count();
    void checkEnd();
    std::uint64_t hash() const;

    BattleConfig config_;
    int words_ = 0;               // 64-bit words per row
    std::uint64_t lastMask_ = 0;  // valid bits of the last word in a row
    // Bit planes: alive (any colour), red, blue. Grey = alive & ~red & ~blue.
    std::vector<std::uint64_t> alive_, red_bits_, blue_bits_;
    std::vector<std::uint64_t> nextAlive_, nextRed_, nextBlue_;
    std::vector<std::uint64_t> zeroRow_;

    int generation_ = 0;
    int red_ = 0, blue_ = 0, grey_ = 0;
    Winner winner_ = Winner::None;
    EndReason reason_ = EndReason::None;
    int period_ = 0;
    double redScore_ = 0, blueScore_ = 0;
    std::vector<int> redHistory_, blueHistory_;
    std::unordered_map<std::uint64_t, int> seen_;
    std::deque<std::uint64_t> seenOrder_;
};

}  // namespace gol
