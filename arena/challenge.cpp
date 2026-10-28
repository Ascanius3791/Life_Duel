// life_challenge: how far can the computer push a Challenge level of Life Duel?
//
// A Challenge is won by beating the level's fixed bot (Blue). The record is the
// fewest living red cells on the starting board, and like in the browser the
// battle runs to its end (no generation limit) with home cells not counted.
// Battles are deterministic, so one battle decides whether an army wins.
//
// The search works on sets of red cells in Red's zone, not on bought pieces:
// cells are what the record counts, and any set of cells can be drawn in the
// browser. It never sees the records players found; it starts from random
// armies made of arsenal pieces.
//
//  1. Seeding: random armies of arsenal pieces with random sizes, until enough
//     of them win.
//  2. Shrinking: an archive keeps winners by cell count (only the smallest
//     counts, a few dozen armies each, preferring the larger winning margin).
//     Each round mutates archive armies, mostly towards fewer cells (delete a
//     cell, a cluster, two cells for one new one, replace a cluster by a
//     smaller pattern) but also sideways (move a cell, shift a cluster or the
//     whole army), because in a chaotic game a neighbour of a winner that keeps
//     its size is often the one from which a smaller winner can be reached.
//     A new smallest winner gets a sweep over all its single-cell deletions.
//     Part of every round are fresh random armies smaller than the record.
//
// Results go to a text file, one winner per line with its LD1 army code, which
// the browser imports under My armies.
#include "arena.hpp"

#include "gol/battle.hpp"
#include "gol/fast_battle.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace arena;

