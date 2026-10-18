#include "gol/fast_battle.hpp"

#include <algorithm>
#include <cstring>

#if defined(__AVX__)
#include <immintrin.h>
#endif

// The SIMD vector type below is only used inside this file, so the ABI note
// GCC gives when it is passed by value without AVX enabled does not matter.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wpsabi"
#endif

namespace gol {

namespace {

// Rows per tile = 64-bit lanes per SIMD vector (4 = one AVX2 register).
constexpr int kTileRows = 4;

#if defined(__GNUC__) || defined(__clang__)
// GCC/Clang vector extension: element-wise operators, compiled to SSE/AVX/NEON.
typedef std::uint64_t Vec __attribute__((vector_size(kTileRows * sizeof(std::uint64_t))));
#if defined(__POPCNT__)
inline int popcount(std::uint64_t x) { return __builtin_popcountll(x); }
#else
// Without the POPCNT instruction GCC would call a slow library function.
inline int popcount(std::uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<int>((x * 0x0101010101010101ull) >> 56);
}
#endif
inline int lowestBit(std::uint64_t x) { return __builtin_ctzll(x); }
#else
struct Vec {
    std::uint64_t v[kTileRows];
    std::uint64_t& operator[](int i) { return v[i]; }
    std::uint64_t operator[](int i) const { return v[i]; }
#define GOL_VEC_OP(op)                                                  \
    friend Vec operator op(Vec a, const Vec& b) {                       \
        for (int i = 0; i < kTileRows; ++i) a.v[i] = a.v[i] op b.v[i];  \
        return a;                                                       \
    }
    GOL_VEC_OP(&)
    GOL_VEC_OP(|)
    GOL_VEC_OP(^)
    GOL_VEC_OP(+)
    GOL_VEC_OP(-)
#undef GOL_VEC_OP
    friend Vec operator~(Vec a) {
        for (auto& x : a.v) x = ~x;
        return a;
    }
    friend Vec operator<<(Vec a, int n) {
        for (auto& x : a.v) x <<= n;
        return a;
    }
    friend Vec operator>>(Vec a, int n) {
        for (auto& x : a.v) x >>= n;
        return a;
    }
};
inline int popcount(std::uint64_t x) {
    int n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
}
inline int lowestBit(std::uint64_t x) {
    int n = 0;
    for (; !(x & 1); x >>= 1) ++n;
    return n;
}
#endif

inline Vec load(const std::uint64_t* p) {
    Vec v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store(std::uint64_t* p, const Vec& v) { std::memcpy(p, &v, sizeof v); }
inline Vec broadcast(std::uint64_t x) {
    Vec v;
    for (int i = 0; i < kTileRows; ++i) v[i] = x;
    return v;
}
inline bool any(const Vec& v) {
#if defined(__AVX__) && (defined(__GNUC__) || defined(__clang__))
    if (kTileRows == 4) return !_mm256_testz_si256(reinterpret_cast<__m256i>(v), reinterpret_cast<__m256i>(v));
#endif
    std::uint64_t x = 0;
    for (int i = 0; i < kTileRows; ++i) x |= v[i];
    return x != 0;
}

// Number of set bits in every lane.
inline Vec popcounts(const Vec& v) {
#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512VL__)
    static_assert(kTileRows == 4, "one AVX2 register per tile");
    return reinterpret_cast<Vec>(_mm256_popcnt_epi64(reinterpret_cast<__m256i>(v)));
#elif defined(__AVX2__)
    static_assert(kTileRows == 4, "one AVX2 register per tile");
    // Nibble lookup table, then byte sums per 64-bit lane.
    const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1, 2, 1, 2, 2, 3, 1, 2,
                                         2, 3, 2, 3, 3, 4);
    const __m256i nibble = _mm256_set1_epi8(0x0f);
    const __m256i x = reinterpret_cast<__m256i>(v);
    const __m256i lo = _mm256_shuffle_epi8(lut, _mm256_and_si256(x, nibble));
    const __m256i hi = _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(x, 4), nibble));
    return reinterpret_cast<Vec>(_mm256_sad_epu8(_mm256_add_epi8(lo, hi), _mm256_setzero_si256()));
#else
    Vec r = v;
    for (int i = 0; i < kTileRows; ++i) r[i] = static_cast<std::uint64_t>(popcount(v[i]));
    return r;
#endif
}
inline std::int64_t sum(const Vec& v) {
    std::uint64_t x = 0;
    for (int i = 0; i < kTileRows; ++i) x += v[i];
    return static_cast<std::int64_t>(x);
}

