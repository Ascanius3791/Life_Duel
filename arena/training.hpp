#pragma once

// League training (PSRO): a league of armies with their payoff matrix, the
// evolutionary best-response search against its Nash mixture, and the pieces
// of twin training: two leagues trained side by side with different random
// seeds, the payoffs between them (cross-evaluation), their union and the
// stopping rules.
//
// Battles are collected as jobs and played in one batch, so several leagues
// can search at the same time and share the worker threads. Every job writes
// to its own slot, so the results never depend on the number of threads.

#include "arena.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena {

using Matrix = std::vector<std::vector<double>>;

// One match to play: *out = payoff(*a, *b), and *neg = -*out if neg is set.
struct MatchJob {
    const Army* a = nullptr;
    const Army* b = nullptr;
    double* out = nullptr;
    double* neg = nullptr;
};
void playJobs(const Arena& arena, const std::vector<MatchJob>& jobs, int threads);
// While set, playJobs records every battle it plays in `log` (nullptr stops it).
class MatchLog;
void setMatchLog(MatchLog* log);

// Expected score of army i (row i of m) against a mixture.
double expectedScore(const Matrix& m, int i, const std::vector<double>& meta);
int supportSize(const std::vector<double>& meta);

struct SearchOptions {
    int population = 24;
    int generations = 20;
    int support = 6;        // strongest mixture members the search plays against
    double minGain = 0.02;  // minimum score vs the mixture to join the league
};

struct IterationResult {
    double gain = 0;     // best score of the search against the (truncated) mixture
    double fullMix = 0;  // the added army's score against the whole mixture
    bool added = false;
    std::vector<int> support;
    std::vector<double> weight;
    long long matches = 0;  // matches played in this iteration
};

// One league in training. The search is a small state machine so that the
// battles of several leagues can be played together:
//   beginSearch, then while searching(): generationJobs, play, endGeneration;
//   then addBest, play, finishIteration.
// trainIteration() below does exactly that.
class LeagueTrainer {
public:
    // A new league (`resumed` false) gets `seeds` random armies; a resumed one
    // continues with its armies and a random generator derived from their count.
    LeagueTrainer(const Arena& arena, std::vector<Army> armies, bool resumed, std::uint64_t seed, int seeds);

    const Arena& arena() const { return arena_; }
    const std::vector<Army>& armies() const { return armies_; }
    const Matrix& payoff() const { return m_; }
    const std::vector<double>& meta() const { return meta_; }

    // Payoffs not computed yet (all of them after construction). Call
    // finishPayoff() after playing them.
    void payoffJobs(std::vector<MatchJob>& jobs);
    void finishPayoff();

    void beginSearch(const SearchOptions& options);
    bool searching() const { return searching_; }
    int generation() const { return gen_; }
    double bestFitness() const { return pop_.empty() ? 0 : pop_[0].fitness; }
    void generationJobs(std::vector<MatchJob>& jobs);
    void endGeneration();
    // Adds the search's best army if it gains enough; its missing payoffs go to `jobs`.
    void addBest(std::vector<MatchJob>& jobs);
    IterationResult finishIteration();

private:
    // Jobs for the payoff rows not computed yet; `known` supplies entries that need no battle.
    using KnownPayoff = std::function<bool(int i, int j, double& v)>;
    void extendJobs(std::vector<MatchJob>& jobs, const KnownPayoff& known);

    struct Individual {
        Army army;
        double fitness = 0;
        bool evaluated = false;
    };

    const Arena& arena_;
    std::vector<Army> armies_;
    Matrix m_;
    std::vector<double> meta_;
    Rng rng_;
    std::unordered_map<std::string, int> leagueIndex_;

    SearchOptions options_;
    bool searching_ = false;
    int gen_ = 0;
    std::vector<int> support_;
    std::vector<double> weight_;
    std::vector<Individual> pop_;
    std::unordered_map<std::string, std::vector<double>> seen_;
    std::vector<int> todo_;
    std::vector<std::vector<double>*> rows_;
    IterationResult result_;
};