namespace {

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

int pick(Rng& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
bool chance(Rng& rng, double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

// ---------------------------------------------------------------------------
// Levels (as in battle/web/levels.js)
// ---------------------------------------------------------------------------
struct Level {
    const char* id;
    const char* preset;
    const char* bot;  // Blue's army, written in Red's frame
};

const Level kLevels[] = {
    {"hive", "hive_lattice",
     "beehive@6,2,r3,m1;block@38,0,r3,m1;block@2,0,r0,m0;r_pentomino@20,18,r1,m0;cell@6,21,r2,m0;"
     "r_pentomino@36,41,r3,m1;beehive@5,99,r0,m0;r_pentomino@49,49,r0,m1;beehive@42,77,r0,m1;beehive@1,76,r2,m1;"
     "blinker@44,90,r1,m1;cell@49,69,r1,m1;beehive@17,69,r2,m0;cell@20,80,r2,m0;gosper_gun@28,88,r3,m0;"
     "cell@47,107,r2,m0;pulsar@1,112,r2,m0;cell@27,121,r2,m1;glider@39,16,r3,m0;cell@5,54,r2,m0"},
    {"empty", "empty",
     "acorn@36,6,r0,m1;mwss@44,8,r2,m1;acorn@30,51,r0,m1;cell@22,56,r1,m0;cell@21,33,r1,m1;acorn@17,15,r1,m0;"
     "blinker@11,36,r2,m1;hwss@25,65,r2,m1;mwss@46,76,r2,m0;glider@23,95,r1,m0;beehive@18,103,r3,m0;"
     "cell@21,103,r0,m1;hwss@42,119,r2,m1;acorn@45,110,r2,m0"},
    {"champion", "empty",
     "acorn@36,6,r0,m1;mwss@46,5,r2,m1;block@41,36,r3,m0;mwss@43,43,r2,m1;acorn@30,57,r0,m1;"
     "mwss@46,57,r0,m0;cell@21,44,r2,m1;cell@44,113,r1,m1;blinker@48,35,r2,m1;cell@16,36,r3,m0;beehive@14,59,r1,m0;"
     "lwss@36,86,r2,m1;cell@31,108,r3,m0;acorn@40,109,r2,m0;lwss@45,123,r0,m0;cell@39,55,r3,m1;acorn@22,119,r2,m0;"
     "cell@26,125,r1,m1;cell@35,124,r1,m0;cell@9,102,r0,m1"},
};

// ---------------------------------------------------------------------------
// Red armies as cell sets
// ---------------------------------------------------------------------------
// A cell of Red's zone, packed as y * zoneWidth + x. An army is a sorted list
// without duplicates.
using CellSet = std::vector<int>;

struct Outcome {
    bool won = false;
    bool capped = false;  // hit the safety cap before the battle ended (counts as not won)
    gol::EndReason reason = gol::EndReason::None;
    int generation = 0;
    double red = 0, blue = 0;  // final scores
    // Red's share of the counted cells at the end in [-1, 1]; 1 for an elimination win.
    double margin() const {
        if (reason == gol::EndReason::Elimination) return won ? 1 : -1;
        return red + blue > 0 ? (red - blue) / (red + blue) : 0;
    }
};

class Challenge {
public:
    Challenge(const Level& level, int cap) : arena_(settingsFor(level)), cap_(cap) {
        Army bot;
        if (!parsePieces(arena_, level.bot, bot)) {
            std::fprintf(stderr, "cannot parse the bot of level %s\n", level.id);
            std::exit(1);
        }
        base_ = arena_.board(Army{}, bot);
        zw_ = arena_.zones().leftEnd;
    }

    int zoneWidth() const { return zw_; }
    int height() const { return arena_.settings().height; }
    const Arena& arena() const { return arena_; }

    std::vector<std::uint8_t> board(const CellSet& red) const {
        std::vector<std::uint8_t> cells = base_;
        const int w = arena_.settings().width;
        for (int c : red) cells[static_cast<std::size_t>(c / zw_) * w + c % zw_] = gol::Red;
        return cells;
    }

    gol::BattleConfig config() const {
        gol::BattleConfig config{arena_.settings().width, arena_.settings().height, arena_.settings().variant, 0};
        config.ignoreHome = true;
        return config;
    }

    Outcome play(const CellSet& red) const {
        gol::FastBattle battle(config(), board(red));
        while (!battle.finished() && battle.generation() < cap_) battle.step();
        Outcome o;
        o.capped = !battle.finished();
        o.won = battle.winner() == gol::Winner::Red;
        o.reason = battle.endReason();
        o.generation = battle.generation();
        o.red = battle.redScore();
        o.blue = battle.blueScore();
        return o;
    }

    // The same battle with the reference engine (the one the browser runs).
    Outcome verify(const CellSet& red) const {
        gol::Battle battle(config(), board(red));
        while (!battle.finished() && battle.generation() < cap_) battle.step();
        Outcome o;
        o.capped = !battle.finished();
        o.won = battle.winner() == gol::Winner::Red;
        o.reason = battle.endReason();
        o.generation = battle.generation();
        o.red = battle.redScore();
        o.blue = battle.blueScore();
        return o;
    }

    // Adds the cells of an arsenal piece at (x, y), clipped to the zone.
    void stamp(CellSet& set, const gol::Pattern& shape, int x, int y) const {
        for (const auto& c : shape.cells) {
            const int cx = x + c.x, cy = y + c.y;
            if (cx >= 0 && cx < zw_ && cy >= 0 && cy < height()) set.push_back(cy * zw_ + cx);
        }
    }

private:
    static Settings settingsFor(const Level& level) {
        Settings s;
        s.preset = level.preset;
        s.count = 900;
        s.fieldSeed = 1;
        s.maxGenerations = 0;
        return s;
    }

    Arena arena_;
    int cap_;
    std::vector<std::uint8_t> base_;  // grey battlefield and the bot
    int zw_ = 0;
};

void normalise(CellSet& s) {
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
}

std::string key(const CellSet& s) {
    std::string k(s.size() * 2, '\0');
    for (std::size_t i = 0; i < s.size(); ++i) {
        k[2 * i] = static_cast<char>(s[i] & 0xff);
        k[2 * i + 1] = static_cast<char>(s[i] >> 8);
    }
    return k;
}

// The army as `cell` pieces: the LD1 code the browser imports, and the
// life_arena form.
std::string ld1Code(const Challenge& ch, const CellSet& s, const std::string& name) {
    std::string out = "LD1|" + std::to_string(ch.zoneWidth()) + "x" + std::to_string(ch.height()) + "|" + name + "|";
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) out += ',';
        out += "cell." + std::to_string(s[i] % ch.zoneWidth()) + "." + std::to_string(s[i] / ch.zoneWidth()) + ".0.0";
    }
    return out;
}

std::string piecesText(const Challenge& ch, const CellSet& s) {
    std::string out;
    for (int c : s) {
        if (!out.empty()) out += ';';
        out += "cell@" + std::to_string(c % ch.zoneWidth()) + "," + std::to_string(c / ch.zoneWidth()) + ",r0,m0";
    }
    return out;
}

bool parseCells(const Challenge& ch, const std::string& text, CellSet& s) {
    Army army;
    if (!parsePieces(ch.arena(), text, army)) return false;
    s.clear();
    for (const auto& p : army.pieces) ch.stamp(s, ch.arena().shape(p), p.x, p.y);
    normalise(s);
    return true;
}

// Picture of the army's bounding box ('#' = cell), with its offset.
std::string picture(const Challenge& ch, const CellSet& s) {
    if (s.empty()) return "(empty)\n";
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    for (int c : s) {
        x0 = std::min(x0, c % ch.zoneWidth()), x1 = std::max(x1, c % ch.zoneWidth());
        y0 = std::min(y0, c / ch.zoneWidth()), y1 = std::max(y1, c / ch.zoneWidth());
    }
    std::string out = "  at x=" + std::to_string(x0) + ", y=" + std::to_string(y0) + ":\n";
    for (int y = y0; y <= y1; ++y) {
        out += "  ";
        for (int x = x0; x <= x1; ++x) out += std::binary_search(s.begin(), s.end(), y * ch.zoneWidth() + x) ? '#' : '.';
        out += '\n';
    }
    return out;
}

const char* reasonName(gol::EndReason r) {
    switch (r) {
        case gol::EndReason::Elimination: return "elimination";
        case gol::EndReason::Repetition: return "repetition";
        case gol::EndReason::GenerationLimit: return "limit";
        default: return "unfinished";
    }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------
struct Winner {
    CellSet cells;
    Outcome outcome;
    std::string origin;
};

struct Options {
    std::string level = "hive";
    double minutes = 10;
    int threads = 0;
    std::uint64_t rng = 1;
    int cap = 30000;
    std::string out;
    int perCount = 24;      // archive: armies kept per cell count
    int slack = 4;          // archive: counts kept above the smallest
    int seedWinners = 32;   // seeding stops at this many winners...
    double seedShare = 0.2; // ... or this share of the time
    std::vector<std::string> start;  // armies (life_arena form) to start from instead of seeding
    bool bench = false;
};

class Search {
public:
    Search(const Challenge& ch, const Options& o) : ch_(ch), o_(o), rng_(o.rng) {
        const auto& arsenal = gol::battleArsenal();
        for (int i = 0; i < static_cast<int>(arsenal.size()); ++i) items_.push_back(i);
        for (const auto& item : arsenal) {
            const gol::Pattern* p = gol::findPattern(item.patternId);
            for (int rot = 0; rot < 4; ++rot)
                for (int m = 0; m < 2; ++m) shapes_.push_back(gol::transformed(*p, rot, m == 1));
        }
    }

    void run() {
        start_ = Clock::now();
        for (const auto& text : o_.start) {
            CellSet s;
            if (!parseCells(ch_, text, s)) {
                std::fprintf(stderr, "cannot parse start army: %s\n", text.c_str());
                std::exit(1);
            }
            evaluate({{s, "start"}});
        }
        const int batch = threads() * 6;
        if (o_.start.empty()) {
            // 1. Seeding
            int round = 0;
            while (archiveSize() < o_.seedWinners && secondsSince(start_) < o_.minutes * 60 * o_.seedShare) {
                std::vector<Candidate> cands;
                for (int i = 0; i < batch; ++i) cands.push_back({randomArmy(pick(rng_, 3, budget())), "random"});
                evaluate(cands);
                report("seed", ++round);
            }
        }
        // 2. Shrinking
        int round = 0;
        int lastBest = best();
        while (secondsSince(start_) < o_.minutes * 60) {
            std::vector<Candidate> cands;
            if (archiveSize() == 0) {
                for (int i = 0; i < batch; ++i) cands.push_back({randomArmy(pick(rng_, 3, budget())), "random"});
            } else if (best() < lastBest || sweepPending_) {
                // A new record: try every single-cell deletion of it.
                sweepPending_ = false;
                for (const auto& w : archive_.begin()->second) {
                    for (std::size_t i = 0; i < w.cells.size(); ++i) {
                        CellSet s = w.cells;
                        s.erase(s.begin() + static_cast<long>(i));
                        cands.push_back({s, w.origin + " -1"});
                    }
                    if (static_cast<int>(cands.size()) >= batch * 4) break;
                }
            } else {
                for (int i = 0; i < batch; ++i) {
                    if (chance(rng_, 0.08)) cands.push_back({randomArmy(pick(rng_, 3, std::max(3, best() - 1))), "random"});
                    else cands.push_back(mutate());
                }
            }
            lastBest = best();
            evaluate(cands);
            report("shrink", ++round);
        }
        report("done", round, true);
    }

    int best() const { return archive_.empty() ? 1 << 20 : archive_.begin()->first; }

    void bench() {
        start_ = Clock::now();
        std::vector<Candidate> cands;
        for (int i = 0; i < threads() * 4; ++i) cands.push_back({randomArmy(pick(rng_, 3, budget())), "random"});
        std::vector<Outcome> outcomes(cands.size());
        std::vector<double> seconds(cands.size());
        parallelFor(static_cast<int>(cands.size()), threads(), [&](int i) {
            const auto t = Clock::now();
            outcomes[i] = ch_.play(cands[i].cells);
            seconds[i] = secondsSince(t);
        });
        int won = 0, capped = 0;
        for (std::size_t i = 0; i < cands.size(); ++i) {
            const auto& o = outcomes[i];
            won += o.won, capped += o.capped;
            std::printf("%3zu cells  %-4s %-11s gen %6d  red %8.1f blue %8.1f  %.2fs\n", cands[i].cells.size(),
                        o.won ? "WIN" : "-", reasonName(o.reason), o.generation, o.red, o.blue, seconds[i]);
        }
        std::printf("%d of %zu won, %d capped, %.1fs wall with %d threads\n", won, cands.size(), capped,
                    secondsSince(start_), threads());
    }

private:
    struct Candidate {
        CellSet cells;
        std::string origin;
    };

    int threads() const { return o_.threads > 0 ? o_.threads : std::max(1u, std::thread::hardware_concurrency()); }
    int budget() const { return ch_.arena().settings().budget; }
    const gol::Pattern& shape(int item, int rot, bool mirror) const {
        return shapes_[static_cast<std::size_t>(item) * 8 + rot * 2 + (mirror ? 1 : 0)];
    }

    // Arsenal pieces at random spots and orientations, at most `cells` cells in all.
    CellSet randomArmy(int cells) {
        CellSet s;
        int left = cells;
        for (int tries = 0; tries < 200 && left > 0; ++tries) {
            const int item = items_[pick(rng_, 0, static_cast<int>(items_.size()) - 1)];
            const auto& p = shape(item, pick(rng_, 0, 3), chance(rng_, 0.5));
            const int n = static_cast<int>(p.cells.size());
            if (n > left || p.width > ch_.zoneWidth()) continue;
            ch_.stamp(s, p, pick(rng_, 0, ch_.zoneWidth() - p.width), pick(rng_, 0, ch_.height() - p.height));
            left -= n;
        }
        normalise(s);
        return s;
    }

    // The 8-connected group of cells (gaps of one cell bridged) around s[i].
    std::vector<std::size_t> cluster(const CellSet& s, std::size_t i) const {
        const int zw = ch_.zoneWidth();
        std::vector<std::size_t> group{i};
        std::vector<char> in(s.size(), 0);
        in[i] = 1;
        for (std::size_t k = 0; k < group.size(); ++k) {
            const int a = s[group[k]];
            for (std::size_t j = 0; j < s.size(); ++j) {
                if (in[j]) continue;
                const int b = s[j];
                if (std::abs(a % zw - b % zw) <= 2 && std::abs(a / zw - b / zw) <= 2) {
                    in[j] = 1;
                    group.push_back(j);
                }
            }
        }
        return group;
    }

    bool inZone(int x, int y) const { return x >= 0 && x < ch_.zoneWidth() && y >= 0 && y < ch_.height(); }

    // Moves the given cells by (dx, dy); false if one would leave the zone.
    bool shift(CellSet& s, const std::vector<std::size_t>& which, int dx, int dy) const {
        const int zw = ch_.zoneWidth();
        for (std::size_t i : which) {
            const int x = s[i] % zw + dx, y = s[i] / zw + dy;
            if (!inZone(x, y)) return false;
            s[i] = y * zw + x;
        }
        return true;
    }

    // An empty zone cell within `r` of cell c, or -1.
    int near(const CellSet& s, int c, int r) {
        const int zw = ch_.zoneWidth();
        for (int t = 0; t < 20; ++t) {
            const int x = c % zw + pick(rng_, -r, r), y = c / zw + pick(rng_, -r, r);
            if (inZone(x, y) && !std::binary_search(s.begin(), s.end(), y * zw + x)) return y * zw + x;
        }
        return -1;
    }

    // Parent: from the smallest counts, the smallest one more likely; within a
    // count, the larger margin more likely.
    const Winner& parent() {
        std::vector<const std::vector<Winner>*> levels;
        for (const auto& [n, list] : archive_) levels.push_back(&list);
        std::size_t l = 0;
        while (l + 1 < levels.size() && chance(rng_, 0.4)) ++l;
        const auto& list = *levels[l];
        const int a = pick(rng_, 0, static_cast<int>(list.size()) - 1), b = pick(rng_, 0, static_cast<int>(list.size()) - 1);
        return list[std::min(a, b)];  // the list is sorted by margin, best first
    }

    Candidate mutate() {
        const Winner& w = parent();
        CellSet s = w.cells;
        const int zw = ch_.zoneWidth();
        std::string op;
        const int roll = pick(rng_, 0, 99);
        const std::size_t i = static_cast<std::size_t>(pick(rng_, 0, static_cast<int>(s.size()) - 1));
        if (roll < 35) {  // delete a cell
            s.erase(s.begin() + static_cast<long>(i));
            op = "del";
        } else if (roll < 45) {  // delete a cluster
            auto group = cluster(s, i);
            std::sort(group.rbegin(), group.rend());
            for (std::size_t j : group) s.erase(s.begin() + static_cast<long>(j));
            op = "delgroup";
        } else if (roll < 55) {  // two cells out, one new in, next to an old one
            const int c = near(s, s[i], 2);
            s.erase(s.begin() + static_cast<long>(i));
            if (!s.empty()) s.erase(s.begin() + pick(rng_, 0, static_cast<int>(s.size()) - 1));
            if (c >= 0) s.push_back(c);
            op = "2for1";
        } else if (roll < 70) {  // move a cell nearby
            const int c = near(s, s[i], 2);
            if (c >= 0) s[i] = c;
            op = "move";
        } else if (roll < 82) {  // shift a cluster
            const auto group = cluster(s, i);
            int dx = 0, dy = 0;
            while (dx == 0 && dy == 0) dx = pick(rng_, -3, 3), dy = pick(rng_, -3, 3);
            CellSet t = s;
            if (shift(t, group, dx, dy)) s = t;
            op = "shift";
        } else if (roll < 88) {  // shift everything
            std::vector<std::size_t> all(s.size());
            for (std::size_t j = 0; j < s.size(); ++j) all[j] = j;
            int dx = 0, dy = 0;
            while (dx == 0 && dy == 0) dx = pick(rng_, -2, 2), dy = pick(rng_, -4, 4);
            CellSet t = s;
            if (shift(t, all, dx, dy)) s = t;
            op = "shiftall";
        } else if (roll < 96) {  // a cluster becomes an arsenal piece that is not bigger
            auto group = cluster(s, i);
            const int n = static_cast<int>(group.size());
            int cx = 0, cy = 0;
            for (std::size_t j : group) cx += s[j] % zw, cy += s[j] / zw;
            cx /= n, cy /= n;
            std::vector<int> fits;
            for (int item : items_)
                if (static_cast<int>(gol::findPattern(gol::battleArsenal()[item].patternId)->cells.size()) <= n)
                    fits.push_back(item);
            if (!fits.empty()) {
                std::sort(group.rbegin(), group.rend());
                for (std::size_t j : group) s.erase(s.begin() + static_cast<long>(j));
                const auto& p = shape(fits[pick(rng_, 0, static_cast<int>(fits.size()) - 1)], pick(rng_, 0, 3),
                                      chance(rng_, 0.5));
                ch_.stamp(s, p, cx - p.width / 2, cy - p.height / 2);
            }
            op = "piece";
        } else {  // one cell more, next to an old one
            const int c = near(s, s[i], 1);
            if (c >= 0) s.push_back(c);
            op = "add";
        }
        normalise(s);
        return {s, op};
    }

    void evaluate(const std::vector<Candidate>& cands) {
        std::vector<Outcome> outcomes(cands.size());
        std::vector<char> fresh(cands.size(), 0);
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].cells.empty()) continue;
            const auto [it, inserted] = seen_.emplace(key(cands[i].cells), 0);
            fresh[i] = inserted;
        }
        parallelFor(static_cast<int>(cands.size()), threads(), [&](int i) {
            if (fresh[i]) outcomes[i] = ch_.play(cands[i].cells);
        });
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (!fresh[i]) continue;
            ++battles_;
            capped_ += outcomes[i].capped;
            if (!outcomes[i].won) continue;
            ++wins_;
            add({cands[i].cells, outcomes[i], cands[i].origin});
        }
        if (seen_.size() > 4'000'000) seen_.clear();
    }

    void add(Winner w) {
        const int n = static_cast<int>(w.cells.size());
        if (n > best() + o_.slack) return;
        if (n < best()) sweepPending_ = true, recordAt_ = secondsSince(start_);
        auto& list = archive_[n];
        list.push_back(std::move(w));
        std::stable_sort(list.begin(), list.end(),
                         [](const Winner& a, const Winner& b) { return a.outcome.margin() > b.outcome.margin(); });
        if (static_cast<int>(list.size()) > o_.perCount) list.resize(static_cast<std::size_t>(o_.perCount));
        while (!archive_.empty() && archive_.rbegin()->first > best() + o_.slack) archive_.erase(std::prev(archive_.end()));
        dirty_ = true;
    }

    int archiveSize() const {
        int n = 0;
        for (const auto& [c, list] : archive_) n += static_cast<int>(list.size());
        return n;
    }

    void report(const char* phase, int round, bool final = false) {
        const double t = secondsSince(start_);
        if (!final && t - lastReport_ < 10) return;
        lastReport_ = t;
        std::printf("[%6.0fs] %-6s round %5d  battles %8lld  wins %6lld  capped %lld  best %s", t, phase, round,
                    battles_, wins_, capped_, archive_.empty() ? "-" : std::to_string(best()).c_str());
        if (!archive_.empty()) {
            std::printf("  (");
            for (const auto& [n, list] : archive_) std::printf(" %d:%zu", n, list.size());
            std::printf(" )");
        }
        std::printf("\n");
        std::fflush(stdout);
        if (dirty_ || final) save();
    }

    void save() {
        dirty_ = false;
        if (o_.out.empty() || archive_.empty()) return;
        std::ofstream f(o_.out);
        f << "# life_challenge --level " << o_.level << ": winners against the bot, fewest red cells first\n";
        f << "# battles " << battles_ << ", wins " << wins_ << ", " << secondsSince(start_) / 60 << " min, record found after "
          << recordAt_ / 60 << " min\n";
        for (const auto& [n, list] : archive_) {
            for (const auto& w : list) {
                const auto& o = w.outcome;
                f << "\n# " << n << " cells, " << reasonName(o.reason) << " at generation " << o.generation << ", red "
                  << o.red << " vs blue " << o.blue << " (margin " << o.margin() << "), last step: " << w.origin << "\n";
                f << picture(ch_, w.cells);
                f << ld1Code(ch_, w.cells, "challenge " + o_.level + " " + std::to_string(n) + " cells") << "\n";
                f << piecesText(ch_, w.cells) << "\n";
            }
        }
    }

    const Challenge& ch_;
    const Options& o_;
    Rng rng_;
    std::vector<int> items_;
    std::vector<gol::Pattern> shapes_;
    std::map<int, std::vector<Winner>> archive_;
    std::unordered_map<std::string, char> seen_;
    Clock::time_point start_;
    long long battles_ = 0, wins_ = 0, capped_ = 0;
    double lastReport_ = -1e9, recordAt_ = 0;
    bool sweepPending_ = false, dirty_ = false;
};

