#include "gol/fast_battle.hpp"

#include <algorithm>

namespace gol {

namespace {

int popcount(std::uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    int n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
#endif
}

// Bit-sliced counters: every bit position counts its own neighbours.
// Three bits count modulo 8 (8 neighbours read as 0, which is neither 2 nor 3).
struct Count3 {
    std::uint64_t b0 = 0, b1 = 0, b2 = 0;
    void add(std::uint64_t x) {
        const std::uint64_t c0 = b0 & x;
        b0 ^= x;
        const std::uint64_t c1 = b1 & c0;
        b1 ^= c0;
        b2 ^= c1;
    }
};
// Two bits count modulo 4. Colour counts are only used where at most three
// neighbours are alive, so they are exact wherever they matter.
struct Count2 {
    std::uint64_t b0 = 0, b1 = 0;
    void add(std::uint64_t x) {
        const std::uint64_t c0 = b0 & x;
        b0 ^= x;
        b1 ^= c0;
    }
};

// Bit x of the result holds the cell at x-1 (west neighbour) / x+1 (east neighbour).
inline std::uint64_t west(const std::uint64_t* row, int k) {
    return (row[k] << 1) | (k > 0 ? row[k - 1] >> 63 : 0);
}
inline std::uint64_t east(const std::uint64_t* row, int k, int words) {
    return (row[k] >> 1) | (k + 1 < words ? row[k + 1] << 63 : 0);
}

// Adds the eight neighbours of every bit in word k of the middle row.
template <typename Counter>
inline void addNeighbours(Counter& c, const std::uint64_t* up, const std::uint64_t* mid, const std::uint64_t* down,
                          int k, int words) {
    c.add(west(up, k));
    c.add(up[k]);
    c.add(east(up, k, words));
    c.add(west(mid, k));
    c.add(east(mid, k, words));
    c.add(west(down, k));
    c.add(down[k]);
    c.add(east(down, k, words));
}

}  // namespace

FastBattle::FastBattle(const BattleConfig& config, const std::vector<std::uint8_t>& initial) : config_(config) {
    config_.width = std::max(1, config_.width);
    config_.height = std::max(1, config_.height);
    config_.maxGenerations = std::max(0, config_.maxGenerations);
    const int w = config_.width, h = config_.height;
    words_ = (w + 63) / 64;
    lastMask_ = w % 64 == 0 ? ~0ull : (1ull << (w % 64)) - 1;
    const std::size_t n = static_cast<std::size_t>(words_) * h;
    alive_.assign(n, 0);
    red_bits_.assign(n, 0);
    blue_bits_.assign(n, 0);
    nextAlive_.assign(n, 0);
    nextRed_.assign(n, 0);
    nextBlue_.assign(n, 0);
    zeroRow_.assign(words_, 0);

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const std::uint8_t c = i < initial.size() ? initial[i] : std::uint8_t{Dead};
            if (c == Dead || c > Grey) continue;
            const std::uint64_t bit = 1ull << (x % 64);
            const std::size_t j = index(y, x / 64);
            alive_[j] |= bit;
            if (c == Red) red_bits_[j] |= bit;
            if (c == Blue) blue_bits_[j] |= bit;
        }
    count();
    redHistory_.push_back(red_);
    blueHistory_.push_back(blue_);
    checkEnd();
}

Cell FastBattle::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return Dead;
    const std::size_t j = index(y, x / 64);
    const std::uint64_t bit = 1ull << (x % 64);
    if (!(alive_[j] & bit)) return Dead;
    if (red_bits_[j] & bit) return Red;
    if (blue_bits_[j] & bit) return Blue;
    return Grey;
}

std::vector<std::uint8_t> FastBattle::cells() const {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(config_.width) * config_.height);
    for (int y = 0; y < config_.height; ++y)
        for (int x = 0; x < config_.width; ++x) out[static_cast<std::size_t>(y) * config_.width + x] = at(x, y);
    return out;
}

