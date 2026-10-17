// life_arena: trains Life Duel bots by self-play on one fixed battlefield.
//
// Training keeps a league of armies and the payoff of every pair. Each
// iteration computes the Nash mixture of the league (the best way to pick
// randomly among its armies), then runs an evolutionary search for an army
// that beats that mixture. If one is found it joins the league. The score the
// search reaches against the mixture ("gain") measures how exploitable the
// league still is; it shrinks as the league gets stronger.
//
// With --twins two leagues are trained side by side with different search
// seeds. Every few iterations each league's mixture is attacked with the other
// league's armies (cross-evaluation); training stops when neither can exploit
// the other, when that stops improving, or when the budget is spent.
#include "arena.hpp"
#include "league.hpp"
#include "match_log.hpp"
#include "training.hpp"

#include "gol/battle.hpp"
#include "gol/fast_battle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace arena;

namespace {

using Clock = std::chrono::steady_clock;

// Twin B searches with --rng + this.
constexpr std::uint64_t kTwinSeedOffset = 1000;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Options {
    std::string command;
    std::string league = "league.txt";
    bool leagueGiven = false;
    Settings settings;
    bool settingsGiven = false;
    int iterations = 20;
    bool iterationsGiven = false;
    bool twins = false;
    int evalEvery = 5;
    StopRules rules;
    double maxHours = 0;
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
    std::string logMatches;
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
        "Twin training (train --twins):\n"
        "  --twins            train two leagues A and B with the same settings and search seeds --rng\n"
        "                     and --rng + 1000 on one thread pool. FILE becomes the union of both with the Nash\n"
        "                     mixture over the union (rewritten at every evaluation); the twins, their\n"
        "                     state and the evaluation curve eval.csv live in FILE.twins/ (resumable)\n"
        "  --eval-every K     cross-evaluate every K iterations (default 5): xA = best score of any\n"
        "                     army of B against A's mixture, xB the other way round\n"
        "  --eps X            stop when xA and xB are both below X (default 0.05)\n"
        "  --delta X          stop when max(xA, xB), averaged over the last 3 evaluations, has not\n"
        "  --patience N       dropped by X (default 0.02) for N evaluations in a row (default 6)\n"
        "  --iterations N     with --twins: the maximum for this run (default 1000)\n"
        "  --max-hours H      with --twins: stop after H hours of this run (default: no limit)\n"
        "\n"
        "Training data:\n"
        "  --log-matches P    train: record every battle in P.armies.tsv and P.matches.csv (appends)\n"
        "  life_arena raster --league FILE --log-matches P\n"
        "                     write the logged armies' red-zone bitmaps to P.raster.bin (+ P.raster.json)\n"
        "\n"
        "Other:\n"
        "  --board            match: print the starting board in the browser's run-length format\n"
        "  --battles N        bench: number of battles (default 20); with --league the battles are\n"
        "                     played between the league's armies (their settings), otherwise\n"
        "                     between random armies");
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
        if (a == "--league") o.league = value(), o.leagueGiven = true;
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
        } else if (a == "--iterations") o.iterations = std::max(0, integer()), o.iterationsGiven = true;
        else if (a == "--twins") o.twins = true;
        else if (a == "--eval-every") o.evalEvery = std::max(1, integer());
        else if (a == "--eps") o.rules.eps = std::atof(value().c_str());
        else if (a == "--delta") o.rules.delta = std::atof(value().c_str());
        else if (a == "--patience") o.rules.patience = std::max(1, integer());
        else if (a == "--max-hours") o.maxHours = std::max(0.0, std::atof(value().c_str()));
        else if (a == "--pop") o.population = std::max(4, integer());
        else if (a == "--gens") o.generations = std::max(1, integer());
        else if (a == "--support") o.support = std::max(1, integer());
        else if (a == "--seeds") o.seeds = std::max(2, integer());
        else if (a == "--min-gain") o.minGain = std::atof(value().c_str());
        else if (a == "--threads") o.threads = std::max(1, integer());
        else if (a == "--rng") o.rng = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--board") o.showBoard = true;
        else if (a == "--log-matches") o.logMatches = value();
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

// Payoff of every pair of armies.
Matrix payoffMatrix(const Arena& arena, const std::vector<Army>& armies, int threads) {
    const std::size_t n = armies.size();
    Matrix m(n, std::vector<double>(n, 0));
    std::vector<MatchJob> jobs;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j) jobs.push_back({&armies[i], &armies[j], &m[i][j], &m[j][i]});
    playJobs(arena, jobs, threads);
    return m;
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