// ---------------------------------------------------------------------------
// Exhaustive search over all armies of up to K cells
// ---------------------------------------------------------------------------
// Call two cells linked when they are at most 2 apart in both x and y. A group
// of one or two linked cells that is not linked to any other cell dies in the
// first generation without giving birth to anything (a dead cell next to it
// has at most two live neighbours), and a larger group evolves in the first
// generation as if it were alone. So every army of K cells that is not one
// linked group has the same first generation as a smaller army, and plays the
// same battle. Checking all linked groups of up to K cells at all positions
// therefore checks every army of up to K cells.
//
// That is still millions of battles, but most of them are the same battle:
// Red's zone is quiet until Red reaches the grey cells or Blue arrives, and a
// small group soon dies or settles into a block, a blinker, a beehive...
// Placements whose cells after T generations are the same, and which stayed
// away from everything else until then, play the same battle (the only
// difference is the first T boards, which matter only if the whole board later
// returns to one of them). One battle per such class decides them all.
class Exhaustive {
    using Point = gol::Point;

public:
    Exhaustive(const Challenge& ch, const Options& o) : ch_(ch), o_(o) {
        // How close Red may come during the first T generations without
        // touching anything else: everything else must stay 3 columns away.
        const int w = ch_.arena().settings().width, h = ch_.height();
        gol::Battle quiet(ch_.config(), ch_.board({0, 1, ch_.zoneWidth(), ch_.zoneWidth() + 1}));  // a block in the corner
        int minX = w;
        for (int t = 0; t <= kT; ++t) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (quiet.at(x, y) != gol::Dead && quiet.at(x, y) != gol::Red) minX = std::min(minX, x);
            quiet.step();
        }
        limitX_ = minX - 3;
    }

    void run(int maxCells) {
        const auto start = Clock::now();
        std::vector<Winner> winners;
        long long totalBattles = 0, totalCapped = 0;
        for (int k = 3; k <= maxCells; ++k) {
            const auto shapes = linkedGroups(k);
            std::printf("%d cells: %zu linked groups, classifying %zu placements...\n", k, shapes.size(),
                        shapes.size() * static_cast<std::size_t>(ch_.zoneWidth() * ch_.height()));
            std::fflush(stdout);
            const auto reps = classify(shapes);
            std::printf("%d cells: %zu different battles (%lld placements died out, %lld not isolated)\n", k,
                        reps.size(), died_, loose_);
            std::fflush(stdout);
            std::vector<Outcome> outcomes(reps.size());
            std::atomic<long long> done{0};
            const auto t0 = Clock::now();
            parallelFor(static_cast<int>(reps.size()), threads(), [&](int i) {
                outcomes[i] = ch_.play(reps[i]);
                const long long d = ++done;
                if (d % 20000 == 0) {
                    std::printf("  %lld / %zu battles, %.0fs\n", d, reps.size(), secondsSince(t0));
                    std::fflush(stdout);
                }
            });
            int won = 0, capped = 0;
            for (std::size_t i = 0; i < reps.size(); ++i) {
                capped += outcomes[i].capped;
                if (!outcomes[i].won) continue;
                ++won;
                winners.push_back({reps[i], outcomes[i], "exhaustive"});
            }
            totalBattles += static_cast<long long>(reps.size());
            totalCapped += capped;
            std::printf("%d cells: %d winning, %d capped, %.0fs so far\n", k, won, capped, secondsSince(start));
            std::fflush(stdout);
            save(winners, maxCells, totalBattles, totalCapped);
        }
    }

