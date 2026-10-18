// Minimal self-contained tests for the simulation core.
#include "gol/battle.hpp"
#include "gol/fast_battle.hpp"
#include "gol/life.hpp"
#include "gol/patterns.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace gol;

static void stamp(LifeGrid& g, const Pattern& p, int x, int y) {
    for (const auto& c : p.cells) g.set(x + c.x, y + c.y, true);
}

static std::set<Point> liveCells(const LifeGrid& g) {
    std::set<Point> s;
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x)
            if (g.alive(x, y)) s.insert({x, y});
    return s;
}

static std::set<Point> shifted(std::set<Point> s, int dx, int dy) {
    std::set<Point> out;
    for (auto p : s) out.insert({p.x + dx, p.y + dy});
    return out;
}

// A spaceship must reappear after `period` generations shifted by (dx,dy).
static void checkSpaceship(const char* id, int period, int dx, int dy) {
    LifeGrid g(80, 80, false);
    stamp(g, *findPattern(id), 40, 40);
    const auto before = liveCells(g);
    for (int i = 0; i < period; ++i) g.step();
    const bool ok = liveCells(g) == shifted(before, dx, dy);
    if (!ok) std::printf("  spaceship %s did not translate as expected\n", id);
    CHECK(ok);
}

static void checkOscillator(const char* id, int period) {
    LifeGrid g(60, 60, false);
    stamp(g, *findPattern(id), 20, 20);
    const auto before = liveCells(g);
    for (int i = 0; i < period; ++i) g.step();
    const bool ok = liveCells(g) == before;
    if (!ok) std::printf("  oscillator %s has wrong period\n", id);
    CHECK(ok);
}

static void testClassic() {
    CHECK(parseRle("bo$2bo$3o!").size() == 5);
    const Pattern& gosper = *findPattern("gosper_gun");
    CHECK(gosper.width == 36 && gosper.height == 9 && gosper.cells.size() == 36);

    checkOscillator("blinker", 2);
    checkOscillator("toad", 2);
    checkOscillator("beacon", 2);
    checkOscillator("pulsar", 3);
    checkOscillator("pentadecathlon", 15);
    checkOscillator("block", 1);
    checkOscillator("eater1", 1);

    checkSpaceship("glider", 4, 1, 1);
    checkSpaceship("lwss", 4, 2, 0);
    checkSpaceship("mwss", 4, 2, 0);
    checkSpaceship("hwss", 4, 2, 0);
    checkSpaceship("copperhead", 10, 1, 0);

    // Guns: the population must keep growing period after period.
    for (auto [id, period] : {std::pair{"gosper_gun", 30}, std::pair{"simkin_gun", 120}}) {
        LifeGrid g(400, 400, false);
        stamp(g, *findPattern(id), 200, 200);
        std::size_t last = 0;
        bool growing = true;
        for (int k = 1; k <= 4; ++k) {
            for (int i = 0; i < period; ++i) g.step();
            if (k > 1 && g.population() <= last) growing = false;
            last = g.population();
        }
        if (!growing) std::printf("  gun %s does not emit gliders\n", id);
        CHECK(growing);
    }

    // Diehard vanishes after 130 generations.
    LifeGrid g(100, 100, false);
    stamp(g, *findPattern("diehard"), 45, 45);
    for (int i = 0; i < 130; ++i) g.step();
    CHECK(g.population() == 0);

    // Transformations keep the cell count and rotate the bounding box.
    const Pattern r = transformed(gosper, 1, false);
    CHECK(r.width == 9 && r.height == 36 && r.cells.size() == 36);
    const Pattern back = transformed(transformed(gosper, 2, true), 2, true);
    CHECK(back.cells == gosper.cells);
}

static std::vector<std::uint8_t> board(int w, int h, std::initializer_list<std::tuple<int, int, Cell>> cells) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(w) * h, Dead);
    for (auto [x, y, c] : cells) b[static_cast<std::size_t>(y) * w + x] = c;
    return b;
}