// Bit-sliced neighbour counting: every bit position counts its own eight
// neighbours with a tree of full adders.
inline void fullAdd(const Vec& a, const Vec& b, const Vec& c, Vec& sum, Vec& carry) {
    const Vec t = a ^ b;
    sum = t ^ c;
    carry = (a & b) | (t & c);
}

// Partial sums of a tile's neighbours in one bit plane. `left`, `mid`, `right`
// point to the tile's first row in the columns k-1, k, k+1. The neighbour
// count is s0 + s1 + s2 + 2 * (c0 + c1 + c2), where (s0, c0) / (s2, c2) count
// the three cells above / below and (s1, c1) the two beside each cell.
struct Neighbours {
    Vec s0, c0, s1, c1, s2, c2;
};
inline Neighbours neighbours(const std::uint64_t* left, const std::uint64_t* mid, const std::uint64_t* right) {
    Neighbours n;
    auto west = [&](const Vec& m, int d) { return (m << 1) | (load(left + d) >> 63); };  // bit x: cell x-1
    auto east = [&](const Vec& m, int d) { return (m >> 1) | (load(right + d) << 63); };  // bit x: cell x+1
    const Vec up = load(mid - 1), here = load(mid), down = load(mid + 1);
    fullAdd(west(up, -1), up, east(up, -1), n.s0, n.c0);
    const Vec w = west(here, 0), e = east(here, 0);
    n.s1 = w ^ e;
    n.c1 = w & e;
    fullAdd(west(down, 1), down, east(down, 1), n.s2, n.c2);
    return n;
}

// Neighbour count modulo 8 (eight neighbours read as 0, which is neither 2 nor 3).
struct Count3 {
    Vec b0, b1, b2;
    explicit Count3(const Neighbours& n) {
        Vec k1, x, y;
        fullAdd(n.s0, n.s1, n.s2, b0, k1);
        fullAdd(n.c0, n.c1, n.c2, x, y);
        b1 = x ^ k1;
        b2 = y ^ (x & k1);
    }
};
// Neighbour count modulo 4. Colour counts are only used where at most three
// neighbours are alive, so they are exact wherever they matter.
struct Count2 {
    Vec b0, b1;
    explicit Count2(const Neighbours& n) {
        Vec k1;
        fullAdd(n.s0, n.s1, n.s2, b0, k1);
        b1 = n.c0 ^ n.c1 ^ n.c2 ^ k1;
    }
};

// Repetition hash: the sum over all words j of the board array (padding
// included) of wordHash. It can be updated word by word. Two bits per cell,
// (red or grey, blue or grey), tell all four states apart; both are salted
// with the word position and folded by one 64x64->128 bit multiplication (as
// in wyhash).
constexpr std::uint64_t kSalt1 = 0x9e3779b97f4a7c15ull, kSalt2 = 0xd6e8feb86659fd93ull,
                        kSalt2Offset = 0xa0761d6478bd642full;
inline std::uint64_t fold(std::uint64_t a, std::uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 u128;
    const u128 p = static_cast<u128>(a) * b;
    return static_cast<std::uint64_t>(p) ^ static_cast<std::uint64_t>(p >> 64);
#else
    // Portable fallback: the high half of the product from 32-bit parts.
    const std::uint64_t al = a & 0xffffffffu, ah = a >> 32, bl = b & 0xffffffffu, bh = b >> 32;
    const std::uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const std::uint64_t mid = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
    return (a * b) ^ (hh + (lh >> 32) + (hl >> 32) + (mid >> 32));
#endif
}
inline std::uint64_t wordHash(std::uint64_t alive, std::uint64_t red, std::uint64_t blue, std::size_t j) {
    const std::uint64_t salt1 = (j + 1) * kSalt1, salt2 = (j + 1) * kSalt2 + kSalt2Offset;
    return fold((alive & ~blue) ^ salt1, (alive & ~red) ^ salt2);
}

}  // namespace

