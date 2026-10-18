#include "gol/battle.hpp"

#include "gol/patterns.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace gol {

Zones zonesFor(int width) {
    const int zone = width / 5;
    return {zone, zone, width - zone};
}

// ---------------------------------------------------------------------------
// Battle simulation
// ---------------------------------------------------------------------------

Battle::Battle(const BattleConfig& config, std::vector<std::uint8_t> initial)
    : config_(config), cells_(std::move(initial)) {
    config_.width = std::max(1, config_.width);
    config_.height = std::max(1, config_.height);
    config_.maxGenerations = std::max(0, config_.maxGenerations);
    cells_.resize(static_cast<std::size_t>(config_.width) * config_.height, Dead);
    for (auto& c : cells_) c = c > Grey ? std::uint8_t{Dead} : c;
    next_.resize(cells_.size());
    count();
    record();
    checkEnd();
}

Cell Battle::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return Dead;
    return static_cast<Cell>(cells_[static_cast<std::size_t>(y) * config_.width + x]);
}

void Battle::step() {
    if (finished()) return;

    const int w = config_.width;
    const int h = config_.height;
    const bool contagion = config_.variant == Variant::Contagion;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Count neighbours per colour. Everything outside the board is dead.
            int red = 0, blue = 0, alive = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                const std::uint8_t* row = &cells_[static_cast<std::size_t>(ny) * w];
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx;
                    if (nx < 0 || nx >= w) continue;
                    const std::uint8_t c = row[nx];
                    alive += c != Dead;
                    red += c == Red;
                    blue += c == Blue;
                }
            }

            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const std::uint8_t self = cells_[i];
            std::uint8_t result = Dead;
            if (self == Dead) {
                if (alive == 3) result = majorityColour(red, blue);  // birth
            } else if (alive == 2 || alive == 3) {
                result = contagion ? std::uint8_t{majorityColour(red, blue)} : self;  // survival
            }
            next_[i] = result;
        }
    }

    cells_.swap(next_);
    ++generation_;
    count();
    record();
    checkEnd();
}

int Battle::advance(int generations) {
    int done = 0;
    while (done < generations && !finished()) {
        step();
        ++done;
    }
    return done;
}

int Battle::advanceFor(double milliseconds) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::duration<double, std::milli>(milliseconds);
    int done = 0;
    while (!finished()) {
        step();
        ++done;
        if (clock::now() >= deadline) break;
    }
    return done;
}

void Battle::runToEnd() {
    while (!finished()) step();
}

void Battle::count() {
    red_ = blue_ = grey_ = 0;
    for (auto c : cells_) {
        red_ += c == Red;
        blue_ += c == Blue;
        grey_ += c == Grey;
    }
    redHome_ = blueHome_ = 0;
    if (!config_.ignoreHome) return;
    const Zones z = zonesFor(config_.width);
    for (int y = 0; y < config_.height; ++y) {
        const std::uint8_t* row = &cells_[static_cast<std::size_t>(y) * config_.width];
        for (int x = 0; x < z.leftEnd; ++x) redHome_ += row[x] == Red;
        for (int x = z.rightStart; x < config_.width; ++x) blueHome_ += row[x] == Blue;
    }
}

void Battle::record() {
    redHistory_.push_back(red_);
    blueHistory_.push_back(blue_);
    greyHistory_.push_back(grey_);
    redCounted_.push_back(red_ - redHome_);
    blueCounted_.push_back(blue_ - blueHome_);
}