private:
    static constexpr int kT = 30;  // generations after which placements are compared

    int threads() const { return o_.threads > 0 ? o_.threads : std::max(1u, std::thread::hardware_concurrency()); }

    // Packed coordinates on an unbounded plane.
    static constexpr long long kSpan = 1 << 22, kMid = 1 << 20;
    static long long pack(int x, int y) { return (x + kMid) * kSpan + (y + kMid); }
    static int unpackX(long long c) { return static_cast<int>(c / kSpan - kMid); }
    static int unpackY(long long c) { return static_cast<int>(c % kSpan - kMid); }

    // One generation of plain Life; with `bounded`, only inside the board.
    std::vector<long long> step(const std::vector<long long>& live, bool bounded) const {
        std::vector<long long> around;
        around.reserve(live.size() * 9);
        for (long long c : live)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) around.push_back(c + dx * kSpan + dy);
        std::sort(around.begin(), around.end());
        std::vector<long long> next;
        for (std::size_t i = 0; i < around.size();) {
            std::size_t j = i;
            while (j < around.size() && around[j] == around[i]) ++j;
            const int n = static_cast<int>(j - i);  // live neighbours, plus one if alive itself
            const long long c = around[i];
            if (n == 3 || (n == 4 && std::binary_search(live.begin(), live.end(), c))) {
                const int x = unpackX(c), y = unpackY(c);
                if (!bounded || (x >= 0 && y >= 0 && y < ch_.height())) next.push_back(c);
            }
            i = j;
        }
        return next;
    }

    struct Fate {
        std::vector<long long> cells;      // after kT generations (empty: died out)
        int minX, minY, maxX, maxY;        // over all generations up to kT
    };

    Fate evolve(std::vector<long long> live, bool bounded) const {
        std::sort(live.begin(), live.end());
        Fate f{{}, 1 << 20, 1 << 20, -(1 << 20), -(1 << 20)};
        for (int t = 0;; ++t) {
            for (long long c : live) {
                f.minX = std::min(f.minX, unpackX(c)), f.maxX = std::max(f.maxX, unpackX(c));
                f.minY = std::min(f.minY, unpackY(c)), f.maxY = std::max(f.maxY, unpackY(c));
            }
            if (t == kT || live.empty()) break;
            live = step(live, bounded);
        }
        f.cells = std::move(live);
        return f;
    }

    // All linked groups of k cells, each with its smallest x and y at 0.
    static std::vector<std::vector<Point>> linkedGroups(int k) {
        using Shape = std::vector<std::pair<int, int>>;
        auto norm = [](Shape s) {
            int mx = 1 << 20, my = 1 << 20;
            for (auto [x, y] : s) mx = std::min(mx, x), my = std::min(my, y);
            for (auto& [x, y] : s) x -= mx, y -= my;
            std::sort(s.begin(), s.end());
            return s;
        };
        std::set<Shape> level{Shape{{0, 0}}};
        for (int n = 2; n <= k; ++n) {
            std::set<Shape> next;
            for (const auto& s : level)
                for (auto [x, y] : s)
                    for (int dx = -2; dx <= 2; ++dx)
                        for (int dy = -2; dy <= 2; ++dy) {
                            const std::pair<int, int> c{x + dx, y + dy};
                            if (std::find(s.begin(), s.end(), c) != s.end()) continue;
                            Shape t = s;
                            t.push_back(c);
                            next.insert(norm(t));
                        }
            level = std::move(next);
        }
        std::vector<std::vector<Point>> out;
        for (const auto& s : level) {
            std::vector<Point> cells;
            for (auto [x, y] : s) cells.push_back({x, y});
            out.push_back(cells);
        }
        return out;
    }

    // One army per different battle among all placements of all shapes.
    std::vector<CellSet> classify(const std::vector<std::vector<Point>>& shapes) {
        const int zw = ch_.zoneWidth(), h = ch_.height();
        died_ = loose_ = 0;
        // Fate of every shape on the unbounded plane, its cells relative to the
        // shape's corner; the settled shapes get ids.
        std::map<std::vector<long long>, int> settledIds;
        struct Free {
            Fate fate;
            int settled = -1;  // id of the normalised cells after kT generations
            int dx = 0, dy = 0;  // where they are relative to the shape's corner
        };
        std::vector<Free> free(shapes.size());
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            std::vector<long long> live;
            for (const auto& c : shapes[s]) live.push_back(pack(c.x, c.y));
            free[s].fate = evolve(live, false);
            const auto& cells = free[s].fate.cells;
            if (cells.empty()) continue;
            int mx = 1 << 20, my = 1 << 20;
            for (long long c : cells) mx = std::min(mx, unpackX(c)), my = std::min(my, unpackY(c));
            std::vector<long long> normal;
            for (long long c : cells) normal.push_back(pack(unpackX(c) - mx, unpackY(c) - my));
            free[s].settled = settledIds.emplace(normal, static_cast<int>(settledIds.size())).first->second;
            free[s].dx = mx, free[s].dy = my;
        }

        std::unordered_map<std::uint64_t, CellSet> classes;
        std::vector<CellSet> loose;
        auto seed = [&](std::size_t s, int x, int y) {
            CellSet set;
            for (const auto& c : shapes[s]) set.push_back((y + c.y) * zw + x + c.x);
            normalise(set);
            return set;
        };
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            int sw = 0, sh = 0;
            for (const auto& c : shapes[s]) sw = std::max(sw, c.x + 1), sh = std::max(sh, c.y + 1);
            const Fate& f = free[s].fate;
            for (int y = 0; y + sh <= h; ++y)
                for (int x = 0; x + sw <= zw; ++x) {
                    const bool inside = x + f.minX >= 0 && y + f.minY >= 0 && y + f.maxY < h && x + f.maxX <= limitX_;
                    std::uint64_t key;
                    if (inside) {
                        if (free[s].settled < 0) {
                            ++died_;
                            continue;
                        }
                        key = (static_cast<std::uint64_t>(free[s].settled) << 24) |
                              (static_cast<std::uint64_t>(x + free[s].dx + 2048) << 12) |
                              static_cast<std::uint64_t>(y + free[s].dy + 2048);
                    } else {
                        // Near an edge or the grey cells: follow this placement itself.
                        std::vector<long long> live;
                        for (const auto& c : shapes[s]) live.push_back(pack(x + c.x, y + c.y));
                        const Fate g = evolve(live, true);
                        if (g.maxX > limitX_) {
                            ++loose_;
                            loose.push_back(seed(s, x, y));
                            continue;
                        }
                        if (g.cells.empty()) {
                            ++died_;
                            continue;
                        }
                        key = 0x9e3779b97f4a7c15ull;
                        for (long long c : g.cells) key = (key ^ static_cast<std::uint64_t>(c)) * 0x100000001b3ull;
                        key |= 1ull << 63;
                    }
                    if (classes.find(key) == classes.end()) classes.emplace(key, seed(s, x, y));
                }
        }
        std::vector<CellSet> reps = std::move(loose);
        for (auto& [k, set] : classes) reps.push_back(std::move(set));
        std::sort(reps.begin(), reps.end());
        return reps;
    }

    void save(const std::vector<Winner>& winners, int maxCells, long long battles, long long capped) const {
        if (o_.out.empty()) return;
        std::ofstream f(o_.out);
        f << "# life_challenge --level " << o_.level << " --exhaustive " << maxCells
          << ": every army of up to that many cells, " << battles << " different battles, " << capped
          << " capped (counted as lost)\n";
        f << "# " << winners.size() << " winning\n";
        for (const auto& w : winners) {
            const auto& o = w.outcome;
            f << "\n# " << w.cells.size() << " cells, " << reasonName(o.reason) << " at generation " << o.generation
              << ", red " << o.red << " vs blue " << o.blue << " (margin " << o.margin() << ")\n";
            f << picture(ch_, w.cells);
            f << ld1Code(ch_, w.cells, "challenge " + o_.level + " " + std::to_string(w.cells.size()) + " cells") << "\n";
            f << piecesText(ch_, w.cells) << "\n";
        }
    }

    const Challenge& ch_;
    const Options& o_;
    int limitX_ = 0;
    long long died_ = 0, loose_ = 0;
};

