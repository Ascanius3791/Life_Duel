// Tests for the bot-training building blocks.
#include "arena.hpp"
#include "league.hpp"
#include "training.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace arena;

namespace {

// Armies and payoffs of trained leagues, as text, for exact comparisons.
std::string dump(const Arena& arena, const std::vector<LeagueTrainer*>& leagues) {
    std::string out;
    for (const auto* l : leagues) {
        for (const auto& a : l->armies()) out += formatPieces(arena, a) + "\n";
        char buf[32];
        for (const auto& row : l->payoff())
            for (double v : row) std::snprintf(buf, sizeof buf, "%.17g ", v), out += buf;
        out += "|\n";
    }
    return out;
}

Evaluation eval(double xA, double xB) {
    Evaluation e;
    e.xA = xA;
    e.xB = xB;
    return e;
}

void testTwins() {
    Settings s;
    s.width = 120;
    s.height = 60;
    s.preset = "random_mirror";
    s.count = 300;
    s.maxGenerations = 300;
    const Arena arena(s);

    // Twin determinism: the same seeds give the same leagues whatever the number
    // of threads, and sharing the thread pool with B does not change A at all:
    // twin A is exactly the single league with A's seed.
    SearchOptions options;
    options.population = 8;
    options.generations = 3;
    options.support = 3;
    auto run = [&](int threads, bool twins) {
        LeagueTrainer a(arena, {}, false, 5, 4), b(arena, {}, false, 5 + 1000, 4);
        std::vector<LeagueTrainer*> leagues{&a};
        if (twins) leagues.push_back(&b);
        computePayoffs(leagues, threads);
        for (int i = 0; i < 3; ++i) trainIteration(leagues, options, threads);
        return std::make_pair(dump(arena, {&a}), twins ? dump(arena, {&b}) : std::string());
    };
    const auto twins1 = run(1, true), twins4 = run(4, true), single = run(3, false);
    CHECK(twins1 == twins4);
    CHECK(twins4.first == single.first);
    CHECK(twins4.first != twins4.second);

    // Cross-evaluation: c[b][a] = payoff(B[b], A[a]); only new pairs are played.
    Rng rng(11);
    std::vector<Army> A, B;
    for (int i = 0; i < 3; ++i) A.push_back(arena.randomArmy(rng));
    for (int i = 0; i < 2; ++i) B.push_back(arena.randomArmy(rng));
    CrossMatrix cross;
    std::vector<MatchJob> jobs;
    cross.extend(A, B, jobs);
    CHECK(jobs.size() == 6);
    playJobs(arena, jobs, 2);
    A.push_back(arena.randomArmy(rng));
    B.push_back(arena.randomArmy(rng));
    jobs.clear();
    cross.extend(A, B, jobs);
    CHECK(jobs.size() == 4 * 3 - 3 * 2);
    playJobs(arena, jobs, 2);
    const Matrix& c = cross.values();
    CHECK(c.size() == 3 && c[0].size() == 4);
    for (std::size_t b = 0; b < B.size(); ++b)
        for (std::size_t a = 0; a < A.size(); ++a) CHECK(c[b][a] == arena.payoff(B[b], A[a]));

    // xA: best score of a B army against A's mixture; xB the other way round.
    const auto x = crossScore({{0.5, -0.2}, {-0.1, 0.3}}, {1, 0}, {0.5, 0.5});
    CHECK(std::fabs(x.xA - 0.5) < 1e-12 && x.bestB == 0);
    CHECK(std::fabs(x.xB - -0.05) < 1e-12 && x.bestA == 1);
    const std::vector<double> uniformA(4, 0.25), uniformB(3, 1.0 / 3);
    const auto xr = crossScore(c, uniformA, uniformB);
    double bestA = -2, bestB = -2;
    for (const auto& b : B) {
        double v = 0;
        for (const auto& a : A) v += 0.25 * arena.payoff(b, a);
        bestA = std::max(bestA, v);
    }
    for (const auto& a : A) {
        double v = 0;
        for (const auto& b : B) v += arena.payoff(a, b) / 3;
        bestB = std::max(bestB, v);
    }
    CHECK(std::fabs(xr.xA - bestA) < 1e-12 && std::fabs(xr.xB - bestB) < 1e-12);

    // Union: B's copy of an A army is left out; the assembled matrix is the real one.
    std::vector<Army> B2 = B;
    B2[1] = A[2];
    jobs.clear();
    CrossMatrix cross2;
    cross2.extend(A, B2, jobs);
    playJobs(arena, jobs, 2);
    auto league = [&](const std::vector<Army>& armies) {
        Matrix m(armies.size(), std::vector<double>(armies.size(), 0));
        for (std::size_t i = 0; i < armies.size(); ++i)
            for (std::size_t j = 0; j < armies.size(); ++j)
                if (i != j) m[i][j] = arena.payoff(armies[i], armies[j]);
        return m;
    };
    const UnionLeague u = unionLeague(arena, A, league(A), B2, league(B2), cross2.values());
    CHECK(u.armies.size() == A.size() + B2.size() - 1);
    CHECK(u.payoff == league(u.armies));
    double total = 0;
    for (double w : u.meta) total += w;
    CHECK(std::fabs(total - 1) < 1e-9);

    // Stopping rules.
    StopRules rules;  // eps 0.05, delta 0.02, patience 6
    CHECK(checkStop({}, rules).empty());
    CHECK(checkStop({eval(0.3, 0.2), eval(0.01, 0.04)}, rules).rfind("converged", 0) == 0);
    CHECK(checkStop({eval(0.01, 0.2)}, rules).empty());
    std::vector<Evaluation> flat;
    for (int i = 0; i <= rules.patience; ++i) {
        CHECK(checkStop(flat, rules).empty());
        flat.push_back(eval(0.3, 0.1));
    }
    CHECK(checkStop(flat, rules).rfind("plateau", 0) == 0);
    // Steady progress never plateaus; it ends converged.
    std::vector<Evaluation> falling;
    std::string why;
    for (int i = 0; i < 100 && why.empty(); ++i) {
        falling.push_back(eval(0.9 - 0.04 * i, 0.5 - 0.04 * i));
        why = checkStop(falling, rules);
    }
    CHECK(why.rfind("converged", 0) == 0);
    // Whatever the evaluations do, the rules fire within the documented bound.
    const int bound = static_cast<int>((2 / rules.delta + 1) * (rules.patience + 1));
    for (int seed = 0; seed < 20; ++seed) {
        Rng r(seed);
        std::uniform_real_distribution<double> any(-1, 1), down(0, 0.03);
        std::vector<Evaluation> h;
        double level = 1;
        while (checkStop(h, rules).empty() && static_cast<int>(h.size()) <= bound) {
            level = seed % 2 ? any(r) : std::max(-1.0, level - down(r));  // noise, or a slow fall
            h.push_back(eval(level, seed % 2 ? any(r) : 0.9));
        }
        CHECK(static_cast<int>(h.size()) <= bound);
    }

    // The evaluation curve survives a CSV round trip (values are kept to 4 decimals).
    std::vector<Evaluation> curve{eval(0.1234, -0.5), eval(0.02, 0.0301)};
    curve[1].iteration = 10;
    curve[1].supportUnion = 7;
    curve[1].seconds = 12.5;
    std::vector<Evaluation> back;
    CHECK(parseEvaluationsCsv(evaluationsCsv(curve), back));
    CHECK(back.size() == 2 && back[1].iteration == 10 && back[1].supportUnion == 7 && back[1].seconds == 12.5 &&
          back[0].xA == 0.1234 && back[1].xB == 0.0301);
}

// train --from: armies over the budget are repaired deterministically, empty
// and duplicate armies are dropped.
void testSeedArmies() {
    Settings s;
    s.width = 120;
    s.height = 60;
    s.preset = "empty";
    s.maxGenerations = 100;
    s.budget = 20;
    const Arena arena(s);
    const std::vector<std::pair<std::string, std::string>> raw = {
        {"fits", "block@1,1,r0,m0;glider@10,10,r0,m0"},                                    // 4 + 5
        {"over", "block@1,1,r0,m0;pulsar@20,20,r0,m0;glider@10,10,r0,m0;cell@5,5,r0,m0"},  // 4 + 48 + 5 + 1
        {"again", "block@1,1,r0,m0;glider@10,10,r0,m0"},                                   // same as "fits"
        {"empty", ""},
        {"huge", "gosper_gun@1,1,r0,m0;gosper_gun@30,30,r0,m0"},                           // 36 cells each
    };
    SeedReport report;
    const auto armies = seedArmies(arena, raw, "src.txt", report);
    CHECK(armies.size() == 2);
    CHECK(report.kept == 2 && report.repaired == 2 && report.duplicates == 1 && report.empty == 2);
    for (const auto& a : armies) CHECK(arena.legal(a));
    // "over" (58 points) loses its last pieces until it fits: cell, glider and pulsar go.
    CHECK(formatPieces(arena, armies[1]) == "block@1,1,r0,m0");
    CHECK(armies[0].origin == "fits (src.txt)");
}

}  // namespace