static void testBattle() {
    CHECK(majorityColour(2, 1) == Red);
    CHECK(majorityColour(0, 1) == Blue);
    CHECK(majorityColour(1, 1) == Grey);
    CHECK(majorityColour(0, 0) == Grey);

    BattleConfig cfg{20, 20, Variant::BirthOnly, 100};

    // Birth: two red + one blue parent -> red child (vertical blinker -> horizontal).
    // Far away a lone blue block keeps blue alive so nobody is eliminated.
    {
        Battle b(cfg, board(20, 20, {{5, 4, Red}, {5, 5, Red}, {5, 6, Blue},
                                     {15, 15, Blue}, {16, 15, Blue}, {15, 16, Blue}, {16, 16, Blue}}));
        b.step();
        CHECK(b.at(4, 5) == Red);
        CHECK(b.at(6, 5) == Red);
        CHECK(b.at(5, 5) == Red);  // survivor keeps its colour
    }
    // Tie: red + blue + grey parent -> grey child.
    {
        Battle b(cfg, board(20, 20, {{5, 4, Red}, {5, 5, Grey}, {5, 6, Blue}}));
        b.step();
        CHECK(b.at(4, 5) == Grey);
        CHECK(b.at(6, 5) == Grey);
        CHECK(b.finished() && b.winner() == Winner::Draw && b.endReason() == EndReason::Elimination);
    }
    // Contagion: survivors are re-coloured by the majority of their neighbours.
    {
        BattleConfig c2 = cfg;
        c2.variant = Variant::Contagion;
        Battle b(c2, board(20, 20, {{5, 4, Blue}, {5, 5, Red}, {5, 6, Blue},
                                    {15, 15, Red}, {16, 15, Red}, {15, 16, Red}, {16, 16, Red}}));
        b.step();
        CHECK(b.at(5, 5) == Blue);
    }
    // Elimination at generation 0.
    {
        Battle b(cfg, board(20, 20, {{1, 1, Red}, {2, 1, Red}, {1, 2, Red}, {2, 2, Red}}));
        CHECK(b.finished() && b.winner() == Winner::Red && b.generation() == 0);
    }
    // Repetition: a red blinker (3 cells) vs a blue block (4 cells) -> period 2 cycle, blue wins.
    {
        Battle b(cfg, board(20, 20, {{3, 3, Red}, {4, 3, Red}, {5, 3, Red},
                                     {15, 15, Blue}, {16, 15, Blue}, {15, 16, Blue}, {16, 16, Blue}}));
        b.runToEnd();
        CHECK(b.endReason() == EndReason::Repetition);
        CHECK(b.period() == 2);
        CHECK(b.winner() == Winner::Blue);
        CHECK(b.generation() == 2);
    }
    // Generation limit with growing gliders is decided by counts.
    {
        BattleConfig c3{60, 60, Variant::BirthOnly, 10};
        auto cells = board(60, 60, {{2, 2, Red}, {3, 2, Red}, {2, 3, Red}, {3, 3, Red}});
        const Pattern& glider = *findPattern("glider");
        for (auto p : glider.cells) cells[static_cast<std::size_t>(30 + p.y) * 60 + 30 + p.x] = Blue;
        Battle b(c3, cells);
        b.runToEnd();
        CHECK(b.endReason() == EndReason::GenerationLimit && b.generation() == 10);
        CHECK(b.winner() == Winner::Blue);
        CHECK(b.redHistory().size() == 11);
    }
    // No generation limit: the glider flies into the dead edge, settles, and the frozen board ends it.
    {
        BattleConfig c4{60, 60, Variant::BirthOnly, 0};
        auto cells = board(60, 60, {{2, 2, Red}, {3, 2, Red}, {2, 3, Red}, {3, 3, Red}});
        const Pattern& glider = *findPattern("glider");
        for (auto p : glider.cells) cells[static_cast<std::size_t>(30 + p.y) * 60 + 30 + p.x] = Blue;
        Battle b(c4, cells);
        CHECK(b.unlimited());
        b.advance(100000);
        CHECK(b.finished() && b.endReason() == EndReason::Repetition && b.generation() > 50);
    }
    // Home cells do not count: two red blocks in red's own area (8 cells) vs one
    // blue block in the middle (4 cells). The frozen board is red's with all
    // cells counted, blue's with ignoreHome. A red army that only stays at home
    // still wins by elimination.
    {
        auto cells = board(50, 20, {{2, 2, Red}, {3, 2, Red}, {2, 3, Red}, {3, 3, Red},
                                    {2, 8, Red}, {3, 8, Red}, {2, 9, Red}, {3, 9, Red},
                                    {25, 5, Blue}, {26, 5, Blue}, {25, 6, Blue}, {26, 6, Blue}});
        BattleConfig c5{50, 20, Variant::BirthOnly, 100};
        Battle all(c5, cells);
        all.runToEnd();
        CHECK(all.endReason() == EndReason::Repetition && all.winner() == Winner::Red);
        c5.ignoreHome = true;
        Battle away(c5, cells);
        CHECK(away.redHome() == 8 && away.blueHome() == 0 && away.redScore() == 0 && away.blueScore() == 4);
        away.runToEnd();
        CHECK(away.endReason() == EndReason::Repetition && away.winner() == Winner::Blue);
        CHECK(away.redScoreHistory().back() == 0 && away.redHistory().back() == 8);
        Battle alone(c5, board(50, 20, {{2, 2, Red}, {3, 2, Red}, {2, 3, Red}, {3, 3, Red}}));
        CHECK(alone.finished() && alone.endReason() == EndReason::Elimination && alone.winner() == Winner::Red);
    }

    // Board encoding round trip.
    {
        auto cells = board(7, 3, {{0, 0, Red}, {1, 0, Red}, {6, 2, Grey}, {3, 1, Blue}});
        const std::string text = encodeBoard(cells);
        std::vector<std::uint8_t> back(cells.size());
        CHECK(decodeBoard(text, back) && back == cells);
        std::vector<std::uint8_t> small(3);
        CHECK(!decodeBoard("5.", small));
    }

    // Battlefields stay inside the middle area and symmetric presets are symmetric.
    for (const auto& preset : battlefieldPresets()) {
        const int w = 260, h = 128;
        const auto field = makeBattlefield(preset.id, w, h, 800, 42);
        const Zones z = zonesFor(w);
        bool inside = true, mirrored = true, rotational = true;
        int count = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto c = field[static_cast<std::size_t>(y) * w + x];
                if (c != Dead) {
                    ++count;
                    if (x < z.leftEnd || x >= z.rightStart || c != Grey) inside = false;
                }
                if (c != field[static_cast<std::size_t>(y) * w + (w - 1 - x)]) mirrored = false;
                if (c != field[static_cast<std::size_t>(h - 1 - y) * w + (w - 1 - x)]) rotational = false;
            }
        if (!inside) std::printf("  preset %s leaves the battlefield\n", preset.id.c_str());
        CHECK(inside);
        if (preset.id != "random" && preset.id != "random_rotational" && !mirrored)
            std::printf("  preset %s is not mirror symmetric\n", preset.id.c_str());
        if (preset.id == "random_rotational") CHECK(rotational);
        else if (preset.id != "random") CHECK(mirrored);
        if (preset.id != "empty") CHECK(count > 0);
        if (preset.id == "random") CHECK(count == 800);
    }
}