// Payoffs for all leagues' pending jobs at once.
void computePayoffs(const std::vector<LeagueTrainer*>& leagues, int threads);
// One PSRO iteration for every league; the searches run in lockstep and each
// search generation of all leagues is played as one batch. `progress` is
// called after every search generation.
using SearchProgress = std::function<void(int generation)>;
std::vector<IterationResult> trainIteration(const std::vector<LeagueTrainer*>& leagues, const SearchOptions& options,
                                            int threads, const SearchProgress& progress = nullptr);

// ---------------------------------------------------------------------------
// Seeding with armies of older leagues
// ---------------------------------------------------------------------------
// Armies bought under other prices (or another budget) can cost more than the
// budget. fitToBudget sells pieces of every such army, one at a time, until it
// fits: each time the piece whose sale hurts least, i.e. leaves the best
// expected score against the mixture (`opponents` with `weights`, normally the
// Nash mixture of the source league). Only pieces whose sale alone makes the
// army fit are candidates, unless there are none; then every piece is. Ties
// sell the later piece. Battles are deterministic, and so is the result.
struct Refit {
    int army = 0;                   // index into the armies passed in
    std::vector<std::string> sold;  // pattern ids of the sold pieces, in order
    int points = 0;                 // their price
    double before = 0, after = 0;   // score against the mixture before and after
};
std::vector<Refit> fitToBudget(const Arena& arena, std::vector<Army>& armies, const std::vector<Army>& opponents,
                               const std::vector<double>& weights, int threads);

// ---------------------------------------------------------------------------
// Pool training
// ---------------------------------------------------------------------------
// A pool of at most `capacity` armies with the complete table of their
// results against each other. Battles are deterministic, so one battle per
// pair is the exact result, and an army's score is its exact mean against all
// other armies in the pool *now*: no sampling luck, and wins against armies
// that have left the pool no longer count. The table holds capacity^2 floats
// (4 MB at 1000, 400 MB at 10000). Every army that joins plays everyone
// already there, so the table stays complete.
struct Pool {
    int capacity = 1000;
    long long replaced = 0;  // armies removed from the pool so far (the generation number)
    int nextId = 1;          // number of the next army that joins ("pool N")
    std::vector<Army> armies;                // oldest first
    std::vector<std::vector<float>> payoff;  // payoff[i][j]: score of army i against army j
    std::vector<double> sum;                 // row sums of payoff

    int size() const { return static_cast<int>(armies.size()); }
    double score(int i) const {  // mean against all other pool armies
        return size() > 1 ? sum[static_cast<std::size_t>(i)] / (size() - 1) : 0;
    }
    int weakest() const;  // index of the lowest score (-1 if empty)
    void remove(int i);  // (does not count as a replacement)
    // Appends `army`; row[j] = its score against armies[j] for every current army.
    void add(Army army, const std::vector<float>& row);
};

// Plays every pair of `armies` once and makes them the pool (keeping its
// capacity and counters). Returns the number of battles.
long long fillPool(const Arena& arena, Pool& pool, std::vector<Army> armies, int threads);

struct PoolOptions {
    int population = 24;
    int search = 20;  // rounds of the evolutionary search
    int sample = 24;  // opponents per search round
    // Successive halving within a round, e.g. {4, 8, 16}: every candidate
    // plays the first 4 opponents of the sample, the better half plays on up to
    // 8, the better half of those up to 16 (the last entry replaces `sample`).
    // Empty: every candidate plays the whole sample.
    std::vector<int> halving;
    // The entrance test: every new army of the last search round plays the
    // pool armies in the same random order, a batch at a time (16 first, then
    // 8 or a quarter of those played so far). After each batch a candidate
    // drops out once we are `confidence` sure (one-sided, normal approximation
    // with the finite-population correction) that its mean against the whole
    // pool will end below minGain, or that `maxEntrants` other candidates
    // will end above it (paired on the same opponents). The rest play
    // everyone, which gives their exact means.
    double confidence = 0.95;
    double minGain = 0;   // the score against the pool needed to join ("beats the pool")
    int maxEntrants = 1;  // armies that may join per iteration
    // A candidate whose results against the pool correlate above this with
    // those of a pool army is a copy of it and does not join (> 1: no check).
    double maxSimilar = 0.9;
};

struct PoolEntry {
    std::string origin;   // "pool N"
    double score = 0;     // exact mean against the pool it joined
    double similar = 0;   // highest correlation with a pool army's results
    std::string removed;  // origin of the army it replaced, if any
};