bool save(const std::string& path, const Arena& arena, const std::vector<Army>& armies,
          const std::vector<double>& meta, const Matrix& m, const std::string& extraJson = "") {
    std::string error;
    if (!writeLeague(path, arena, armies, error) ||
        !writeLeagueJson(path + ".json", arena, armies, meta, m, error, extraJson)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

void printSettings(const Arena& arena, int threads) {
    const Settings& settings = arena.settings();
    const char* symmetry = arena.symmetry() == Arena::Symmetry::Mirror     ? "mirror"
                           : arena.symmetry() == Arena::Symmetry::Rotation ? "rotation"
                                                                           : "none (both sides are played)";
    std::printf("board %dx%d, preset %s (%d grey cells, seed %llu), budget %d, maxgen %d, %s, symmetry %s, %d threads\n",
                settings.width, settings.height, settings.preset.c_str(), settings.count,
                static_cast<unsigned long long>(settings.fieldSeed), settings.budget, settings.maxGenerations,
                variantName(settings.variant), symmetry, threads);
}

SearchOptions searchOptions(const Options& o) {
    SearchOptions s;
    s.population = o.population;
    s.generations = o.generations;
    s.support = o.support;
    s.minGain = o.minGain;
    return s;
}

std::string supportText(const IterationResult& r) {
    std::string text;
    for (std::size_t s = 0; s < r.support.size(); ++s) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s#%d %.2f", s ? ", " : "", r.support[s], r.weight[s]);
        text += buf;
    }
    return text;
}

void printMixture(const Arena& arena, const std::vector<Army>& armies, const std::vector<double>& meta) {
    std::printf("\nNash mixture after %zu armies:\n", armies.size());
    for (std::size_t i = 0; i < armies.size(); ++i)
        if (meta[i] > 0)
            std::printf("  #%-3zu %5.1f%%  cost %3d  %-12s  %s\n", i, 100 * meta[i], arena.cost(armies[i]),
                        armies[i].origin.c_str(), summary(arena, armies[i]).c_str());
}

// ---------------------------------------------------------------------------
// train
// ---------------------------------------------------------------------------
// The battle log of one training run; playJobs stops using it when it goes away.
struct RunLog {
    MatchLog log;
    ~RunLog() { setMatchLog(nullptr); }
};

// Starts recording battles if --log-matches was given.
bool openMatchLog(const Options& o, const Arena& arena, RunLog& run) {
    MatchLog& log = run.log;
    if (o.logMatches.empty()) return true;
    if (!log.open(arena, o.logMatches)) {
        std::fprintf(stderr, "cannot write %s.armies.tsv / .matches.csv\n", o.logMatches.c_str());
        return false;
    }
    setMatchLog(&log);
    std::printf("recording battles in %s.matches.csv\n", o.logMatches.c_str());
    return true;
}

int train(const Options& o) {
    Settings settings = o.settings;
    std::vector<std::pair<std::string, std::string>> raw;
    const bool resumed = loadLeague(o, settings, raw, true);
    if (resumed && o.settingsGiven)
        std::printf("note: %s exists, using its battle settings and ignoring the ones given\n", o.league.c_str());

    const Arena arena(settings);
    std::vector<Army> parsed = parseArmies(arena, raw);
    if (!resumed) std::printf("new league %s with %d random armies\n", o.league.c_str(), o.seeds);
    else std::printf("resuming %s with %zu armies\n", o.league.c_str(), parsed.size());
    LeagueTrainer league(arena, std::move(parsed), resumed, o.rng, o.seeds);
    printSettings(arena, o.threads);
    RunLog log;
    if (!openMatchLog(o, arena, log)) return 1;

    auto t0 = Clock::now();
    computePayoffs({&league}, o.threads);
    std::printf("payoff matrix for %zu armies in %.1fs\n", league.armies().size(), secondsSince(t0));
    save(o.league, arena, league.armies(), league.meta(), league.payoff());

    const SearchOptions options = searchOptions(o);
    for (int iter = 1; iter <= o.iterations; ++iter) {
        const auto start = Clock::now();
        const IterationResult r = trainIteration({&league}, options, o.threads, [&](int gen) {
            std::printf("\r  iter %d  search gen %2d/%d  best %+.3f   ", iter, gen, o.generations,
                        league.bestFitness());
            std::fflush(stdout);
        })[0];
        std::printf("\r%60s\r", "");
        const auto& armies = league.armies();
        std::printf("iter %3d  league %3zu  gain %+.3f  ", iter, armies.size() - (r.added ? 1 : 0), r.gain);
        if (r.added)
            std::printf("(full mix %+.3f)  added #%zu (%s)", r.fullMix, armies.size() - 1,
                        summary(arena, armies.back()).c_str());
        else
            std::printf("no army beats the mixture");
        const double seconds = secondsSince(start);
        std::printf("  [vs %s]  %.1fs, %.0f matches/s\n", supportText(r).c_str(), seconds, r.matches / seconds);
        save(o.league, arena, armies, league.meta(), league.payoff());
    }

    printMixture(arena, league.armies(), league.meta());
    std::printf("saved %s and %s.json (total %.1fs)\n", o.league.c_str(), o.league.c_str(), secondsSince(t0));
    return 0;
}

// ---------------------------------------------------------------------------
// train --twins
// ---------------------------------------------------------------------------
std::string jsonText(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

// The "training" member of the shipped league's JSON: how it was trained and
// the evaluation curve (columns as in the CSV).
std::string trainingJson(const Options& o, int iteration, const std::vector<Evaluation>& history,
                         const std::string& stopReason) {
    std::ostringstream out;
    out << "\"training\": {\"mode\": \"twins\", \"iteration\": " << iteration << ", \"evalEvery\": " << o.evalEvery
        << ", \"eps\": " << o.rules.eps << ", \"delta\": " << o.rules.delta << ", \"patience\": " << o.rules.patience
        << ", \"rngA\": " << o.rng << ", \"rngB\": " << o.rng + kTwinSeedOffset
        << ", \"stopReason\": " << (stopReason.empty() ? std::string("null") : jsonText(stopReason))
        << ",\n    \"columns\": [\"iteration\", \"gainA\", \"gainB\", \"xA\", \"xB\", \"leagueA\", \"leagueB\", "
           "\"supportA\", \"supportB\", \"supportUnion\", \"seconds\"],\n    \"evaluations\": [";
    for (std::size_t i = 0; i < history.size(); ++i) {
        const Evaluation& e = history[i];
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s[%d, %.4f, %.4f, %.4f, %.4f, %d, %d, %d, %d, %d, %.1f]", i ? ", " : "",
                      e.iteration, e.gainA, e.gainB, e.xA, e.xB, e.leagueA, e.leagueB, e.supportA, e.supportB,
                      e.supportUnion, e.seconds);
        out << buf;
    }
    out << "]}";
    return out.str();
}

double round4(double v) { return std::round(v * 1e4) / 1e4; }

int trainTwins(const Options& o) {
    namespace fs = std::filesystem;
    // The twins' own state lives in a sibling directory: life_battle and the
    // static site offer every *.json directly in assets/leagues/ as a bot, so
    // nothing but the shipped league may be written there.
    const std::string dir = o.league + ".twins";
    const std::string pathA = dir + "/A.txt", pathB = dir + "/B.txt", csvPath = dir + "/eval.csv",
                      statePath = dir + "/state.txt";
    std::error_code ec;
    const bool resumed = fs::exists(pathA, ec) && fs::exists(pathB, ec);
    if (!resumed && fs::exists(o.league, ec)) {
        std::fprintf(stderr,
                     "%s exists but %s/ holds no twin leagues; twin training would overwrite it.\n"
                     "Choose a new --league file (or drop --twins to continue it as a single league).\n",
                     o.league.c_str(), dir.c_str());
        return 1;
    }
    Settings settings = o.settings;
    LeagueFile fileA, fileB;
    int iteration = 0;
    double secondsBefore = 0;
    std::vector<Evaluation> history;
    if (resumed) {
        std::string error, text;
        if (!readLeague(pathA, fileA, error) || !readLeague(pathB, fileB, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        settings = fileA.settings;
        if (o.settingsGiven)
            std::printf("note: %s exists, using its battle settings and ignoring the ones given\n", dir.c_str());
        if (readTextFile(statePath, text)) std::sscanf(text.c_str(), "iteration %d seconds %lf", &iteration, &secondsBefore);
        if (readTextFile(csvPath, text) && !parseEvaluationsCsv(text, history)) {
            std::fprintf(stderr, "cannot parse %s\n", csvPath.c_str());
            return 1;
        }
    } else if (!fs::create_directories(dir, ec) && !fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "cannot create %s\n", dir.c_str());
        return 1;
    }

    const Arena arena(settings);
    auto armiesA = parseArmies(arena, fileA.armies), armiesB = parseArmies(arena, fileB.armies);
    if (resumed)
        std::printf("resuming twin leagues in %s at iteration %d (A %zu, B %zu armies, %zu evaluations)\n", dir.c_str(),
                    iteration, armiesA.size(), armiesB.size(), history.size());
    else
        std::printf("new twin leagues in %s with %d random armies each\n", dir.c_str(), o.seeds);
    LeagueTrainer a(arena, std::move(armiesA), resumed, o.rng, o.seeds);
    LeagueTrainer b(arena, std::move(armiesB), resumed, o.rng + kTwinSeedOffset, o.seeds);
    const int maxIterations = o.iterationsGiven ? o.iterations : 1000;
    std::printf("search rng A %llu, B %llu; evaluation every %d iterations; stop at xA, xB < %.3f, or no drop of "
                "%.3f in %d evaluations, or after %d iterations",
                static_cast<unsigned long long>(o.rng), static_cast<unsigned long long>(o.rng + kTwinSeedOffset),
                o.evalEvery, o.rules.eps, o.rules.delta, o.rules.patience, maxIterations);
    if (o.maxHours > 0) std::printf(" or %.2f hours", o.maxHours);
    std::printf("\n");
    printSettings(arena, o.threads);
    RunLog log;
    if (!openMatchLog(o, arena, log)) return 1;

    const auto t0 = Clock::now();
    auto elapsed = [&] { return secondsBefore + secondsSince(t0); };
    computePayoffs({&a, &b}, o.threads);
    std::printf("payoff matrices for %zu + %zu armies in %.1fs\n", a.armies().size(), b.armies().size(),
                secondsSince(t0));

    auto saveTwins = [&] {
        std::string error;
        char state[96];
        std::snprintf(state, sizeof state, "iteration %d\nseconds %.1f\n", iteration, elapsed());
        if (!writeLeague(pathA, arena, a.armies(), error) || !writeLeague(pathB, arena, b.armies(), error) ||
            !writeTextFile(statePath, state, error))
            std::fprintf(stderr, "%s\n", error.c_str());
    };
    saveTwins();

    CrossMatrix cross;
    UnionLeague shipped;
    double gainA = history.empty() ? 0 : history.back().gainA, gainB = history.empty() ? 0 : history.back().gainB;
    // Cross-evaluation: only the battles of armies added since the last one are played.
    auto evaluate = [&] {
        const auto start = Clock::now();
        std::vector<MatchJob> jobs;
        cross.extend(a.armies(), b.armies(), jobs);
        playJobs(arena, jobs, o.threads);
        const CrossScore x = crossScore(cross.values(), a.meta(), b.meta());
        shipped = unionLeague(arena, a.armies(), a.payoff(), b.armies(), b.payoff(), cross.values());
        Evaluation e;
        e.iteration = iteration;
        e.gainA = round4(gainA);
        e.gainB = round4(gainB);
        e.xA = round4(x.xA);
        e.xB = round4(x.xB);
        e.leagueA = static_cast<int>(a.armies().size());
        e.leagueB = static_cast<int>(b.armies().size());
        e.supportA = supportSize(a.meta());
        e.supportB = supportSize(b.meta());
        e.supportUnion = supportSize(shipped.meta);
        e.seconds = std::round(elapsed() * 10) / 10;
        if (!history.empty() && history.back().iteration == iteration) history.pop_back();  // re-evaluated on resume
        history.push_back(e);
        std::string error;
        if (!writeTextFile(csvPath, evaluationsCsv(history), error)) std::fprintf(stderr, "%s\n", error.c_str());
        double best;
        int stale;
        plateauState(history, o.rules, best, stale);
        std::printf("  eval @%d: xA %+.3f (B #%d)  xB %+.3f (A #%d)  union %zu armies, support %d  "
                    "plateau %.3f, %d/%d stale  [%zu matches, %.1fs]\n",
                    iteration, x.xA, x.bestB, x.xB, x.bestA, shipped.armies.size(), e.supportUnion, best, stale,
                    o.rules.patience, jobs.size(), secondsSince(start));
    };
    auto ship = [&](const std::string& reason) {
        save(o.league, arena, shipped.armies, shipped.meta, shipped.payoff,
             trainingJson(o, iteration, history, reason));
    };

    const SearchOptions options = searchOptions(o);
    std::string reason;
    int done = 0;
    bool evaluated = false;
    for (;;) {
        std::string budget;
        char buf[96];
        if (done >= maxIterations) {
            std::snprintf(buf, sizeof buf, "budget: %d iterations", maxIterations);
            budget = buf;
        } else if (o.maxHours > 0 && secondsSince(t0) >= o.maxHours * 3600) {
            std::snprintf(buf, sizeof buf, "budget: %.2f hours", o.maxHours);
            budget = buf;
        }
        if (!budget.empty()) {
            if (!evaluated) evaluate(), reason = checkStop(history, o.rules);
            if (reason.empty()) reason = budget;
            ship(reason);
            break;
        }

        ++iteration;
        ++done;
        const auto start = Clock::now();
        const auto r = trainIteration({&a, &b}, options, o.threads, [&](int gen) {
            std::printf("\r  iter %d  search gen %2d/%d  best A %+.3f  B %+.3f   ", iteration, gen, o.generations,
                        a.bestFitness(), b.bestFitness());
            std::fflush(stdout);
        });
        std::printf("\r%70s\r", "");
        gainA = r[0].gain;
        gainB = r[1].gain;
        const double seconds = secondsSince(start);
        auto side = [&](const char* name, const LeagueTrainer& l, const IterationResult& res) {
            std::printf("%s %3zu  gain %+.3f %s", name, l.armies().size(), res.gain, res.added ? "added" : "-    ");
        };
        std::printf("iter %3d  ", iteration);
        side("A", a, r[0]);
        std::printf("  |  ");
        side("B", b, r[1]);
        std::printf("  |  support %zu+%zu  %.1fs, %.0f matches/s\n", r[0].support.size(), r[1].support.size(),
                    seconds, (r[0].matches + r[1].matches) / seconds);
        saveTwins();
        evaluated = false;
        if (iteration % o.evalEvery == 0) {
            evaluate();
            evaluated = true;
            reason = checkStop(history, o.rules);
            ship(reason);
            if (!reason.empty()) break;
        }
    }

    std::printf("\nstopped after iteration %d: %s\n", iteration, reason.c_str());
    printMixture(arena, shipped.armies, shipped.meta);
    std::printf("saved %s and %s.json (union of A and B), twin state and eval.csv in %s/ (this run %.1fs)\n",
                o.league.c_str(), o.league.c_str(), dir.c_str(), secondsSince(t0));
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

    const Matrix m = payoffMatrix(arena, armies, o.threads);
    const auto meta = solveNash(m);
    std::printf("%zu armies, board %dx%d, preset %s, budget %d, maxgen %d, %s\n\n", armies.size(), settings.width,
                settings.height, settings.preset.c_str(), settings.budget, settings.maxGenerations,
                variantName(settings.variant));
    std::printf("  id  weight  vs mix  cost  origin     pieces\n");
    for (std::size_t i = 0; i < armies.size(); ++i)
        std::printf("  %2zu  %5.1f%%  %+.3f  %4d  %-9s  %s\n", i, 100 * meta[i], expectedScore(m, static_cast<int>(i), meta),
                    arena.cost(armies[i]), armies[i].origin.c_str(), summary(arena, armies[i]).c_str());
    return 0;
}

// Cell bitmaps of the logged armies for model training: P.raster.bin holds,
// for every army of P.armies.tsv in id order, its red zone (zone width x height
// cells, row by row) packed 8 cells per byte, most significant bit first.
// Rerunning appends only the armies that are new since the last run.
int raster(const Options& o) {
    Settings settings;
    std::vector<std::pair<std::string, std::string>> raw;
    if (!loadLeague(o, settings, raw, false)) return 1;
    const Arena arena(settings);
    if (o.logMatches.empty()) {
        std::fprintf(stderr, "raster needs --log-matches P\n");
        return 2;
    }
    const int w = settings.width, h = settings.height, zw = arena.zones().leftEnd;
    const std::size_t bytes = (static_cast<std::size_t>(zw) * h + 7) / 8;
    const std::string out = o.logMatches + ".raster.bin";
    std::error_code ec;
    const auto existing = std::filesystem::exists(out) ? std::filesystem::file_size(out, ec) : 0;
    const std::size_t done = existing / bytes;
    std::filesystem::resize_file(out, done * bytes, ec);  // drops a partial record

    std::ifstream in(o.logMatches + ".armies.tsv");
    std::FILE* f = std::fopen(out.c_str(), "ab");
    if (!in || !f) {
        std::fprintf(stderr, "cannot read %s.armies.tsv or write %s\n", o.logMatches.c_str(), out.c_str());
        return 1;
    }
    std::string line;
    std::size_t id = 0, added = 0;
    std::vector<std::uint8_t> packed(bytes);
    while (std::getline(in, line)) {
        if (id++ < done) continue;
        Army army;
        const auto tab = line.find('\t');
        if (tab == std::string::npos || !parsePieces(arena, line.substr(tab + 1), army)) {
            std::fprintf(stderr, "bad army line %zu\n", id - 1);
            std::fclose(f);
            return 1;
        }
        const auto cells = arena.board(army, Army{});
        std::fill(packed.begin(), packed.end(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < zw; ++x)
                if (cells[static_cast<std::size_t>(y) * w + x] == gol::Red) {
                    const std::size_t k = static_cast<std::size_t>(y) * zw + x;
                    packed[k / 8] |= static_cast<std::uint8_t>(0x80 >> (k % 8));
                }
        std::fwrite(packed.data(), 1, bytes, f);
        ++added;
    }
    std::fclose(f);
    const char* symmetry = arena.symmetry() == Arena::Symmetry::Mirror     ? "mirror"
                           : arena.symmetry() == Arena::Symmetry::Rotation ? "rotation"
                                                                           : "none";
    std::FILE* meta = std::fopen((o.logMatches + ".raster.json").c_str(), "w");
    if (meta) {
        std::fprintf(meta, "{\"width\": %d, \"height\": %d, \"zoneWidth\": %d, \"armies\": %zu, \"symmetry\": \"%s\", "
                           "\"preset\": \"%s\", \"maxGenerations\": %d}\n",
                     w, h, zw, done + added, symmetry, settings.preset.c_str(), settings.maxGenerations);
        std::fclose(meta);
    }
    std::printf("%s: %zu armies (%zu new), %d x %d cells each\n", out.c_str(), done + added, added, zw, h);
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
    // Battles between league armies (as in training) or between random armies.
    Settings settings = o.settings;
    std::vector<std::pair<std::string, std::string>> raw;
    if (o.leagueGiven && !loadLeague(o, settings, raw, false)) return 1;
    const Arena arena(settings);
    Rng rng(o.rng);
    std::vector<std::vector<std::uint8_t>> boards;
    const auto armies = parseArmies(arena, raw);
    if (o.leagueGiven && armies.size() < 2) {
        std::fprintf(stderr, "bench needs a league with at least two armies\n");
        return 1;
    }
    for (int i = 0; i < o.battles; ++i) {
        if (armies.empty()) {
            boards.push_back(arena.board(arena.randomArmy(rng), arena.randomArmy(rng)));
        } else {
            const std::size_t n = armies.size(), a = static_cast<std::size_t>(i) % n;
            const std::size_t b = (a + 1 + static_cast<std::size_t>(i) / n % (n - 1)) % n;
            boards.push_back(arena.board(armies[a], armies[b]));
        }
    }
    const gol::BattleConfig cfg{settings.width, settings.height, settings.variant, settings.maxGenerations};

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

    // Throughput on all threads, as in training (each battle several times
    // so that every thread has enough work).
    const int rounds = std::max(1, (16 * o.threads + o.battles - 1) / o.battles);
    t = Clock::now();
    parallelFor(o.battles * rounds, o.threads, [&](int i) {
        gol::FastBattle battle(cfg, boards[static_cast<std::size_t>(i % o.battles)]);
        battle.runToEnd();
    });
    const double parallel = secondsSince(t);

    std::printf("%d battles (%s) on %dx%d, %lld generations in total\n", o.battles,
                armies.empty() ? "random armies" : "league armies", settings.width, settings.height, gens);
    std::printf("  reference engine:    %7.2f ms/battle  %8.0f gen/s\n", 1000 * slow / o.battles, slowGens / slow);
    std::printf("  bit-parallel engine: %7.2f ms/battle  %8.0f gen/s  (%.1fx faster)\n", 1000 * fast / o.battles,
                gens / fast, slow / fast);
    std::printf("  bit-parallel, %d threads: %.0f battles/s\n", o.threads, o.battles * rounds / parallel);
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
    if (o.command == "train") return o.twins ? trainTwins(o) : train(o);
    if (o.command == "show") return show(o);
    if (o.command == "match") return match(o);
    if (o.command == "raster") return raster(o);
    if (o.command == "bench") return bench(o);
    usage();
    return 2;
}
