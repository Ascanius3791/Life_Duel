#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gol {

// Cell states of the coloured 1v1 variant.
enum Cell : std::uint8_t { Dead = 0, Red = 1, Blue = 2, Grey = 3 };

enum class Variant {
    // Only newborn cells pick a colour; survivors keep the colour they have.
    BirthOnly,
    // Survivors are re-coloured every generation by the same majority rule.
    Contagion,
};

// The colour rule shared by births (and, in the Contagion variant, survivors):
// the colour with more cells among the eight neighbours wins, ties become grey.
// Grey neighbours keep the cell alive but never vote.
inline Cell majorityColour(int redNeighbours, int blueNeighbours) {
    if (redNeighbours > blueNeighbours) return Red;
    if (blueNeighbours > redNeighbours) return Blue;
    return Grey;
}

enum class Winner { None, Red, Blue, Draw };
enum class EndReason { None, Elimination, GenerationLimit, Repetition };

struct BattleConfig {
    int width = 260;
    int height = 128;
    Variant variant = Variant::BirthOnly;
    // 0 = no limit: the battle only ends by elimination or repetition.
    int maxGenerations = 2000;
    // Cells inside their own starting area (red in the left zone, blue in the
    // right one) do not count for the score that decides a generation limit or
    // a repetition. Elimination still looks at every cell.
    bool ignoreHome = false;
};

// Number of recent boards a battle remembers to detect repetition. Any cycle
// up to that period is still detected at the moment it first repeats, while
// memory stays bounded in battles without a generation limit.
constexpr std::size_t kRepetitionWindow = 1 << 16;

// The two starting areas and the battlefield between them.
struct Zones {
    int zoneWidth;  // width of each player's area
    int leftEnd;    // red area: x in [0, leftEnd)
    int rightStart; // blue area: x in [rightStart, width)
};
Zones zonesFor(int width);

class Battle {
public:
    // `initial` holds width*height cells, row by row.
    Battle(const BattleConfig& config, std::vector<std::uint8_t> initial);

    const BattleConfig& config() const { return config_; }
    int width() const { return config_.width; }
    int height() const { return config_.height; }
    Cell at(int x, int y) const;
    const std::vector<std::uint8_t>& cells() const { return cells_; }

    // Advances one generation; does nothing once the battle is over.
    void step();
    // Advances up to `generations` steps, stopping early when the battle ends.
    int advance(int generations);
    // Advances until roughly `milliseconds` have elapsed or the battle ends.
    int advanceFor(double milliseconds);
    // Runs until the battle is decided. Without a generation limit this may never return.
    void runToEnd();

    int generation() const { return generation_; }
    int redCount() const { return red_; }
    int blueCount() const { return blue_; }
    int greyCount() const { return grey_; }
    // Cells in their own starting area (only counted with config().ignoreHome, else 0).
    int redHome() const { return redHome_; }
    int blueHome() const { return blueHome_; }

    bool finished() const { return winner_ != Winner::None; }
    bool unlimited() const { return config_.maxGenerations == 0; }
    Winner winner() const { return winner_; }
    EndReason endReason() const { return reason_; }
    // Length of the detected cycle (1 = still life) when reason is Repetition.
    int period() const { return period_; }
    // Score used to decide the winner: the counted cells (all of them, or
    // those outside the own area with ignoreHome), or their average over the cycle.
    double redScore() const { return redScore_; }
    double blueScore() const { return blueScore_; }

    // Cell counts for every generation computed so far (index = generation).
    const std::vector<int>& redHistory() const { return redHistory_; }
    const std::vector<int>& blueHistory() const { return blueHistory_; }
    const std::vector<int>& greyHistory() const { return greyHistory_; }
    // Counted cells per generation (equal to the cell counts without ignoreHome).
    const std::vector<int>& redScoreHistory() const { return redCounted_; }
    const std::vector<int>& blueScoreHistory() const { return blueCounted_; }

private:
    void count();
    void record();
    void checkEnd();
    std::uint64_t hash() const;

    BattleConfig config_;
    std::vector<std::uint8_t> cells_;
    std::vector<std::uint8_t> next_;
    int generation_ = 0;
    int red_ = 0, blue_ = 0, grey_ = 0;
    int redHome_ = 0, blueHome_ = 0;
    Winner winner_ = Winner::None;
    EndReason reason_ = EndReason::None;
    int period_ = 0;
    double redScore_ = 0, blueScore_ = 0;
    std::vector<int> redHistory_, blueHistory_, greyHistory_;
    std::vector<int> redCounted_, blueCounted_;
    // Hashes of the last kRepetitionWindow boards (hash -> generation).
    std::unordered_map<std::uint64_t, int> seen_;
    std::deque<std::uint64_t> seenOrder_;
};

// Neutral grey battlefield generation. Cells are only placed in the middle
// area between the two players' zones.
struct BattlefieldPreset {
    std::string id;
    std::string name;
    std::string description;
    bool usesCount;  // whether the "number of cells" parameter matters
};
const std::vector<BattlefieldPreset>& battlefieldPresets();

std::vector<std::uint8_t> makeBattlefield(std::string_view presetId, int width, int height, int cellCount,
                                          std::uint64_t seed);

// Compact text encoding of a board used by the browser protocol:
// runs of '.', 'r', 'b', 'g' with an optional repeat count, e.g. "12.3r.g".
std::string encodeBoard(const std::vector<std::uint8_t>& cells);
bool decodeBoard(std::string_view text, std::vector<std::uint8_t>& out);

const char* toString(Winner winner);
const char* toString(EndReason reason);

}  // namespace gol
