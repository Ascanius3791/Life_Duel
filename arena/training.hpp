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