struct PoolResult {
    double fitness = 0;  // best fitness in the last search round (on that round's sample)
    double best = 0;     // best exact mean of a candidate (or best mean so far, if all dropped out)
    int tested = 0;      // new armies that took the entrance test
    int belowBar = 0;    // of which dropped out below minGain
    int behind = 0;      // dropped out behind maxEntrants others
    int copies = 0;      // passed, but too similar to a pool army
    int left = 0;        // passed and not copies, but beyond maxEntrants
    std::vector<PoolEntry> entries;
    long long matches = 0;      // battles in total
    long long testMatches = 0;  // of which in the entrance test and with the new armies
};

// One iteration: an evolutionary search for armies that beat the pool,
// judged in every round against `sample` pool armies drawn anew at random (so
// the candidates cannot specialise on a few opponents), then the entrance
// test. The armies that pass join one after another, best first, as long as
// they still beat the pool (which now includes the ones before them) by
// minGain and are not copies; once the pool is full each replaces the army
// with the lowest exact score. They are appended with origin "pool N".
using PoolProgress = std::function<void(int round, double best)>;
PoolResult poolIteration(const Arena& arena, Pool& pool, const PoolOptions& options, Rng& rng, int threads,
                         const PoolProgress& progress = nullptr);

// ---------------------------------------------------------------------------
// Twin training
// ---------------------------------------------------------------------------

// c[b][a] = payoff(B[b], A[a]) for two leagues A and B. It grows with the
// leagues (armies are only ever appended); only new pairs are played.
class CrossMatrix {
public:
    void extend(const std::vector<Army>& a, const std::vector<Army>& b, std::vector<MatchJob>& jobs);
    const Matrix& values() const { return c_; }

private:
    Matrix c_;
    std::size_t cols_ = 0;
};

// xA = max over B's armies of their score against A's mixture: a lower bound
// on how exploitable A is (B never trained against A). xB the other way round.
struct CrossScore {
    double xA = 0, xB = 0;
    int bestB = -1, bestA = -1;  // the armies reaching them
};
CrossScore crossScore(const Matrix& c, const std::vector<double>& metaA, const std::vector<double>& metaB);

// Union of A and B (B's armies that equal one of A's are left out), with its
// payoff matrix assembled from both leagues and the cross matrix, and its
// Nash mixture. Origins get " (A)" / " (B)" appended.
struct UnionLeague {
    std::vector<Army> armies;
    Matrix payoff;
    std::vector<double> meta;
};
UnionLeague unionLeague(const Arena& arena, const std::vector<Army>& a, const Matrix& ma, const std::vector<Army>& b,
                        const Matrix& mb, const Matrix& cross);

struct Evaluation {
    int iteration = 0;
    double gainA = 0, gainB = 0;
    double xA = 0, xB = 0;
    int leagueA = 0, leagueB = 0;
    int supportA = 0, supportB = 0, supportUnion = 0;
    double seconds = 0;
};

struct StopRules {
    double eps = 0.05;    // converged: xA and xB both below it
    double delta = 0.02;  // plateau: the smoothed max(xA, xB) has not dropped by delta ...
    int patience = 6;     // ... for this many evaluations in a row
    static constexpr int smoothing = 3;  // evaluations averaged for the plateau rule
};

// Why training should stop after the last evaluation of `history`, or "" to go
// on. Converged is checked first, then plateau. The plateau value is the mean
// of max(xA, xB) over the last `smoothing` evaluations; its best-so-far can only
// drop by delta at a time within [-1, 1], so with delta > 0 the plateau rule
// always fires after at most (2 / delta + 1) * (patience + 1) evaluations.
std::string checkStop(const std::vector<Evaluation>& history, const StopRules& rules);
// The plateau value of the last evaluation and how many evaluations it has gone without improving.
void plateauState(const std::vector<Evaluation>& history, const StopRules& rules, double& best, int& stale);

// The evaluation curve as CSV (with header) and back.
std::string evaluationsCsv(const std::vector<Evaluation>& history);
bool parseEvaluationsCsv(const std::string& text, std::vector<Evaluation>& history);

}  // namespace arena