int main() {
    testSeedArmies();
    for (const char* preset : {"random_mirror", "random_rotational", "random"}) {
        Settings s;
        s.width = 120;
        s.height = 60;
        s.preset = preset;
        s.count = 300;
        s.maxGenerations = 300;
        const Arena arena(s);
        Rng rng(3);

        // Mutation and crossover always give legal armies, even from an empty one.
        Army army;
        for (int i = 0; i < 2000; ++i) {
            army = i % 50 == 0 ? Army{} : arena.mutate(army, rng);
            CHECK(arena.legal(army));
        }
        for (int i = 0; i < 200; ++i) CHECK(arena.legal(arena.crossover(arena.randomArmy(rng), Army{}, rng)));

        // Both colours get the same army: swapping sides flips the score exactly.
        for (int i = 0; i < 5; ++i) {
            const Army a = arena.randomArmy(rng), b = arena.randomArmy(rng);
            CHECK(arena.payoff(a, b) == -arena.payoff(b, a));
            CHECK(arena.payoff(a, a) == 0 || arena.symmetry() == Arena::Symmetry::None);
        }

        // Text form round trip.
        const Army a = arena.randomArmy(rng);
        Army back;
        CHECK(parsePieces(arena, formatPieces(arena, a), back));
        CHECK(formatPieces(arena, back) == formatPieces(arena, a));
    }

    // Rock-paper-scissors: the Nash mixture is uniform.
    const auto meta = solveNash({{0, 1, -1}, {-1, 0, 1}, {1, -1, 0}});
    for (double w : meta) CHECK(w > 0.3 && w < 0.37);
    // A dominant strategy takes everything.
    const auto pure = solveNash({{0, 1, 1}, {-1, 0, 1}, {-1, -1, 0}});
    CHECK(pure[0] > 0.99);

    testTwins();

    if (failures == 0) std::printf("All arena tests passed.\n");
    return failures == 0 ? 0 : 1;
}