void Battle::checkEnd() {
    auto decideByScore = [this](EndReason reason) {
        reason_ = reason;
        if (redScore_ > blueScore_) winner_ = Winner::Red;
        else if (blueScore_ > redScore_) winner_ = Winner::Blue;
        else winner_ = Winner::Draw;
    };

    redScore_ = red_ - redHome_;
    blueScore_ = blue_ - blueHome_;

    // A colour can never come back once it is gone: births need a neighbour of
    // that colour to win the vote. The survivor wins, wherever its cells are.
    // Alone on the board, Red only ends early when it dies out (a draw at 0).
    if (red_ == 0 || (blue_ == 0 && !config_.solo)) {
        reason_ = EndReason::Elimination;
        winner_ = red_ > 0 ? Winner::Red : blue_ > 0 ? Winner::Blue : Winner::Draw;
        return;
    }

    // Exact repetition of a whole board means the game would loop forever.
    // The score is then the average over one full cycle so oscillating
    // armies are judged fairly.
    const std::uint64_t h = hash();
    const auto [it, inserted] = seen_.emplace(h, generation_);
    if (inserted) {
        seenOrder_.push_back(h);
        if (seenOrder_.size() > kRepetitionWindow) {
            seen_.erase(seenOrder_.front());
            seenOrder_.pop_front();
        }
    } else {
        const int first = it->second;
        period_ = generation_ - first;
        double redSum = 0, blueSum = 0;
        for (int g = first; g < generation_; ++g) {
            redSum += redCounted_[g];
            blueSum += blueCounted_[g];
        }
        redScore_ = redSum / period_;
        blueScore_ = blueSum / period_;
        decideByScore(EndReason::Repetition);
        return;
    }

    if (!unlimited() && generation_ >= config_.maxGenerations) {
        decideByScore(EndReason::GenerationLimit);
    }
}

std::uint64_t Battle::hash() const {
    // FNV-1a over 8-byte words followed by a final avalanche.
    std::uint64_t h = 1469598103934665603ull;
    const std::size_t n = cells_.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t word = 0;
        for (int k = 0; k < 8; ++k) word |= static_cast<std::uint64_t>(cells_[i + k]) << (8 * k);
        h = (h ^ word) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ cells_[i]) * 1099511628211ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

const char* toString(Winner winner) {
    switch (winner) {
        case Winner::Red: return "red";
        case Winner::Blue: return "blue";
        case Winner::Draw: return "draw";
        default: return "none";
    }
}

const char* toString(EndReason reason) {
    switch (reason) {
        case EndReason::Elimination: return "elimination";
        case EndReason::GenerationLimit: return "generation_limit";
        case EndReason::Repetition: return "repetition";
        default: return "none";
    }
}

// ---------------------------------------------------------------------------
// Battlefields
// ---------------------------------------------------------------------------

const std::vector<BattlefieldPreset>& battlefieldPresets() {
    static const std::vector<BattlefieldPreset> presets = {
        {"random_mirror", "Random (mirrored)", "Random grey cells, mirror symmetric so both sides are equal.", true},
        {"random_rotational", "Random (rotational)", "Random grey cells with 180° rotational symmetry.", true},
        {"random", "Random (unfair!)", "Completely random grey cells. Not symmetric.", true},
        {"reactor", "Reactor core", "A dense symmetric soup in the centre that explodes before you arrive.", true},
        {"bunkers", "Bunker line", "A column of blocks down the middle.", false},
        {"hive_lattice", "Hive lattice", "A regular lattice of beehives filling the battlefield.", false},
        {"blinker_field", "Blinker field", "Rows of blinkers ticking away.", false},
        {"pulsar_towers", "Pulsar towers", "Two mirrored columns of pulsars.", false},
        {"penta_gates", "Pentadecathlon gates", "Vertical pentadecathlons that spark and deflect.", false},
        {"fortress", "Fortress", "Block walls on both flanks protecting a diamond of beehives.", false},
        {"empty", "No man's land", "Nothing in between. Pure army against army.", false},
    };
    return presets;
}

namespace {

class Board {
public:
    Board(int w, int h) : w_(w), h_(h), zones_(zonesFor(w)), cells_(static_cast<std::size_t>(w) * h, Dead) {}

    int width() const { return w_; }
    int height() const { return h_; }
    // Grey cells may only appear in [minX, maxX) so a gap remains to the players' zones.
    int minX() const { return zones_.leftEnd + 3; }
    int maxX() const { return zones_.rightStart - 3; }
    int centre() const { return w_ / 2; }

    void set(int x, int y) {
        if (x >= minX() && x < maxX() && y >= 0 && y < h_) cells_[static_cast<std::size_t>(y) * w_ + x] = Grey;
    }
    bool get(int x, int y) const { return cells_[static_cast<std::size_t>(y) * w_ + x] != Dead; }