void FastBattle::step() {
    if (finished()) return;

    const int h = config_.height;
    const int words = words_;
    const bool contagion = config_.variant == Variant::Contagion;
    auto row = [&](const std::vector<std::uint64_t>& plane, int y) {
        return y < 0 || y >= h ? zeroRow_.data() : plane.data() + index(y, 0);
    };

    for (int y = 0; y < h; ++y) {
        const std::uint64_t *au = row(alive_, y - 1), *am = row(alive_, y), *ad = row(alive_, y + 1);
        const std::uint64_t *ru = row(red_bits_, y - 1), *rm = row(red_bits_, y), *rd = row(red_bits_, y + 1);
        const std::uint64_t *bu = row(blue_bits_, y - 1), *bm = row(blue_bits_, y), *bd = row(blue_bits_, y + 1);
        for (int k = 0; k < words; ++k) {
            Count3 alive;
            Count2 red, blue;
            addNeighbours(alive, au, am, ad, k, words);
            addNeighbours(red, ru, rm, rd, k, words);
            addNeighbours(blue, bu, bm, bd, k, words);

            const std::uint64_t self = am[k];
            const std::uint64_t three = alive.b0 & alive.b1 & ~alive.b2;
            const std::uint64_t two = ~alive.b0 & alive.b1 & ~alive.b2;
            const std::uint64_t mask = k + 1 == words ? lastMask_ : ~0ull;
            const std::uint64_t birth = ~self & three & mask;
            const std::uint64_t survive = self & (two | three);

            // Majority vote: compare the two-bit red and blue counts.
            const std::uint64_t same1 = ~(red.b1 ^ blue.b1);
            const std::uint64_t redWins = (red.b1 & ~blue.b1) | (same1 & red.b0 & ~blue.b0);
            const std::uint64_t blueWins = (blue.b1 & ~red.b1) | (same1 & blue.b0 & ~red.b0);

            const std::size_t j = index(y, k);
            nextAlive_[j] = birth | survive;
            nextRed_[j] = (birth & redWins) | (survive & (contagion ? redWins : rm[k]));
            nextBlue_[j] = (birth & blueWins) | (survive & (contagion ? blueWins : bm[k]));
        }
    }

    alive_.swap(nextAlive_);
    red_bits_.swap(nextRed_);
    blue_bits_.swap(nextBlue_);
    ++generation_;
    count();
    redHistory_.push_back(red_);
    blueHistory_.push_back(blue_);
    checkEnd();
}

int FastBattle::advance(int generations) {
    int done = 0;
    while (done < generations && !finished()) {
        step();
        ++done;
    }
    return done;
}

void FastBattle::runToEnd() {
    while (!finished()) step();
}

void FastBattle::count() {
    int alive = 0;
    red_ = blue_ = 0;
    for (std::size_t j = 0; j < alive_.size(); ++j) {
        alive += popcount(alive_[j]);
        red_ += popcount(red_bits_[j]);
        blue_ += popcount(blue_bits_[j]);
    }
    grey_ = alive - red_ - blue_;
}

// Same end conditions and scores as Battle::checkEnd.
void FastBattle::checkEnd() {
    auto decideByScore = [this](EndReason reason) {
        reason_ = reason;
        if (redScore_ > blueScore_) winner_ = Winner::Red;
        else if (blueScore_ > redScore_) winner_ = Winner::Blue;
        else winner_ = Winner::Draw;
    };

    redScore_ = red_;
    blueScore_ = blue_;

    if (red_ == 0 || blue_ == 0) {
        decideByScore(EndReason::Elimination);
        return;
    }

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
            redSum += redHistory_[g];
            blueSum += blueHistory_[g];
        }
        redScore_ = redSum / period_;
        blueScore_ = blueSum / period_;
        decideByScore(EndReason::Repetition);
        return;
    }

    if (!unlimited() && generation_ >= config_.maxGenerations) decideByScore(EndReason::GenerationLimit);
}

std::uint64_t FastBattle::hash() const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t word) {
        h = (h ^ word) * 1099511628211ull;
        h ^= h >> 29;
    };
    for (std::size_t j = 0; j < alive_.size(); ++j) {
        mix(alive_[j]);
        mix(red_bits_[j]);
        mix(blue_bits_[j]);
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

}  // namespace gol