FastBattle::FastBattle(const BattleConfig& config, const std::vector<std::uint8_t>& initial) : config_(config) {
    config_.width = std::max(1, config_.width);
    config_.height = std::max(1, config_.height);
    config_.maxGenerations = std::max(0, config_.maxGenerations);
    const int w = config_.width, h = config_.height;
    words_ = (w + 63) / 64;
    lastMask_ = w % 64 == 0 ? ~0ull : (1ull << (w % 64)) - 1;
    tiles_ = (h + kTileRows - 1) / kTileRows;
    paddedRows_ = tiles_ * kTileRows;
    stride_ = static_cast<std::size_t>(paddedRows_) + 2;
    const std::size_t n = static_cast<std::size_t>(words_ + 2) * stride_;
    for (auto* plane : {&alive_, &red_bits_, &blue_bits_, &nextAlive_, &nextRed_, &nextBlue_}) plane->assign(n, 0);
    // The empty next buffers act as board t-1 (its totals are zero).
    prevHash_ = hash();
    maskWords_ = (tiles_ + 63) / 64;
    for (auto* mask : {&changed_, &lowEdge_, &highEdge_, &nextChanged_, &nextLowEdge_, &nextHighEdge_})
        mask->assign(static_cast<std::size_t>(words_ + 2) * maskWords_, 0);
    // There is no board t-2 yet: everything counts as changed.
    markAllChanged();

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const std::uint8_t c = i < initial.size() ? initial[i] : std::uint8_t{Dead};
            if (c == Dead || c > Grey) continue;
            const std::uint64_t bit = 1ull << (x % 64);
            const std::size_t j = index(x / 64, y);
            alive_[j] |= bit;
            if (c == Red) red_bits_[j] |= bit;
            if (c == Blue) blue_bits_[j] |= bit;
        }
    int alive = 0;
    for (std::size_t j = 0; j < n; ++j) {
        alive += popcount(alive_[j]);
        red_ += popcount(red_bits_[j]);
        blue_ += popcount(blue_bits_[j]);
    }
    grey_ = alive - red_ - blue_;
    hash_ = hash();
    countHome();
    redHistory_.push_back(red_ - redHome_);
    blueHistory_.push_back(blue_ - blueHome_);
    checkEnd();
}

int FastBattle::countColumns(const std::vector<std::uint64_t>& plane, int x0, int x1) const {
    int n = 0;
    for (int k = x0 / 64; k * 64 < x1; ++k) {
        const int lo = std::max(x0 - k * 64, 0), hi = std::min(x1 - k * 64, 64);
        const std::uint64_t mask = (hi == 64 ? ~0ull : (1ull << hi) - 1) & ~((1ull << lo) - 1);
        const std::uint64_t* column = &plane[index(k, 0)];
        for (int y = 0; y < config_.height; ++y) n += popcount(column[y] & mask);
    }
    return n;
}

void FastBattle::countHome() {
    if (!config_.ignoreHome) return;
    const Zones z = zonesFor(config_.width);
    redHome_ = countColumns(red_bits_, 0, z.leftEnd);
    blueHome_ = countColumns(blue_bits_, z.rightStart, config_.width);
}

void FastBattle::markAllChanged() {
    for (int k = 0; k < words_; ++k)
        for (int m = 0; m < maskWords_; ++m) {
            const std::size_t i = static_cast<std::size_t>(k + 1) * maskWords_ + m;
            const int bits = std::min(64, tiles_ - 64 * m);
            changed_[i] = lowEdge_[i] = highEdge_[i] = bits == 64 ? ~0ull : (1ull << bits) - 1;
        }
}