// Runs both engines side by side and compares every generation cell by cell,
// and the result at the end. Returns false (and prints `what`) on a mismatch.
static bool sameAsReference(const BattleConfig& cfg, const std::vector<std::uint8_t>& cells, const char* what) {
    Battle slow(cfg, cells);
    FastBattle fast(cfg, cells);
    while (true) {
        if (fast.cells() != slow.cells() || fast.redCount() != slow.redCount() ||
            fast.blueCount() != slow.blueCount() || fast.greyCount() != slow.greyCount() ||
            fast.redScore() != slow.redScore() || fast.blueScore() != slow.blueScore() ||
            fast.finished() != slow.finished() || fast.generation() != slow.generation()) {
            std::printf("  fast engine differs (%s): %dx%d variant=%d gen=%d\n", what, cfg.width, cfg.height,
                        static_cast<int>(cfg.variant), slow.generation());
            return false;
        }
        if (slow.finished()) {
            if (fast.winner() != slow.winner() || fast.endReason() != slow.endReason() ||
                fast.period() != slow.period() || fast.redScore() != slow.redScore() ||
                fast.blueScore() != slow.blueScore()) {
                std::printf("  fast engine result differs (%s): %dx%d\n", what, cfg.width, cfg.height);
                return false;
            }
            return true;
        }
        slow.step();
        fast.step();
    }
}

