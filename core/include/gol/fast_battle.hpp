#pragma once

#include "gol/battle.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace gol {

// Bit-parallel implementation of exactly the same rules and end conditions as
// Battle, for bulk simulation (bot training, analysis) where no per-cell
// access is needed during the run. The core tests check that both engines
// agree cell by cell.
//
// The board is stored as bit planes (alive, red, blue) in columns of 64-bit
// words: word (k, y) holds cells x = 64k .. 64k+63 of row y, and the words of
// one column are contiguous. The board is updated in tiles of kTileRows rows of
// one column, one SIMD vector per bit plane.
//
// Only tiles whose neighbourhood changed are recomputed: the engine keeps two
// boards (t and t-1) and remembers which tiles of board t differ from board
// t-2. Where nothing around a tile changed in that sense, generation t+1 of the
// tile equals generation t-1, which is already in the buffer that receives
// t+1. This skips empty space, still lifes and period-2 oscillators. Cell
// counts and the repetition hash are updated from the changed words only.
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
    // Word (k, y) in a plane; column -1 / words_ and rows -1 / paddedRows_ exist and stay empty.
    std::size_t index(int k, int y) const {
        return static_cast<std::size_t>(k + 1) * stride_ + static_cast<std::size_t>(y + 1);
    }
    void markAllChanged();
    // Cells of `plane` with x in [x0, x1), over all rows.
    int countColumns(const std::vector<std::uint64_t>& plane, int x0, int x1) const;
    void countHome();
    void checkEnd();
    // Full hash of the current board; step() keeps hash_ up to date incrementally.
    std::uint64_t hash() const;

    BattleConfig config_;
    int words_ = 0;               // 64-bit words per row (= columns of words)
    std::uint64_t lastMask_ = 0;  // valid bits of the last word in a row
    int tiles_ = 0;               // tiles per column
    int paddedRows_ = 0;          // tiles_ * kTileRows
    std::size_t stride_ = 0;      // words per column including the empty rows
    // Bit planes: alive (any colour), red, blue. Grey = alive & ~red & ~blue.
    std::vector<std::uint64_t> alive_, red_bits_, blue_bits_;
    std::vector<std::uint64_t> nextAlive_, nextRed_, nextBlue_;
    // One bit per tile of a column (maskWords_ entries per column, with an empty
    // column on both sides): changed_ = the tile may differ from the same tile
    // two generations earlier; lowEdge_ / highEdge_ = bit 0 / bit 63 of one of
    // its words may differ (that bit is a neighbour of the column to the left
    // / right).
    int maskWords_ = 0;
    std::vector<std::uint64_t> changed_, lowEdge_, highEdge_, nextChanged_, nextLowEdge_, nextHighEdge_;
    // Totals of the board before the current one, and repetition hashes.
    int prevAlive_ = 0, prevRed_ = 0, prevBlue_ = 0;
    std::uint64_t hash_ = 0, prevHash_ = 0;

    int generation_ = 0;
    int red_ = 0, blue_ = 0, grey_ = 0;
    int redHome_ = 0, blueHome_ = 0;  // only with config_.ignoreHome
    Winner winner_ = Winner::None;
    EndReason reason_ = EndReason::None;
    int period_ = 0;
    double redScore_ = 0, blueScore_ = 0;
    // Counted cells per generation (see BattleConfig::ignoreHome).
    std::vector<int> redHistory_, blueHistory_;
    std::unordered_map<std::uint64_t, int> seen_;
    std::deque<std::uint64_t> seenOrder_;
};

}  // namespace gol