    void stamp(const Pattern& p, int x, int y) {
        for (const auto& c : p.cells) set(x + c.x, y + c.y);
    }

    // Copies the left half onto the right half, mirrored at the vertical centre line.
    void mirror() {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_ / 2; ++x) cells_[idx(w_ - 1 - x, y)] = cells_[idx(x, y)];
    }
    // Copies the left half onto the right half, rotated by 180° around the centre.
    void rotate() {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_ / 2; ++x) cells_[idx(w_ - 1 - x, h_ - 1 - y)] = cells_[idx(x, y)];
    }

    std::vector<std::uint8_t> take() { return std::move(cells_); }

private:
    std::size_t idx(int x, int y) const { return static_cast<std::size_t>(y) * w_ + x; }
    int w_, h_;
    Zones zones_;
    std::vector<std::uint8_t> cells_;
};

// The <random> distributions are implementation-defined: libc++ (Emscripten,
// macOS) draws other numbers than libstdc++ from the same engine, so the same
// seed would give another battlefield in the browser build. These two
// replicate libstdc++'s algorithms exactly, which keeps every battlefield the
// native builds (and the bots trained on them) have ever generated.

// Uniform in [lo, hi], like std::uniform_int_distribution<int> in libstdc++
// (Lemire's nearly divisionless method on the full 64-bit engine output).
__extension__ typedef unsigned __int128 Uint128;  // GCC and Clang, also for wasm32

int uniformInt(std::mt19937_64& rng, int lo, int hi) {
    const std::uint64_t range = static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi - lo)) + 1;
    Uint128 product = static_cast<Uint128>(rng()) * range;
    std::uint64_t low = static_cast<std::uint64_t>(product);
    if (low < range) {
        const std::uint64_t threshold = (0 - range) % range;
        while (low < threshold) {
            product = static_cast<Uint128>(rng()) * range;
            low = static_cast<std::uint64_t>(product);
        }
    }
    return lo + static_cast<int>(static_cast<std::uint64_t>(product >> 64));
}

// True with probability p, like std::bernoulli_distribution in libstdc++
// (generate_canonical<double, 53> on one 64-bit draw).
bool chance(std::mt19937_64& rng, double p) {
    double u = static_cast<double>(rng()) / 18446744073709551616.0;  // 2^64
    if (u >= 1.0) u = std::nextafter(1.0, 0.0);
    return u < p;
}

// Scatter up to `count` distinct cells into the rectangle [x0,x1) x [y0,y1).
void scatter(Board& b, std::mt19937_64& rng, int count, int x0, int x1, int y0, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    const long long area = static_cast<long long>(x1 - x0) * (y1 - y0);
    count = static_cast<int>(std::min<long long>(count, area));
    int placed = 0;
    for (long long attempts = 0; placed < count && attempts < area * 20; ++attempts) {
        const int x = uniformInt(rng, x0, x1 - 1);
        const int y = uniformInt(rng, y0, y1 - 1);
        if (!b.get(x, y)) {
            b.set(x, y);
            ++placed;
        }
    }
}

const Pattern& pat(std::string_view id) { return *findPattern(id); }

}  // namespace