// The bit-parallel engine must match the reference engine cell by cell,
// including word boundaries (widths around multiples of 64), tile boundaries
// (heights around multiples of 4, more than 256 rows = more than one mask
// word per column) and both variants.
static void testFastBattle() {
    std::mt19937_64 rng(7);
    int mismatches = 0;
    std::uniform_int_distribution<int> pct(0, 99), colour(1, 3);
    auto soup = [&](int w, int h, int density) {
        std::vector<std::uint8_t> cells(static_cast<std::size_t>(w) * h);
        for (auto& c : cells) c = pct(rng) < density ? static_cast<std::uint8_t>(colour(rng)) : std::uint8_t{Dead};
        return cells;
    };
    for (const int w : {1, 5, 63, 64, 65, 130, 200, 330, 340})
        for (const Variant v : {Variant::BirthOnly, Variant::Contagion})
            for (const int density : {15, 40})
                for (const bool ignoreHome : {false, true})
                    mismatches += !sameAsReference({w, 37, v, 400, ignoreHome}, soup(w, 37, density), "soup");
    for (const int h : {1, 2, 3, 4, 5, 8, 9, 300})
        for (const Variant v : {Variant::BirthOnly, Variant::Contagion})
            mismatches += !sameAsReference({70, h, v, 300}, soup(70, h, 30), "height");

    // Sparse armies on the battlefield presets: long runs that end in still
    // lifes or oscillators of various periods (pulsars: 3, pentadecathlons: 15),
    // so the skipping of unchanged tiles and the repetition check are exercised.
    for (const auto& preset : battlefieldPresets())
        for (const Variant v : {Variant::BirthOnly, Variant::Contagion})
            for (const bool ignoreHome : {false, true}) {
            const int w = 160, h = 96;
            auto cells = makeBattlefield(preset.id, w, h, 400, 11);
            const Zones z = zonesFor(w);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const bool left = x < z.leftEnd, right = x >= z.rightStart;
                    if ((left || right) && pct(rng) < 12) cells[static_cast<std::size_t>(y) * w + x] = left ? Red : Blue;
                }
            mismatches += !sameAsReference({w, h, v, 1500, ignoreHome}, cells, preset.id.c_str());
        }
    CHECK(mismatches == 0);
}

// The random battlefields must be the same on every platform: bots are trained
// on a battlefield given by its seed, and the browser (WebAssembly) build and
// both peers of an online game regenerate it from that seed. These fingerprints
// were taken with the original libstdc++ <random> distributions.
static void testBattlefieldsArePortable() {
    struct Case {
        const char* preset;
        int w, h, count;
        std::uint64_t seed, fnv;
    };
    const Case cases[] = {
        {"random_mirror", 260, 128, 900, 1, 0xafafdafc53e5e807ull},
        {"random_rotational", 360, 176, 900, 123456789, 0xf42be7a25858ba8full},
        {"random", 200, 100, 5000, 7, 0x903324589628937dull},
        {"reactor", 260, 128, 900, 4503599627370496ull, 0x21f7e464bf12b203ull},
        {"reactor", 77, 33, 5000, 0, 0x19340c0188f29c11ull},
    };
    for (const auto& c : cases) {
        std::uint64_t h = 1469598103934665603ull;
        for (auto cell : makeBattlefield(c.preset, c.w, c.h, c.count, c.seed)) h = (h ^ cell) * 1099511628211ull;
        if (h != c.fnv) std::printf("battlefield %s seed %llu differs\n", c.preset, (unsigned long long)c.seed);
        CHECK(h == c.fnv);
    }
}

// A piece costs its number of live cells.
static void testArsenalPrices() {
    CHECK(!battleArsenal().empty());
    for (const auto& item : battleArsenal()) {
        const Pattern* p = findPattern(item.patternId);
        CHECK(p != nullptr);
        if (p) CHECK(item.cost == static_cast<int>(p->cells.size()));
    }
}

int main() {
    testArsenalPrices();
    testBattlefieldsArePortable();
    testClassic();
    testBattle();
    testFastBattle();
    if (failures == 0) std::printf("All core tests passed.\n");
    return failures == 0 ? 0 : 1;
}
