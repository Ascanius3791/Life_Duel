// life_arena: trains Life Duel bots by self-play on one fixed battlefield.
//
// Training keeps a league of armies and the payoff of every pair. Each
// iteration computes the Nash mixture of the league (the best way to pick
// randomly among its armies), then runs an evolutionary search for an army
// that beats that mixture. If one is found it joins the league. The score the
// search reaches against the mixture ("gain") measures how exploitable the
// league still is; it shrinks as the league gets stronger.
#include "arena.hpp"
#include "league.hpp"

#include "gol/battle.hpp"
#include "gol/fast_battle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace arena;

namespace {

using Clock = std::chrono::steady_clock;
using Matrix = std::vector<std::vector<double>>;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Options {
    std::string command;
    std::string league = "league.txt";
    Settings settings;
    bool settingsGiven = false;
    int iterations = 20;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int population = 24;
    int generations = 20;
    int support = 6;
    int seeds = 8;
    double minGain = 0.02;
    std::uint64_t rng = 1;
    std::vector<int> ids;  // positional numbers for show / match
    bool showBoard = false;
    int battles = 20;
};

void usage() {
    std::puts(
        "life_arena - train Life Duel bots by self-play\n"
        "\n"
        "  life_arena train [options]       grow the league (creates it if missing, resumes otherwise)\n"
        "  life_arena show [options] [ID]   list the league with Nash weights; with ID draw that army\n"
        "  life_arena match [options] A B   play army A (red) against army B (blue)\n"
        "  life_arena bench [options]       compare the reference and the bit-parallel engine\n"
        "\n"
        "League:\n"
        "  --league FILE      league file (default league.txt; FILE.json is written next to it)\n"
        "\n"
        "Battle settings (only when a new league is created):\n"
        "  --size WxH         board size (default 260x128)\n"
        "  --preset ID        battlefield preset (default random_mirror)\n"
        "  --count N          grey cells for random presets (default 900)\n"
        "  --field-seed N     battlefield seed (default 1)\n"
        "  --budget N         points per player (default 120)\n"
        "  --maxgen N         generation limit, 0 = none (default 2000)\n"
        "  --variant V        birth | contagion (default birth)\n"
        "\n"
        "Training:\n"
        "  --iterations N     league iterations (default 20)\n"
        "  --pop N            population of the search (default 24)\n"
        "  --gens N           generations of the search per iteration (default 20)\n"
        "  --support N        strongest mixture members to train against (default 6)\n"
        "  --seeds N          random armies in a new league (default 8)\n"
        "  --min-gain X       minimum score vs the mixture to join the league (default 0.02)\n"
        "  --threads N        worker threads (default: all cores)\n"
        "  --rng N            random seed of the search (default 1)\n"
        "\n"
        "Other:\n"
        "  --board            match: print the starting board in the browser's run-length format\n"
        "  --battles N        bench: number of battles (default 20)");
}

bool parseArgs(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        auto integer = [&]() { return std::atoi(value().c_str()); };
        Settings& s = o.settings;
        if (a == "--league") o.league = value();
        else if (a == "--size") {
            const std::string v = value();
            if (std::sscanf(v.c_str(), "%dx%d", &s.width, &s.height) != 2 || s.width < 20 || s.height < 10) {
                std::fprintf(stderr, "bad --size %s\n", v.c_str());
                return false;
            }
            o.settingsGiven = true;
        } else if (a == "--preset") s.preset = value(), o.settingsGiven = true;
        else if (a == "--count") s.count = integer(), o.settingsGiven = true;
        else if (a == "--field-seed") s.fieldSeed = std::strtoull(value().c_str(), nullptr, 10), o.settingsGiven = true;
        else if (a == "--budget") s.budget = std::max(1, integer()), o.settingsGiven = true;
        else if (a == "--maxgen") s.maxGenerations = std::max(0, integer()), o.settingsGiven = true;
        else if (a == "--variant") {
            if (!parseVariant(value(), s.variant)) {
                std::fprintf(stderr, "--variant must be birth or contagion\n");
                return false;
            }
            o.settingsGiven = true;
        } else if (a == "--iterations") o.iterations = std::max(0, integer());
        else if (a == "--pop") o.population = std::max(4, integer());
        else if (a == "--gens") o.generations = std::max(1, integer());
        else if (a == "--support") o.support = std::max(1, integer());
        else if (a == "--seeds") o.seeds = std::max(2, integer());
        else if (a == "--min-gain") o.minGain = std::atof(value().c_str());
        else if (a == "--threads") o.threads = std::max(1, integer());
        else if (a == "--rng") o.rng = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--board") o.showBoard = true;
        else if (a == "--battles") o.battles = std::max(1, integer());
        else if (a == "-h" || a == "--help") return false;
        else if (!a.empty() && a[0] != '-') o.ids.push_back(std::atoi(a.c_str()));
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    bool presetKnown = false;
    for (const auto& p : gol::battlefieldPresets()) presetKnown |= p.id == o.settings.preset;
    if (!presetKnown) {
        std::fprintf(stderr, "unknown preset %s; known:", o.settings.preset.c_str());
        for (const auto& p : gol::battlefieldPresets()) std::fprintf(stderr, " %s", p.id.c_str());
        std::fputc('\n', stderr);
        return false;
    }
    return true;
}

// Loads the league named in the options. Returns false if it does not exist or is broken.
bool loadLeague(const Options& o, Settings& settings, std::vector<std::pair<std::string, std::string>>& raw,
                bool quietIfMissing) {
    LeagueFile file;
    std::string error;
    if (!readLeague(o.league, file, error)) {
        if (!quietIfMissing) std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    settings = file.settings;
    raw = std::move(file.armies);
    return true;
}

std::vector<Army> parseArmies(const Arena& arena, const std::vector<std::pair<std::string, std::string>>& raw) {
    std::vector<Army> armies;
    for (const auto& [origin, text] : raw) {
        Army a;
        a.origin = origin;
        if (!parsePieces(arena, text, a) || !arena.legal(a)) {
            std::fprintf(stderr, "skipping invalid army '%s'\n", origin.c_str());
            continue;
        }
        armies.push_back(std::move(a));
    }
    return armies;
}

// Extends the payoff matrix to all armies; entries already computed are kept.
void extendPayoff(const Arena& arena, const std::vector<Army>& armies, Matrix& m, int threads) {
    const std::size_t n = armies.size(), old = m.size();
    m.resize(n);
    for (auto& row : m) row.resize(n, 0);
    std::vector<std::pair<int, int>> jobs;
    for (std::size_t i = old; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j) jobs.emplace_back(static_cast<int>(i), static_cast<int>(j));
    parallelFor(static_cast<int>(jobs.size()), threads, [&](int k) {
        const auto [i, j] = jobs[k];
        const double v = arena.payoff(armies[i], armies[j]);
        m[i][j] = v;
        m[j][i] = -v;  // zero-sum and symmetric: the swapped battle is the mirror image
    });
    for (std::size_t i = old; i < n; ++i) m[i][i] = 0;
}

// "2x gosper_gun, block, 3x glider"
std::string summary(const Arena& arena, const Army& army) {
    std::map<std::string, int> counts;
    for (const auto& p : army.pieces) ++counts[arena.itemId(p.item)];
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [id, n] : counts) sorted.emplace_back(-arena.cost(arena.findItem(id)) * n, id);
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (const auto& [neg, id] : sorted) {
        if (!out.empty()) out += ", ";
        if (counts[id] > 1) out += std::to_string(counts[id]) + "x ";
        out += id;
    }
    return out.empty() ? "(empty)" : out;
}

bool save(const Options& o, const Arena& arena, const std::vector<Army>& armies, const std::vector<double>& meta,
          const Matrix& m) {
    std::string error;
    if (!writeLeague(o.league, arena, armies, error) ||
        !writeLeagueJson(o.league + ".json", arena, armies, meta, m, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

double expected(const Matrix& m, int i, const std::vector<double>& meta) {
    double v = 0;
    for (std::size_t j = 0; j < meta.size() && j < m[i].size(); ++j) v += m[i][j] * meta[j];
    return v;
}

// ---------------------------------------------------------------------------
// train
// ---------------------------------------------------------------------------
struct Individual {
    Army army;
    double fitness = 0;
    bool evaluated = false;
};

int train(const Options& o) {
    Settings settings = o.settings;
    std::vector<std::pair<std::string, std::string>> raw;
    const bool resumed = loadLeague(o, settings, raw, true);
    if (resumed && o.settingsGiven)
        std::printf("note: %s exists, using its battle settings and ignoring the ones given\n", o.league.c_str());

    const Arena arena(settings);
    Rng rng(o.rng);
    std::vector<Army> armies = parseArmies(arena, raw);
    if (!resumed) {
        for (int i = 0; i < o.seeds; ++i) armies.push_back(arena.randomArmy(rng));
        std::printf("new league %s with %d random armies\n", o.league.c_str(), o.seeds);
    } else {
        rng.seed(o.rng ^ (armies.size() * 0x9e3779b97f4a7c15ull));
        std::printf("resuming %s with %zu armies\n", o.league.c_str(), armies.size());
    }
    if (armies.empty()) armies.push_back(arena.randomArmy(rng));

    const char* symmetry = arena.symmetry() == Arena::Symmetry::Mirror     ? "mirror"
                           : arena.symmetry() == Arena::Symmetry::Rotation ? "rotation"
                                                                           : "none (both sides are played)";
    std::printf("board %dx%d, preset %s (%d grey cells, seed %llu), budget %d, maxgen %d, %s, symmetry %s, %d threads\n",
                settings.width, settings.height, settings.preset.c_str(), settings.count,
                static_cast<unsigned long long>(settings.fieldSeed), settings.budget, settings.maxGenerations,
                variantName(settings.variant), symmetry, o.threads);

    auto t0 = Clock::now();
    Matrix m;
    extendPayoff(arena, armies, m, o.threads);
    std::printf("payoff matrix for %zu armies in %.1fs\n", armies.size(), secondsSince(t0));
    std::vector<double> meta = solveNash(m);
    save(o, arena, armies, meta, m);

    for (int iter = 1; iter <= o.iterations; ++iter) {
        const auto start = Clock::now();
        meta = solveNash(m);

        // Opponents: the strongest members of the mixture, re-weighted.
        std::vector<int> support;
        for (std::size_t i = 0; i < meta.size(); ++i)
            if (meta[i] > 0) support.push_back(static_cast<int>(i));
        std::sort(support.begin(), support.end(), [&](int a, int b) { return meta[a] > meta[b]; });
        if (static_cast<int>(support.size()) > o.support) support.resize(o.support);
        std::vector<double> weight;
        double total = 0;
        for (int s : support) total += meta[s];
        for (int s : support) weight.push_back(meta[s] / total);

        // Starting population: the mixture's armies, mutations of them, crossovers and fresh random armies.
        std::vector<Individual> pop;
        auto anyLeague = [&]() -> const Army& {
            return armies[std::uniform_int_distribution<int>(0, static_cast<int>(armies.size()) - 1)(rng)];
        };
        auto anySupport = [&]() -> const Army& {
            return armies[support[std::uniform_int_distribution<int>(0, static_cast<int>(support.size()) - 1)(rng)]];
        };
        for (int s : support)
            if (static_cast<int>(pop.size()) < o.population / 4) pop.push_back({armies[s]});
        while (static_cast<int>(pop.size()) < o.population) {
            const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
            Army a = kind < 5   ? arena.mutate(anySupport(), rng)
                     : kind < 7 ? arena.mutate(arena.crossover(anySupport(), anyLeague(), rng), rng)
                                : arena.randomArmy(rng);
            pop.push_back({std::move(a)});
        }

        for (int gen = 0; gen < o.generations; ++gen) {
            // Evaluate everything new against every opponent, all in parallel.
            std::vector<int> todo;
            for (std::size_t k = 0; k < pop.size(); ++k)
                if (!pop[k].evaluated) todo.push_back(static_cast<int>(k));
            std::vector<double> scores(todo.size() * support.size());
            parallelFor(static_cast<int>(scores.size()), o.threads, [&](int job) {
                const int k = todo[job / support.size()];
                const int s = support[job % support.size()];
                scores[job] = arena.payoff(pop[k].army, armies[s]);
            });
            for (std::size_t t = 0; t < todo.size(); ++t) {
                double f = 0;
                for (std::size_t s = 0; s < support.size(); ++s) f += weight[s] * scores[t * support.size() + s];
                pop[todo[t]].fitness = f;
                pop[todo[t]].evaluated = true;
            }
            std::stable_sort(pop.begin(), pop.end(),
                             [](const Individual& a, const Individual& b) { return a.fitness > b.fitness; });
            std::printf("\r  iter %d  search gen %2d/%d  best %+.3f   ", iter, gen + 1, o.generations,
                        pop[0].fitness);
            std::fflush(stdout);
            if (gen + 1 == o.generations) break;

            // Next generation: keep the best quarter, breed the rest by tournament selection.
            const int elite = std::max(1, o.population / 4);
            auto tournament = [&]() -> const Army& {
                int best = std::uniform_int_distribution<int>(0, static_cast<int>(pop.size()) - 1)(rng);
                for (int r = 0; r < 2; ++r)
                    best = std::min(best, std::uniform_int_distribution<int>(0, static_cast<int>(pop.size()) - 1)(rng));
                return pop[best].army;  // sorted, so the lower index is fitter
            };
            std::vector<Individual> next(pop.begin(), pop.begin() + elite);
            while (static_cast<int>(next.size()) < o.population) {
                const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
                Army child = kind < 3   ? arena.mutate(arena.crossover(tournament(), tournament(), rng), rng)
                             : kind < 9 ? arena.mutate(tournament(), rng)
                                        : arena.randomArmy(rng);
                next.push_back({std::move(child)});
            }
            pop = std::move(next);
        }
        std::printf("\r%60s\r", "");

        const Individual& best = pop[0];
        std::string supportText;
        for (std::size_t s = 0; s < support.size(); ++s) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%s#%d %.2f", s ? ", " : "", support[s], weight[s]);
            supportText += buf;
        }
        std::printf("iter %3d  league %3zu  gain %+.3f  ", iter, armies.size(), best.fitness);
        if (best.fitness > o.minGain) {
            Army added = best.army;
            int found = 1;
            for (const auto& a : armies) found += a.origin.rfind("found", 0) == 0;
            added.origin = "found " + std::to_string(found);
            armies.push_back(std::move(added));
            extendPayoff(arena, armies, m, o.threads);
            // The search only saw the strongest members; this is the score against the whole mixture.
            const double full = expected(m, static_cast<int>(armies.size()) - 1, meta);
            meta = solveNash(m);
            std::printf("(full mix %+.3f)  added #%zu (%s)", full, armies.size() - 1,
                        summary(arena, armies.back()).c_str());
        } else {
            std::printf("no army beats the mixture");
        }
        std::printf("  [vs %s]  %.1fs\n", supportText.c_str(), secondsSince(start));
        save(o, arena, armies, meta, m);
    }

    std::printf("\nNash mixture after %zu armies:\n", armies.size());
    for (std::size_t i = 0; i < armies.size(); ++i)
        if (meta[i] > 0)
            std::printf("  #%-3zu %5.1f%%  cost %3d  %s\n", i, 100 * meta[i], arena.cost(armies[i]),
                        summary(arena, armies[i]).c_str());
    std::printf("saved %s and %s.json (total %.1fs)\n", o.league.c_str(), o.league.c_str(), secondsSince(t0));
    return 0;
}

// ---------------------------------------------------------------------------
// show / match / bench
// ---------------------------------------------------------------------------
int show(const Options& o) {
    Settings settings;
    std::vector<std::pair<std::string, std::string>> raw;
    if (!loadLeague(o, settings, raw, false)) return 1;
    const Arena arena(settings);
    const auto armies = parseArmies(arena, raw);

    if (!o.ids.empty()) {
        const int id = o.ids[0];
        if (id < 0 || id >= static_cast<int>(armies.size())) {
            std::fprintf(stderr, "no army #%d\n", id);
            return 1;
        }
        // The red zone, two board rows per text line.
        const auto cells = arena.board(armies[id], Army{});
        const int w = settings.width, zw = arena.zones().leftEnd;
        auto red = [&](int x, int y) {
            return y < settings.height && cells[static_cast<std::size_t>(y) * w + x] == gol::Red;
        };
        std::printf("army #%d (%s), cost %d: %s\n%s\n", id, armies[id].origin.c_str(), arena.cost(armies[id]),
                    summary(arena, armies[id]).c_str(), formatPieces(arena, armies[id]).c_str());
        std::printf("+%s+  <- red zone, the battlefield is to the right\n", std::string(zw, '-').c_str());
        for (int y = 0; y < settings.height; y += 2) {
            std::string line;
            for (int x = 0; x < zw; ++x) {
                const bool top = red(x, y), bottom = red(x, y + 1);
                line += top && bottom ? "█" : top ? "▀" : bottom ? "▄" : " ";
            }
            std::printf("|%s|\n", line.c_str());
        }
        std::printf("+%s+\n", std::string(zw, '-').c_str());
        return 0;
    }

    Matrix m;
    extendPayoff(arena, armies, m, o.threads);
    const auto meta = solveNash(m);
    std::printf("%zu armies, board %dx%d, preset %s, budget %d, maxgen %d, %s\n\n", armies.size(), settings.width,
                settings.height, settings.preset.c_str(), settings.budget, settings.maxGenerations,
                variantName(settings.variant));
    std::printf("  id  weight  vs mix  cost  origin     pieces\n");
    for (std::size_t i = 0; i < armies.size(); ++i)
        std::printf("  %2zu  %5.1f%%  %+.3f  %4d  %-9s  %s\n", i, 100 * meta[i], expected(m, static_cast<int>(i), meta),
                    arena.cost(armies[i]), armies[i].origin.c_str(), summary(arena, armies[i]).c_str());
    return 0;
}

int match(const Options& o) {
    Settings settings;
    std::vector<std::pair<std::string, std::string>> raw;
    if (!loadLeague(o, settings, raw, false)) return 1;
    const Arena arena(settings);
    const auto armies = parseArmies(arena, raw);
    if (o.ids.size() != 2) {
        std::fprintf(stderr, "match needs two army ids\n");
        return 2;
    }
    for (int id : o.ids)
        if (id < 0 || id >= static_cast<int>(armies.size())) {
            std::fprintf(stderr, "no army #%d\n", id);
            return 1;
        }
    const Army& red = armies[o.ids[0]];
    const Army& blue = armies[o.ids[1]];
    const auto r = arena.play(red, blue);
    std::printf("red #%d vs blue #%d: %s by %s at generation %d (red %.1f, blue %.1f, score %+.3f)\n", o.ids[0],
                o.ids[1], gol::toString(r.winner), gol::toString(r.reason), r.generation, r.redScore, r.blueScore,
                r.score);
    if (o.showBoard) std::printf("%s\n", gol::encodeBoard(arena.board(red, blue)).c_str());
    return 0;
}

int bench(const Options& o) {
    const Arena arena(o.settings);
    Rng rng(o.rng);
    std::vector<std::vector<std::uint8_t>> boards;
    for (int i = 0; i < o.battles; ++i) boards.push_back(arena.board(arena.randomArmy(rng), arena.randomArmy(rng)));
    const gol::BattleConfig cfg{o.settings.width, o.settings.height, o.settings.variant, o.settings.maxGenerations};

    struct Outcome {
        int generation;
        gol::Winner winner;
        double red, blue;
        bool operator!=(const Outcome& o) const {
            return generation != o.generation || winner != o.winner || red != o.red || blue != o.blue;
        }
    };
    std::vector<Outcome> reference, fastOutcomes;
    long long slowGens = 0, gens = 0;
    auto t = Clock::now();
    for (const auto& b : boards) {
        gol::Battle battle(cfg, b);
        battle.runToEnd();
        slowGens += battle.generation();
        reference.push_back({battle.generation(), battle.winner(), battle.redScore(), battle.blueScore()});
    }
    const double slow = secondsSince(t);

    t = Clock::now();
    for (const auto& b : boards) {
        gol::FastBattle battle(cfg, b);
        battle.runToEnd();
        gens += battle.generation();
        fastOutcomes.push_back({battle.generation(), battle.winner(), battle.redScore(), battle.blueScore()});
    }
    const double fast = secondsSince(t);
    int differ = 0;
    for (std::size_t i = 0; i < boards.size(); ++i) differ += reference[i] != fastOutcomes[i];

    std::printf("%d battles on %dx%d, %lld generations in total\n", o.battles, o.settings.width, o.settings.height,
                gens);
    std::printf("  reference engine:    %7.1f ms/battle  %8.0f gen/s\n", 1000 * slow / o.battles, slowGens / slow);
    std::printf("  bit-parallel engine: %7.1f ms/battle  %8.0f gen/s  (%.1fx faster)\n", 1000 * fast / o.battles,
                gens / fast, slow / fast);
    if (differ) std::printf("  WARNING: the engines disagree on %d battles\n", differ);
    return differ ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) {
        usage();
        return 2;
    }
    if (o.command == "train") return train(o);
    if (o.command == "show") return show(o);
    if (o.command == "match") return match(o);
    if (o.command == "bench") return bench(o);
    usage();
    return 2;
}