std::vector<std::uint8_t> makeBattlefield(std::string_view presetId, int width, int height, int cellCount,
                                          std::uint64_t seed) {
    Board b(width, height);
    std::mt19937_64 rng(seed);
    const int half = b.centre();
    cellCount = std::max(0, cellCount);

    if (presetId == "random") {
        scatter(b, rng, cellCount, b.minX(), b.maxX(), 0, height);
        return b.take();
    }
    if (presetId == "random_mirror" || presetId == "random_rotational") {
        scatter(b, rng, cellCount / 2, b.minX(), half, 0, height);
        presetId == "random_mirror" ? b.mirror() : b.rotate();
        return b.take();
    }
    if (presetId == "reactor") {
        // Disc filled with ~40% density, sized so that it holds about cellCount cells.
        const double radius = std::min({std::sqrt(cellCount / (0.4 * 3.14159265)), height / 2.0 - 2.0,
                                        (b.maxX() - b.minX()) / 2.0 - 2.0});
        const double cx = (width - 1) / 2.0, cy = (height - 1) / 2.0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < half; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius && chance(rng, 0.4)) b.set(x, y);
        b.mirror();
        return b.take();
    }
    if (presetId == "bunkers") {
        for (int y = 3; y + 2 <= height - 2; y += 8) b.stamp(pat("block"), half - 1, y);
        return b.take();
    }
    if (presetId == "hive_lattice") {
        const Pattern& hive = pat("beehive");  // 4x3, mirror symmetric
        int row = 0;
        for (int y = 4; y + 3 <= height - 3; y += 9, ++row) {
            for (int x = half - 2 - (row % 2) * 7; x >= b.minX(); x -= 14) b.stamp(hive, x, y);
        }
        b.mirror();
        return b.take();
    }
    if (presetId == "blinker_field") {
        const Pattern vertical = transformed(pat("blinker"), 1, false);
        int row = 0;
        for (int y = 3; y + 3 <= height - 3; y += 7, ++row) {
            for (int x = half - 4 - (row % 2) * 4; x >= b.minX(); x -= 8) b.stamp(vertical, x, y);
        }
        b.mirror();
        return b.take();
    }
    if (presetId == "pulsar_towers") {
        const Pattern& pulsar = pat("pulsar");  // 13x13
        for (int y = 3; y + 13 <= height - 3; y += 18) b.stamp(pulsar, half - 4 - 13, y);
        b.mirror();
        return b.take();
    }
    if (presetId == "penta_gates") {
        const Pattern upright = transformed(pat("pentadecathlon"), 1, false);  // 3x10
        int col = 0;
        for (int x = half - 9; x >= b.minX() + 4 && col < 3; x -= 17, ++col) {
            for (int y = 5 + (col % 2) * 12; y + 10 <= height - 5; y += 24) b.stamp(upright, x, y);
        }
        b.mirror();
        return b.take();
    }
    if (presetId == "fortress") {
        const int wallX = b.minX() + (half - b.minX()) / 4;
        for (int y = 2; y + 2 <= height - 2; y += 5) b.stamp(pat("block"), wallX, y);
        const Pattern& hive = pat("beehive");
        const int cy = height / 2 - 1;
        const int r = std::min(height / 3, (half - wallX) - 8);
        for (int d = 0; d <= r; d += 6) {
            const int x = half - 2 - (r - d);
            b.stamp(hive, x, cy - d);
            b.stamp(hive, x, cy + d);
        }
        b.mirror();
        return b.take();
    }
    return b.take();  // "empty" and unknown ids
}

// ---------------------------------------------------------------------------
// Board encoding
// ---------------------------------------------------------------------------

std::string encodeBoard(const std::vector<std::uint8_t>& cells) {
    static const char symbols[4] = {'.', 'r', 'b', 'g'};
    std::string out;
    out.reserve(cells.size() / 4);
    std::size_t i = 0;
    while (i < cells.size()) {
        std::size_t j = i + 1;
        while (j < cells.size() && cells[j] == cells[i]) ++j;
        const std::size_t run = j - i;
        if (run > 1) out += std::to_string(run);
        out += symbols[cells[i] & 3];
        i = j;
    }
    return out;
}

bool decodeBoard(std::string_view text, std::vector<std::uint8_t>& out) {
    std::size_t pos = 0;
    std::size_t run = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            run = run * 10 + static_cast<std::size_t>(c - '0');
            if (run > out.size()) return false;
            continue;
        }
        std::uint8_t value;
        switch (c) {
            case '.': value = Dead; break;
            case 'r': value = Red; break;
            case 'b': value = Blue; break;
            case 'g': value = Grey; break;
            case '\r': case '\n': case ' ': continue;
            default: return false;
        }
        const std::size_t n = run == 0 ? 1 : run;
        run = 0;
        if (pos + n > out.size()) return false;
        std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(pos), n, value);
        pos += n;
    }
    return pos == out.size();
}

}  // namespace gol