Cell FastBattle::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return Dead;
    const std::size_t j = index(x / 64, y);
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

    const int words = words_;
    const int mw = maskWords_;
    const bool contagion = config_.variant == Variant::Contagion;
    const int lastTile = tiles_ - 1;
    // Valid bits of a column's last mask word.
    const std::uint64_t lastTileBits = tiles_ % 64 == 0 ? ~0ull : (1ull << (tiles_ % 64)) - 1;
    // Rows of the last tile below the board must stay empty.
    Vec lastRows = broadcast(0);
    for (int i = 0; i < kTileRows; ++i)
        if (lastTile * kTileRows + i < config_.height) lastRows[i] = ~0ull;

    // The next buffers hold board t-1. A tile is only recomputed where board t
    // differs from board t-2 somewhere in its neighbourhood; elsewhere board
    // t+1 equals board t-1 and the buffer is already right.
    Vec dAlive = broadcast(0), dRed = broadcast(0), dBlue = broadcast(0);
    std::uint64_t dHash = 0;
    Vec laneSalt1, laneSalt2;
    for (int i = 0; i < kTileRows; ++i) {
        laneSalt1[i] = static_cast<std::uint64_t>(i) * kSalt1;
        laneSalt2[i] = static_cast<std::uint64_t>(i) * kSalt2;
    }
    for (int k = 0; k < words; ++k) {
        const std::size_t mrow = static_cast<std::size_t>(k + 1) * mw;
        // Tiles of column k that see a change: in the column itself, in bit 63
        // of the column to the left or in bit 0 of the column to the right,
        // each in the same tile or the ones above and below.
        auto source = [&](int m) -> std::uint64_t {
            return m < 0 || m >= mw ? 0 : changed_[mrow + m] | highEdge_[mrow - mw + m] | lowEdge_[mrow + mw + m];
        };
        const Vec columnMask = broadcast(k + 1 == words ? lastMask_ : ~0ull);
        const std::size_t base = index(k, 0);
        const std::uint64_t* aL = alive_.data() + base - stride_;
        const std::uint64_t* aC = alive_.data() + base;
        const std::uint64_t* aR = alive_.data() + base + stride_;
        const std::uint64_t* rL = red_bits_.data() + base - stride_;
        const std::uint64_t* rC = red_bits_.data() + base;
        const std::uint64_t* rR = red_bits_.data() + base + stride_;
        const std::uint64_t* bL = blue_bits_.data() + base - stride_;
        const std::uint64_t* bC = blue_bits_.data() + base;
        const std::uint64_t* bR = blue_bits_.data() + base + stride_;
        std::uint64_t* nA = nextAlive_.data() + base;
        std::uint64_t* nR = nextRed_.data() + base;
        std::uint64_t* nB = nextBlue_.data() + base;

        for (int m = 0; m < mw; ++m) {
            const std::uint64_t v = source(m);
            std::uint64_t dirty = v | (v << 1) | (v >> 1) | (source(m - 1) >> 63) | (source(m + 1) << 63);
            if (m + 1 == mw) dirty &= lastTileBits;
            std::uint64_t changedBits = 0, lowBits = 0, highBits = 0;
            for (; dirty; dirty &= dirty - 1) {
                const int bit = lowestBit(dirty);
                const int tile = m * 64 + bit;
                const int y0 = tile * kTileRows;

                const Count3 alive(neighbours(aL + y0, aC + y0, aR + y0));
                const Vec self = load(aC + y0);
                const Vec three = alive.b0 & alive.b1 & ~alive.b2;
                const Vec two = ~alive.b0 & alive.b1 & ~alive.b2;
                const Vec valid = tile == lastTile ? columnMask & lastRows : columnMask;
                const Vec birth = ~self & three & valid;
                const Vec survive = self & (two | three);
                const Vec na = birth | survive;
                Vec nr, nb;
                if (!contagion && !any(birth)) {
                    // Survivors keep their colour; no vote needed.
                    nr = survive & load(rC + y0);
                    nb = survive & load(bC + y0);
                } else {
                    const Count2 red(neighbours(rL + y0, rC + y0, rR + y0));
                    const Count2 blue(neighbours(bL + y0, bC + y0, bR + y0));
                    // Majority vote: compare the two-bit red and blue counts.
                    const Vec same1 = ~(red.b1 ^ blue.b1);
                    const Vec redWins = (red.b1 & ~blue.b1) | (same1 & red.b0 & ~blue.b0);
                    const Vec blueWins = (blue.b1 & ~red.b1) | (same1 & blue.b0 & ~red.b0);
                    if (contagion) {
                        nr = na & redWins;
                        nb = na & blueWins;
                    } else {
                        nr = (birth & redWins) | (survive & load(rC + y0));
                        nb = (birth & blueWins) | (survive & load(bC + y0));
                    }
                }

                const Vec oa = load(nA + y0), orr = load(nR + y0), ob = load(nB + y0);
                const Vec diff = (na ^ oa) | (nr ^ orr) | (nb ^ ob);
                if (!any(diff)) continue;
                store(nA + y0, na);
                store(nR + y0, nr);
                store(nB + y0, nb);
                changedBits |= 1ull << bit;
                if (any(diff & broadcast(1))) lowBits |= 1ull << bit;
                if (any(diff >> 63)) highBits |= 1ull << bit;
                // Totals and hash: unchanged words add zero.
                dAlive = dAlive + popcounts(na) - popcounts(oa);
                dRed = dRed + popcounts(nr) - popcounts(orr);
                dBlue = dBlue + popcounts(nb) - popcounts(ob);
                const std::uint64_t j = base + static_cast<std::size_t>(y0) + 1;
                const Vec salt1 = broadcast(j * kSalt1) + laneSalt1;
                const Vec salt2 = broadcast(j * kSalt2 + kSalt2Offset) + laneSalt2;
                const Vec nu = (na & ~nb) ^ salt1, nv = (na & ~nr) ^ salt2;
                const Vec ou = (oa & ~ob) ^ salt1, ov = (oa & ~orr) ^ salt2;
                for (int i = 0; i < kTileRows; ++i) dHash += fold(nu[i], nv[i]) - fold(ou[i], ov[i]);
            }
            nextChanged_[mrow + m] = changedBits;
            nextLowEdge_[mrow + m] = lowBits;
            nextHighEdge_[mrow + m] = highBits;
        }
    }

    alive_.swap(nextAlive_);
    red_bits_.swap(nextRed_);
    blue_bits_.swap(nextBlue_);
    changed_.swap(nextChanged_);
    lowEdge_.swap(nextLowEdge_);
    highEdge_.swap(nextHighEdge_);
    ++generation_;
    // Board 1 was compared with the empty buffer, not with a board -1.
    if (generation_ == 1) markAllChanged();

    // New totals = totals of board t-1 plus the differences.
    const int alive = prevAlive_ + static_cast<int>(sum(dAlive));
    const int red = prevRed_ + static_cast<int>(sum(dRed));
    const int blue = prevBlue_ + static_cast<int>(sum(dBlue));
    prevAlive_ = red_ + blue_ + grey_;
    prevRed_ = red_;
    prevBlue_ = blue_;
    red_ = red;
    blue_ = blue;
    grey_ = alive - red - blue;
    const std::uint64_t nextHash = prevHash_ + dHash;
    prevHash_ = hash_;
    hash_ = nextHash;

    countHome();
    redHistory_.push_back(red_ - redHome_);
    blueHistory_.push_back(blue_ - blueHome_);
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

// Same end conditions and scores as Battle::checkEnd.
void FastBattle::checkEnd() {
    auto decideByScore = [this](EndReason reason) {
        reason_ = reason;
        if (redScore_ > blueScore_) winner_ = Winner::Red;
        else if (blueScore_ > redScore_) winner_ = Winner::Blue;
        else winner_ = Winner::Draw;
    };

    redScore_ = red_ - redHome_;
    blueScore_ = blue_ - blueHome_;

    if (red_ == 0 || (blue_ == 0 && !config_.solo)) {
        reason_ = EndReason::Elimination;
        winner_ = red_ > 0 ? Winner::Red : blue_ > 0 ? Winner::Blue : Winner::Draw;
        return;
    }

    const auto [it, inserted] = seen_.emplace(hash_, generation_);
    if (inserted) {
        seenOrder_.push_back(hash_);
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
    std::uint64_t h = 0;
    for (std::size_t j = 0; j < alive_.size(); ++j) h += wordHash(alive_[j], red_bits_[j], blue_bits_[j], j);
    return h;
}

}  // namespace gol