// ---------------------------------------------------------------------------
// An army against a trained league
// ---------------------------------------------------------------------------
// Plays `red` against every army of a life_arena league on the level's board
// and weights the results by the league's Nash mixture (from FILE.json), once
// under the league's own rules (its generation limit, all cells counted) and
// once under the Challenge rules (no limit, home cells not counted).
int vsLeague(const Challenge& ch, const CellSet& red, const std::string& file, int threads) {
    std::ifstream in(file), json(file + ".json");
    if (!in || !json) {
        std::fprintf(stderr, "need %s and %s.json\n", file.c_str(), file.c_str());
        return 1;
    }
    const Arena& arena = ch.arena();
    std::vector<Army> armies;
    int maxgen = 2000;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("maxgen ", 0) == 0) maxgen = std::atoi(line.c_str() + 7);
        if (line.rfind("army\t", 0) != 0) continue;
        Army a;
        if (!parsePieces(arena, line.substr(line.rfind('\t') + 1), a)) {
            std::fprintf(stderr, "cannot parse: %s\n", line.c_str());
            return 1;
        }
        armies.push_back(a);
    }
    std::vector<double> weights;
    const std::string text((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>());
    for (std::size_t at = text.find("\"weight\":"); at != std::string::npos; at = text.find("\"weight\":", at + 1))
        weights.push_back(std::atof(text.c_str() + at + 9));
    if (weights.size() != armies.size()) {
        std::fprintf(stderr, "%zu armies but %zu weights\n", armies.size(), weights.size());
        return 1;
    }

    Army cells;
    const int cell = arena.findItem("cell");
    for (int c : red) cells.pieces.push_back({cell, c % ch.zoneWidth(), c / ch.zoneWidth(), 0, false});
    Settings leagueRules = arena.settings();
    leagueRules.maxGenerations = maxgen;
    const Arena league(leagueRules);

    struct Row { MatchResult rules; Outcome challenge; };
    std::vector<Row> rows(armies.size());
    parallelFor(static_cast<int>(armies.size()), threads, [&](int i) {
        rows[i].rules = league.play(cells, armies[i]);
        gol::FastBattle battle(ch.config(), arena.board(cells, armies[i]));
        while (!battle.finished() && battle.generation() < 30000) battle.step();
        Outcome& o = rows[i].challenge;
        o.capped = !battle.finished();
        o.won = battle.winner() == gol::Winner::Red;
        o.reason = battle.endReason();
        o.generation = battle.generation();
        o.red = battle.redScore();
        o.blue = battle.blueScore();
    });

    double wWin = 0, wScore = 0, wChallenge = 0, wSum = 0;
    int wins = 0, challengeWins = 0;
    std::printf("%zu cells against the %zu armies of %s\n", red.size(), armies.size(), file.c_str());
    std::printf("  army  Nash wt | league rules (limit %d, all cells)   | Challenge rules (no limit, home not counted)\n", maxgen);
    for (std::size_t i = 0; i < armies.size(); ++i) {
        const auto& r = rows[i].rules;
        const auto& c = rows[i].challenge;
        const bool won = r.winner == gol::Winner::Red;
        wins += won, challengeWins += c.won;
        wWin += weights[i] * won, wScore += weights[i] * r.score, wChallenge += weights[i] * c.won, wSum += weights[i];
        if (weights[i] > 0)
            std::printf("  #%-4zu %6.1f%% | %-4s red %6.1f blue %6.1f %+.3f | %-4s red %6.1f blue %6.1f (%s, gen %d)\n", i,
                        100 * weights[i], won ? "WIN" : "loss", r.redScore, r.blueScore, r.score,
                        c.won ? "WIN" : "loss", c.red, c.blue, reasonName(c.reason), c.generation);
    }
    std::printf("league rules:    wins %d of %zu armies, %.1f%% of the Nash mixture, payoff vs mixture %+.3f\n", wins,
                armies.size(), 100 * wWin / wSum, wScore / wSum);
    std::printf("Challenge rules: wins %d of %zu armies, %.1f%% of the Nash mixture\n", challengeWins, armies.size(),
                100 * wChallenge / wSum);
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: life_challenge [options]\n"
                 "  --level hive|empty|champion   level whose bot to beat (default hive)\n"
                 "  --minutes M      search time (default 10)\n"
                 "  --threads N      worker threads (default: all cores)\n"
                 "  --rng S          random seed (default 1)\n"
                 "  --cap G          safety cap: battles still running after G generations count as lost (30000)\n"
                 "  --out FILE       winners file (default data/challenge/<level>.txt)\n"
                 "  --start ARMY     start from this army (life_arena form) instead of random seeding; repeatable\n"
                 "  --per-count K    archive: armies kept per cell count (24)\n"
                 "  --slack S        archive: cell counts kept above the smallest (4)\n"
                 "  --verify ARMY    play one army (life_arena form) with both engines and print the result\n"
                 "  --exhaustive K   instead: play every army of up to K cells (default out: data/challenge/<level>-exhaustive.txt)\n"
                 "  --vs-league FILE with --verify: play the army against a life_arena league, weighted by its Nash mixture\n"
                 "  --bench          time a few random armies and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string verify;
    int exhaustive = 0;
    std::string league;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--level") o.level = value();
        else if (a == "--minutes") o.minutes = std::atof(value().c_str());
        else if (a == "--threads") o.threads = std::atoi(value().c_str());
        else if (a == "--rng") o.rng = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--cap") o.cap = std::max(1, std::atoi(value().c_str()));
        else if (a == "--out") o.out = value();
        else if (a == "--start") o.start.push_back(value());
        else if (a == "--per-count") o.perCount = std::max(1, std::atoi(value().c_str()));
        else if (a == "--slack") o.slack = std::max(0, std::atoi(value().c_str()));
        else if (a == "--verify") verify = value();
        else if (a == "--bench") o.bench = true;
        else if (a == "--vs-league") league = value();
        else if (a == "--exhaustive") exhaustive = std::max(1, std::atoi(value().c_str()));
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    const Level* level = nullptr;
    for (const auto& l : kLevels)
        if (o.level == l.id) level = &l;
    if (!level) {
        std::fprintf(stderr, "unknown level %s\n", o.level.c_str());
        return 2;
    }
    if (o.out.empty()) o.out = "data/challenge/" + o.level + (exhaustive ? "-exhaustive" : "") + ".txt";

    Challenge ch(*level, o.cap);
    if (!verify.empty()) {
        CellSet s;
        if (!parseCells(ch, verify, s)) {
            std::fprintf(stderr, "cannot parse the army\n");
            return 2;
        }
        if (!league.empty())
            return vsLeague(ch, s, league, o.threads > 0 ? o.threads : std::max(1u, std::thread::hardware_concurrency()));
        for (int engine = 0; engine < 2; ++engine) {
            const Outcome r = engine == 0 ? ch.play(s) : ch.verify(s);
            std::printf("%-9s %zu cells: %s, %s at generation %d, red %.2f vs blue %.2f%s\n",
                        engine == 0 ? "fast" : "reference", s.size(), r.won ? "Red wins" : "Red does not win",
                        reasonName(r.reason), r.generation, r.red, r.blue, r.capped ? " (capped)" : "");
        }
        std::printf("%s", picture(ch, s).c_str());
        return 0;
    }
    if (exhaustive) {
        Exhaustive(ch, o).run(exhaustive);
        return 0;
    }
    Search search(ch, o);
    if (o.bench) {
        search.bench();
        return 0;
    }
    search.run();
    return 0;
}
