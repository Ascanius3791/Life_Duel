#include "training.hpp"

#include "match_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace arena {

namespace {
MatchLog* matchLog = nullptr;

// Arena::payoff, with every battle recorded in the log.
double loggedPayoff(const Arena& arena, const Army& a, const Army& b) {
    const MatchResult r = arena.play(a, b);
    matchLog->record(a, b, r);
    if (arena.symmetry() != Arena::Symmetry::None) return r.score;
    const MatchResult swapped = arena.play(b, a);
    matchLog->record(b, a, swapped);
    return (r.score - swapped.score) / 2;
}
}  // namespace

void setMatchLog(MatchLog* log) { matchLog = log; }

void playJobs(const Arena& arena, const std::vector<MatchJob>& jobs, int threads) {
    if (matchLog) matchLog->beginBatch();
    parallelFor(static_cast<int>(jobs.size()), threads, [&](int k) {
        const MatchJob& job = jobs[static_cast<std::size_t>(k)];
        const double v = matchLog ? loggedPayoff(arena, *job.a, *job.b) : arena.payoff(*job.a, *job.b);
        *job.out = v;
        if (job.neg) *job.neg = -v;  // zero-sum and symmetric: the swapped battle is the mirror image
    });
    if (matchLog) matchLog->flush();
}

double expectedScore(const Matrix& m, int i, const std::vector<double>& meta) {
    double v = 0;
    for (std::size_t j = 0; j < meta.size() && j < m[i].size(); ++j) v += m[i][j] * meta[j];
    return v;
}

int supportSize(const std::vector<double>& meta) {
    return static_cast<int>(std::count_if(meta.begin(), meta.end(), [](double w) { return w > 0; }));
}

// ---------------------------------------------------------------------------
// LeagueTrainer
// ---------------------------------------------------------------------------
LeagueTrainer::LeagueTrainer(const Arena& arena, std::vector<Army> armies, bool resumed, std::uint64_t seed, int seeds)
    : arena_(arena), armies_(std::move(armies)), rng_(seed) {
    if (!resumed) {
        for (int i = 0; i < seeds; ++i) armies_.push_back(arena_.randomArmy(rng_));
    } else {
        rng_.seed(seed ^ (armies_.size() * 0x9e3779b97f4a7c15ull));
    }
    if (armies_.empty()) armies_.push_back(arena_.randomArmy(rng_));
    // Armies are identified by content so that no battle is played twice:
    // league members take their scores from the payoff matrix, and an army that
    // shows up again during a search (a support army in the starting population,
    // a child equal to an earlier one) reuses its scores. The values are the
    // same as replaying the battles, because battles are deterministic and
    // payoff(a, b) == -payoff(b, a) exactly.
    for (std::size_t i = 0; i < armies_.size(); ++i)
        leagueIndex_.emplace(armyKey(arena_, armies_[i]), static_cast<int>(i));
}

void LeagueTrainer::extendJobs(std::vector<MatchJob>& jobs, const KnownPayoff& known) {
    const std::size_t n = armies_.size(), old = m_.size();
    m_.resize(n);
    for (auto& row : m_) row.resize(n, 0);
    for (std::size_t i = old; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j) {
            double v;
            if (known && known(static_cast<int>(i), static_cast<int>(j), v)) {
                m_[i][j] = v;
                m_[j][i] = -v;
            } else {
                jobs.push_back({&armies_[i], &armies_[j], &m_[i][j], &m_[j][i]});
                ++result_.matches;
            }
        }
}

void LeagueTrainer::payoffJobs(std::vector<MatchJob>& jobs) { extendJobs(jobs, nullptr); }

void LeagueTrainer::finishPayoff() { meta_ = solveNash(m_); }

void LeagueTrainer::beginSearch(const SearchOptions& options) {
    options_ = options;
    result_ = IterationResult{};
    meta_ = solveNash(m_);

    // Opponents: the strongest members of the mixture, re-weighted.
    support_.clear();
    weight_.clear();
    for (std::size_t i = 0; i < meta_.size(); ++i)
        if (meta_[i] > 0) support_.push_back(static_cast<int>(i));
    std::sort(support_.begin(), support_.end(), [&](int a, int b) { return meta_[a] > meta_[b]; });
    if (static_cast<int>(support_.size()) > options_.support) support_.resize(options_.support);
    double total = 0;
    for (int s : support_) total += meta_[s];
    for (int s : support_) weight_.push_back(meta_[s] / total);

    // Starting population: the mixture's armies, mutations of them, crossovers and fresh random armies.
    pop_.clear();
    auto anyLeague = [&]() -> const Army& {
        return armies_[std::uniform_int_distribution<int>(0, static_cast<int>(armies_.size()) - 1)(rng_)];
    };
    auto anySupport = [&]() -> const Army& {
        return armies_[support_[std::uniform_int_distribution<int>(0, static_cast<int>(support_.size()) - 1)(rng_)]];
    };
    for (int s : support_)
        if (static_cast<int>(pop_.size()) < options_.population / 4) pop_.push_back({armies_[s]});
    while (static_cast<int>(pop_.size()) < options_.population) {
        const int kind = std::uniform_int_distribution<int>(0, 9)(rng_);
        Army a = kind < 5   ? arena_.mutate(anySupport(), rng_)
                 : kind < 7 ? arena_.mutate(arena_.crossover(anySupport(), anyLeague(), rng_), rng_)
                            : arena_.randomArmy(rng_);
        pop_.push_back({std::move(a)});
    }
    seen_.clear();  // scores against the support of every army seen in this iteration
    gen_ = 0;
    searching_ = true;
    result_.support = support_;
    result_.weight = weight_;
}

void LeagueTrainer::generationJobs(std::vector<MatchJob>& jobs) {
    // Everything new plays every opponent.
    todo_.clear();
    for (std::size_t k = 0; k < pop_.size(); ++k)
        if (!pop_[k].evaluated) todo_.push_back(static_cast<int>(k));
    rows_.assign(todo_.size(), nullptr);
    for (std::size_t t = 0; t < todo_.size(); ++t) {
        const Army& army = pop_[todo_[t]].army;
        const std::string key = armyKey(arena_, army);
        auto [it, inserted] = seen_.emplace(key, std::vector<double>(support_.size()));
        rows_[t] = &it->second;  // stays valid: unordered_map nodes do not move
        if (!inserted) continue;
        const auto member = leagueIndex_.find(key);
        if (member != leagueIndex_.end()) {
            for (std::size_t s = 0; s < support_.size(); ++s) it->second[s] = m_[member->second][support_[s]];
        } else {
            for (std::size_t s = 0; s < support_.size(); ++s)
                jobs.push_back({&army, &armies_[support_[s]], &it->second[s], nullptr});
            result_.matches += static_cast<long long>(support_.size());
        }
    }
}

void LeagueTrainer::endGeneration() {
    for (std::size_t t = 0; t < todo_.size(); ++t) {
        double f = 0;
        for (std::size_t s = 0; s < support_.size(); ++s) f += weight_[s] * (*rows_[t])[s];
        pop_[todo_[t]].fitness = f;
        pop_[todo_[t]].evaluated = true;
    }
    std::stable_sort(pop_.begin(), pop_.end(),
                     [](const Individual& a, const Individual& b) { return a.fitness > b.fitness; });
    if (++gen_ == options_.generations) {
        searching_ = false;
        return;
    }

    // Next generation: keep the best quarter, breed the rest by tournament selection.
    const int elite = std::max(1, options_.population / 4);
    auto tournament = [&]() -> const Army& {
        int best = std::uniform_int_distribution<int>(0, static_cast<int>(pop_.size()) - 1)(rng_);
        for (int r = 0; r < 2; ++r)
            best = std::min(best, std::uniform_int_distribution<int>(0, static_cast<int>(pop_.size()) - 1)(rng_));
        return pop_[best].army;  // sorted, so the lower index is fitter
    };
    std::vector<Individual> next(pop_.begin(), pop_.begin() + elite);
    while (static_cast<int>(next.size()) < options_.population) {
        const int kind = std::uniform_int_distribution<int>(0, 9)(rng_);
        Army child = kind < 3   ? arena_.mutate(arena_.crossover(tournament(), tournament(), rng_), rng_)
                     : kind < 9 ? arena_.mutate(tournament(), rng_)
                                : arena_.randomArmy(rng_);
        next.push_back({std::move(child)});
    }
    pop_ = std::move(next);
}

void LeagueTrainer::addBest(std::vector<MatchJob>& jobs) {
    const Individual& best = pop_[0];
    result_.gain = best.fitness;
    if (!(best.fitness > options_.minGain)) return;
    Army added = best.army;
    int found = 1;
    for (const auto& a : armies_) found += a.origin.rfind("found", 0) == 0;
    added.origin = "found " + std::to_string(found);
    armies_.push_back(std::move(added));
    const std::string key = armyKey(arena_, armies_.back());
    leagueIndex_.emplace(key, static_cast<int>(armies_.size()) - 1);
    // Its battles against the support were played during the search.
    const std::vector<double>& played = seen_.at(key);
    const int last = static_cast<int>(armies_.size()) - 1;
    extendJobs(jobs, [&](int i, int j, double& v) {
        if (i != last) return false;
        const auto s = std::find(support_.begin(), support_.end(), j);
        if (s == support_.end()) return false;
        v = played[s - support_.begin()];
        return true;
    });
    result_.added = true;
}

IterationResult LeagueTrainer::finishIteration() {
    if (result_.added) {
        // The search only saw the strongest members; this is the score against the whole mixture.
        result_.fullMix = expectedScore(m_, static_cast<int>(armies_.size()) - 1, meta_);
        meta_ = solveNash(m_);
    }
    pop_.clear();
    seen_.clear();
    return result_;
}

void computePayoffs(const std::vector<LeagueTrainer*>& leagues, int threads) {
    if (leagues.empty()) return;
    std::vector<MatchJob> jobs;
    for (auto* l : leagues) l->payoffJobs(jobs);
    playJobs(leagues[0]->arena(), jobs, threads);
    const int n = static_cast<int>(leagues.size());
    parallelFor(n, std::min(n, threads), [&](int i) { leagues[i]->finishPayoff(); });
}

std::vector<IterationResult> trainIteration(const std::vector<LeagueTrainer*>& leagues, const SearchOptions& options,
                                            int threads, const SearchProgress& progress) {
    // All leagues must share the arena of the first one (same battle settings).
    const int n = static_cast<int>(leagues.size());
    if (n == 0) return {};
    const Arena& arena = leagues[0]->arena();
    // The Nash solver runs once per league, on separate threads.
    parallelFor(n, std::min(n, threads), [&](int i) { leagues[i]->beginSearch(options); });
    std::vector<MatchJob> jobs;
    for (;;) {
        jobs.clear();
        bool any = false;
        for (auto* l : leagues)
            if (l->searching()) l->generationJobs(jobs), any = true;
        if (!any) break;
        playJobs(arena, jobs, threads);
        int gen = 0;
        for (auto* l : leagues)
            if (l->searching()) l->endGeneration(), gen = std::max(gen, l->generation());
        if (progress) progress(gen);
    }
    jobs.clear();
    for (auto* l : leagues) l->addBest(jobs);
    playJobs(arena, jobs, threads);
    std::vector<IterationResult> results(leagues.size());
    parallelFor(n, std::min(n, threads), [&](int i) { results[i] = leagues[i]->finishIteration(); });
    return results;
}

// ---------------------------------------------------------------------------
// Seeding with armies of older leagues
// ---------------------------------------------------------------------------
std::vector<Refit> fitToBudget(const Arena& arena, std::vector<Army>& armies, const std::vector<Army>& opponents,
                               const std::vector<double>& weights, int threads) {
    const int budget = arena.settings().budget;
    // Expected scores of `list` against the mixture, played in one batch.
    auto scores = [&](const std::vector<Army>& list) {
        Matrix m(list.size(), std::vector<double>(opponents.size(), 0));
        std::vector<MatchJob> jobs;
        for (std::size_t i = 0; i < list.size(); ++i)
            for (std::size_t j = 0; j < opponents.size(); ++j)
                if (weights[j] > 0) jobs.push_back({&list[i], &opponents[j], &m[i][j], nullptr});
        playJobs(arena, jobs, threads);
        std::vector<double> out;
        for (std::size_t i = 0; i < list.size(); ++i) out.push_back(expectedScore(m, static_cast<int>(i), weights));
        return out;
    };

    std::vector<Refit> fits;
    std::vector<Army> list;
    for (std::size_t i = 0; i < armies.size(); ++i)
        if (arena.cost(armies[i]) > budget) {
            fits.emplace_back();
            fits.back().army = static_cast<int>(i);
            list.push_back(armies[i]);
        }
    const auto before = scores(list);
    for (std::size_t k = 0; k < fits.size(); ++k) fits[k].before = fits[k].after = before[k];

    // Every round sells one piece of each army still over the budget.
    for (;;) {
        std::vector<Army> candidates;
        std::vector<std::pair<std::size_t, std::size_t>> sale;  // (refit, piece) of each candidate
        for (std::size_t k = 0; k < fits.size(); ++k) {
            const Army& a = armies[static_cast<std::size_t>(fits[k].army)];
            const int cost = arena.cost(a);
            if (cost <= budget) continue;
            bool any = false;
            for (const auto& p : a.pieces) any |= cost - arena.cost(p.item) <= budget;
            for (std::size_t p = 0; p < a.pieces.size(); ++p) {
                if (any && cost - arena.cost(a.pieces[p].item) > budget) continue;
                candidates.push_back(a);
                candidates.back().pieces.erase(candidates.back().pieces.begin() + static_cast<std::ptrdiff_t>(p));
                sale.emplace_back(k, p);
            }
        }
        if (candidates.empty()) break;
        const auto s = scores(candidates);
        std::vector<int> best(fits.size(), -1);
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            int& b = best[sale[c].first];
            if (b < 0 || s[c] >= s[static_cast<std::size_t>(b)]) b = static_cast<int>(c);  // ties: the later piece
        }
        for (std::size_t k = 0; k < fits.size(); ++k) {
            if (best[k] < 0) continue;
            const auto c = static_cast<std::size_t>(best[k]);
            Army& a = armies[static_cast<std::size_t>(fits[k].army)];
            const int item = a.pieces[sale[c].second].item;
            fits[k].sold.push_back(arena.itemId(item));
            fits[k].points += arena.cost(item);
            fits[k].after = s[c];
            a.pieces = std::move(candidates[c].pieces);
        }
    }
    return fits;
}

// ---------------------------------------------------------------------------
// Pool training
// ---------------------------------------------------------------------------
int Pool::weakest() const {
    int w = -1;
    for (int i = 0; i < size(); ++i)
        if (w < 0 || sum[static_cast<std::size_t>(i)] < sum[static_cast<std::size_t>(w)]) w = i;
    return w;
}

void Pool::remove(int i) {
    const auto at = static_cast<std::size_t>(i);
    for (std::size_t k = 0; k < payoff.size(); ++k) {
        sum[k] -= payoff[k][at];
        payoff[k].erase(payoff[k].begin() + static_cast<std::ptrdiff_t>(at));
    }
    armies.erase(armies.begin() + static_cast<std::ptrdiff_t>(at));
    payoff.erase(payoff.begin() + static_cast<std::ptrdiff_t>(at));
    sum.erase(sum.begin() + static_cast<std::ptrdiff_t>(at));
}

void Pool::add(Army army, const std::vector<float>& row) {
    double total = 0;
    for (std::size_t k = 0; k < payoff.size(); ++k) {
        payoff[k].push_back(-row[k]);
        sum[k] -= row[k];
        total += row[k];
    }
    armies.push_back(std::move(army));
    payoff.push_back(row);
    payoff.back().push_back(0);  // against itself
    sum.push_back(total);
}

long long fillPool(const Arena& arena, Pool& pool, std::vector<Army> armies, int threads) {
    const std::size_t n = armies.size();
    std::vector<std::vector<double>> m(n, std::vector<double>(n, 0));
    std::vector<MatchJob> jobs;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) jobs.push_back({&armies[i], &armies[j], &m[i][j], &m[j][i]});
    playJobs(arena, jobs, threads);
    pool.armies = std::move(armies);
    pool.payoff.assign(n, {});
    pool.sum.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        pool.payoff[i].assign(m[i].begin(), m[i].end());
        for (double v : m[i]) pool.sum[i] += v;
    }
    return static_cast<long long>(jobs.size());
}

namespace {

// `k` distinct pool indices other than `except`, uniformly at random (all of
// them, shuffled, if there are fewer).
std::vector<int> drawOpponents(int poolSize, int k, Rng& rng, int except = -1) {
    std::vector<int> all;
    for (int i = 0; i < poolSize; ++i)
        if (i != except) all.push_back(i);
    const int n = static_cast<int>(all.size());
    k = std::min(k, n);
    for (int i = 0; i < k; ++i)  // partial Fisher-Yates
        std::swap(all[static_cast<std::size_t>(i)],
                  all[static_cast<std::size_t>(std::uniform_int_distribution<int>(i, n - 1)(rng))]);
    all.resize(static_cast<std::size_t>(k));
    return all;
}

// Successive halving of `armies` against `opponents` (pool indices): all of
// them play opponents [0, stages[0]), the better half (by mean so far) plays
// on up to stages[1], and so on; the last stage is all of `opponents`.
// results[a][k] is army a's score against opponents[k] (NaN if not played),
// reached[a] the number of opponents it played. Returns the battles played.
long long halvingRace(const Arena& arena, const std::vector<const Army*>& armies, const std::vector<int>& opponents,
                      const std::vector<int>& stages, const Pool& pool, int threads,
                      std::vector<std::vector<double>>& results, std::vector<int>& reached) {
    const std::size_t m = armies.size(), total = opponents.size();
    results.assign(m, std::vector<double>(total, std::numeric_limits<double>::quiet_NaN()));
    reached.assign(m, 0);
    std::vector<double> sum(m, 0);
    std::vector<int> alive(m);
    for (std::size_t i = 0; i < m; ++i) alive[i] = static_cast<int>(i);
    std::vector<std::size_t> ends;
    for (int s : stages)
        if (s > 0 && static_cast<std::size_t>(s) < total && (ends.empty() || static_cast<std::size_t>(s) > ends.back()))
            ends.push_back(static_cast<std::size_t>(s));
    ends.push_back(total);
    long long matches = 0;
    std::size_t done = 0;
    for (std::size_t s = 0; s < ends.size(); ++s) {
        std::vector<MatchJob> jobs;
        for (int a : alive)
            for (std::size_t k = done; k < ends[s]; ++k)
                jobs.push_back({armies[static_cast<std::size_t>(a)], &pool.armies[static_cast<std::size_t>(opponents[k])],
                                &results[static_cast<std::size_t>(a)][k], nullptr});
        playJobs(arena, jobs, threads);
        matches += static_cast<long long>(jobs.size());
        for (int a : alive) {
            for (std::size_t k = done; k < ends[s]; ++k) sum[static_cast<std::size_t>(a)] += results[static_cast<std::size_t>(a)][k];
            reached[static_cast<std::size_t>(a)] = static_cast<int>(ends[s]);
        }
        done = ends[s];
        if (s + 1 < ends.size()) {
            std::stable_sort(alive.begin(), alive.end(), [&](int a, int b) { return sum[a] > sum[b]; });
            alive.resize(std::max<std::size_t>(1, alive.size() / 2));
        }
    }
    return matches;
}

double meanPlayed(const std::vector<double>& results, int reached) {
    double s = 0;
    for (int k = 0; k < reached; ++k) s += results[static_cast<std::size_t>(k)];
    return reached ? s / reached : 0;
}

// The z of a one-sided normal test at this confidence (e.g. 0.95 -> 1.645).
double normalQuantile(double confidence) {
    double lo = 0, hi = 10;
    for (int i = 0; i < 100; ++i) {
        const double z = (lo + hi) / 2;
        (0.5 * std::erfc(-z / std::sqrt(2.0)) < confidence ? lo : hi) = z;
    }
    return (lo + hi) / 2;
}

// Mean and standard deviation of x[0..k); the deviation is at least 0.2, so a
// few equal results do not count as certainty.
std::pair<double, double> meanSd(const std::vector<double>& x, std::size_t k) {
    double m = 0, v = 0;
    for (std::size_t i = 0; i < k; ++i) m += x[i];
    m /= static_cast<double>(k);
    for (std::size_t i = 0; i < k; ++i) v += (x[i] - m) * (x[i] - m);
    v /= static_cast<double>(std::max<std::size_t>(1, k - 1));
    return {m, std::max(0.2, std::sqrt(v))};
}

struct TestOutcome {
    std::vector<int> passed;               // candidates that played everyone
    std::vector<std::vector<double>> rows;  // every candidate's results, in the order of the opponents
    double best = -1e9;                    // best mean so far of those that dropped out
    int belowBar = 0, behind = 0;
    long long matches = 0;
};

// The sequential entrance test described at PoolOptions.
TestOutcome entranceTest(const Arena& arena, const std::vector<const Army*>& candidates,
                         const std::vector<int>& opponents, double bar, int keep, double confidence,
                         const Pool& pool, int threads) {
    TestOutcome out;
    const std::size_t total = opponents.size();
    const double z = normalQuantile(confidence);
    out.rows.assign(candidates.size(), std::vector<double>(total, 0));
    std::vector<int> alive(candidates.size());
    for (std::size_t i = 0; i < alive.size(); ++i) alive[i] = static_cast<int>(i);
    auto res = [&](int a) -> const std::vector<double>& { return out.rows[static_cast<std::size_t>(a)]; };
    std::size_t played = 0;
    bool decided = false;  // at most `keep` left and all surely above the bar: no more checks needed
    while (played < total && !alive.empty()) {
        const std::size_t upto =
            decided ? total
                    : std::min(total, played == 0 ? std::size_t{16} : played + std::max<std::size_t>(8, played / 4));
        std::vector<MatchJob> jobs;
        for (int a : alive)
            for (std::size_t k = played; k < upto; ++k)
                jobs.push_back({candidates[static_cast<std::size_t>(a)],
                                &pool.armies[static_cast<std::size_t>(opponents[k])],
                                &out.rows[static_cast<std::size_t>(a)][k], nullptr});
        playJobs(arena, jobs, threads);
        out.matches += static_cast<long long>(jobs.size());
        played = upto;
        if (played == total) break;

        // Standard error of the mean over all opponents per unit of deviation,
        // from the first `played` of them (finite-population correction).
        const double f = std::sqrt((1 - static_cast<double>(played) / static_cast<double>(total)) /
                                   static_cast<double>(played));
        std::vector<int> above;
        for (int a : alive) {
            const auto [m, sd] = meanSd(res(a), played);
            if (m + z * sd * f < bar) {
                ++out.belowBar;
                out.best = std::max(out.best, m);
            } else {
                above.push_back(a);
            }
        }
        // Behind: surely worse than `keep` others (paired on the same opponents).
        alive.clear();
        bool allSure = static_cast<int>(above.size()) <= keep;
        std::vector<double> d(played);
        for (int a : above) {
            int worse = 0;
            for (int b : above) {
                if (b == a || worse >= keep) continue;
                for (std::size_t k = 0; k < played; ++k) d[k] = res(a)[k] - res(b)[k];
                const auto [m, sd] = meanSd(d, played);
                worse += m + z * sd * f < 0;
            }
            const auto [m, sd] = meanSd(res(a), played);
            if (worse >= keep) {
                ++out.behind;
                out.best = std::max(out.best, m);
                continue;
            }
            alive.push_back(a);
            allSure = allSure && m - z * sd * f > bar;
        }
        decided = allSure && static_cast<int>(alive.size()) <= keep;
    }
    out.passed = alive;
    return out;
}

}  // namespace

PoolResult poolIteration(const Arena& arena, Pool& pool, const PoolOptions& options, Rng& rng, int threads,
                         const PoolProgress& progress) {
    PoolResult result;
    if (pool.armies.empty()) pool.add(arena.randomArmy(rng), {});
    const int n = pool.size();
    auto anyPool = [&]() -> const Army& {
        return pool.armies[static_cast<std::size_t>(std::uniform_int_distribution<int>(0, n - 1)(rng))];
    };

    // Starting population: pool armies, mutations and crossovers of them, fresh random armies.
    std::vector<Army> pop;
    while (static_cast<int>(pop.size()) < options.population / 4) pop.push_back(anyPool());
    while (static_cast<int>(pop.size()) < options.population) {
        const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
        pop.push_back(kind < 5   ? arena.mutate(anyPool(), rng)
                      : kind < 7 ? arena.mutate(arena.crossover(anyPool(), anyPool(), rng), rng)
                                 : arena.randomArmy(rng));
    }

    // Every round is judged on a new sample, the survivors included. With
    // halving, candidates that look bad after a few battles stop early; the
    // ranking puts those that got further first, then the higher mean.
    const std::vector<int> stages = options.halving.empty() ? std::vector<int>{options.sample} : options.halving;
    std::vector<double> fitness;
    for (int round = 1;; ++round) {
        const std::vector<int> sample = drawOpponents(n, stages.back(), rng);
        const std::size_t m = pop.size();
        std::vector<const Army*> armies;
        for (const Army& a : pop) armies.push_back(&a);
        std::vector<std::vector<double>> results;
        std::vector<int> reached;
        result.matches += halvingRace(arena, armies, sample, stages, pool, threads, results, reached);
        fitness.assign(m, 0);
        for (std::size_t i = 0; i < m; ++i) fitness[i] = meanPlayed(results[i], reached[i]);
        std::vector<int> order(m);
        for (std::size_t i = 0; i < m; ++i) order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return reached[a] != reached[b] ? reached[a] > reached[b] : fitness[a] > fitness[b];
        });
        std::vector<Army> sorted;
        std::vector<double> sortedFitness;
        for (int i : order) sorted.push_back(std::move(pop[static_cast<std::size_t>(i)])), sortedFitness.push_back(fitness[i]);
        pop = std::move(sorted);
        fitness = std::move(sortedFitness);
        if (progress) progress(round, fitness[0]);
        if (round >= options.search) break;

        // Next round: keep the best quarter, breed the rest by tournament selection.
        const int elite = std::max(1, options.population / 4);
        auto tournament = [&]() -> const Army& {
            int best = std::uniform_int_distribution<int>(0, static_cast<int>(pop.size()) - 1)(rng);
            for (int r = 0; r < 2; ++r)
                best = std::min(best, std::uniform_int_distribution<int>(0, static_cast<int>(pop.size()) - 1)(rng));
            return pop[static_cast<std::size_t>(best)];
        };
        std::vector<Army> next(pop.begin(), pop.begin() + elite);
        while (static_cast<int>(next.size()) < options.population) {
            const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
            next.push_back(kind < 3   ? arena.mutate(arena.crossover(tournament(), tournament(), rng), rng)
                           : kind < 9 ? arena.mutate(tournament(), rng)
                                      : arena.randomArmy(rng));
        }
        pop = std::move(next);
    }
    result.fitness = fitness[0];

    // The candidates: the new armies of the last round, best first.
    std::unordered_set<std::string> inPool;
    for (const Army& a : pool.armies) inPool.insert(armyKey(arena, a));
    std::vector<const Army*> candidates;
    for (const Army& a : pop)
        if (inPool.insert(armyKey(arena, a)).second) candidates.push_back(&a);
    result.tested = static_cast<int>(candidates.size());
    if (candidates.empty()) return result;

    // The entrance test against the whole pool.
    const int keep = std::max(1, options.maxEntrants);
    const std::vector<int> opponents = drawOpponents(n, n, rng);
    TestOutcome test =
        entranceTest(arena, candidates, opponents, options.minGain, keep, options.confidence, pool, threads);
    result.testMatches = test.matches;
    result.belowBar = test.belowBar;
    result.behind = test.behind;
    result.best = test.best;

    // Those that passed join best first. rows[c][j]: score against pool army j,
    // kept in step with the pool as armies leave and join.
    struct Passed {
        int candidate;
        std::vector<float> row;
        double total = 0;
    };
    std::vector<Passed> passed;
    for (int c : test.passed) {
        Passed p{c, std::vector<float>(static_cast<std::size_t>(n), 0)};
        for (std::size_t k = 0; k < opponents.size(); ++k) {
            p.row[static_cast<std::size_t>(opponents[k])] = static_cast<float>(test.rows[static_cast<std::size_t>(c)][k]);
            p.total += test.rows[static_cast<std::size_t>(c)][k];
        }
        result.best = std::max(result.best, p.total / n);
        passed.push_back(std::move(p));
    }
    std::stable_sort(passed.begin(), passed.end(), [](const Passed& a, const Passed& b) { return a.total > b.total; });
    for (std::size_t i = 0; i < passed.size(); ++i) {
        Passed& p = passed[i];
        if (static_cast<int>(result.entries.size()) >= keep) {
            ++result.left;
            continue;
        }
        const int size = pool.size();
        double score = 0;
        for (float v : p.row) score += v;
        score /= size;
        if (!(score > options.minGain)) {  // the armies that joined before it beat it
            ++result.belowBar;
            continue;
        }
        // A copy: its results against the pool correlate with a pool army's.
        double similar = -1;
        if (size >= 3 && options.maxSimilar <= 1) {
            for (int j = 0; j < size; ++j) {
                const std::vector<float>& q = pool.payoff[static_cast<std::size_t>(j)];
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                for (int k = 0; k < size; ++k) {
                    if (k == j) continue;
                    const double x = p.row[static_cast<std::size_t>(k)], y = q[static_cast<std::size_t>(k)];
                    sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y;
                }
                const double m = size - 1;
                const double vx = sxx - sx * sx / m, vy = syy - sy * sy / m;
                if (vx > 1e-12 && vy > 1e-12) similar = std::max(similar, (sxy - sx * sy / m) / std::sqrt(vx * vy));
            }
            if (similar > options.maxSimilar) {
                ++result.copies;
                continue;
            }
        }
        PoolEntry entry;
        entry.score = score;
        entry.similar = similar;
        if (size >= pool.capacity) {
            const int w = pool.weakest();
            entry.removed = pool.armies[static_cast<std::size_t>(w)].origin;
            pool.remove(w);
            ++pool.replaced;
            for (std::size_t r = i; r < passed.size(); ++r) passed[r].row.erase(passed[r].row.begin() + w);
        }
        Army entrant = *candidates[static_cast<std::size_t>(p.candidate)];
        entrant.origin = entry.origin = "pool " + std::to_string(pool.nextId++);
        pool.add(std::move(entrant), p.row);
        result.entries.push_back(entry);

        // The candidates still waiting play the new army.
        if (static_cast<int>(result.entries.size()) < keep && i + 1 < passed.size()) {
            const std::size_t rest = passed.size() - i - 1;
            std::vector<double> scores(rest);
            std::vector<MatchJob> jobs;
            for (std::size_t r = 0; r < rest; ++r)
                jobs.push_back({candidates[static_cast<std::size_t>(passed[i + 1 + r].candidate)], &pool.armies.back(),
                                &scores[r], nullptr});
            playJobs(arena, jobs, threads);
            result.testMatches += static_cast<long long>(jobs.size());
            for (std::size_t r = 0; r < rest; ++r) passed[i + 1 + r].row.push_back(static_cast<float>(scores[r]));
        }
    }
    result.matches += result.testMatches;
    return result;
}

// ---------------------------------------------------------------------------
// Twin training
// ---------------------------------------------------------------------------
void CrossMatrix::extend(const std::vector<Army>& a, const std::vector<Army>& b, std::vector<MatchJob>& jobs) {
    const std::size_t oldRows = c_.size(), oldCols = cols_;
    c_.resize(b.size());
    for (auto& row : c_) row.resize(a.size(), 0);
    cols_ = a.size();
    for (std::size_t i = 0; i < b.size(); ++i)
        for (std::size_t j = i < oldRows ? oldCols : 0; j < a.size(); ++j)
            jobs.push_back({&b[i], &a[j], &c_[i][j], nullptr});
}

CrossScore crossScore(const Matrix& c, const std::vector<double>& metaA, const std::vector<double>& metaB) {
    CrossScore r;
    if (c.empty() || c[0].empty()) return r;
    const std::size_t nb = c.size(), na = c[0].size();
    r.xA = r.xB = -std::numeric_limits<double>::infinity();
    for (std::size_t b = 0; b < nb; ++b) {
        double v = 0;
        for (std::size_t a = 0; a < na && a < metaA.size(); ++a) v += c[b][a] * metaA[a];
        if (v > r.xA) r.xA = v, r.bestB = static_cast<int>(b);
    }
    for (std::size_t a = 0; a < na; ++a) {
        double v = 0;
        for (std::size_t b = 0; b < nb && b < metaB.size(); ++b) v -= c[b][a] * metaB[b];
        if (v > r.xB) r.xB = v, r.bestA = static_cast<int>(a);
    }
    return r;
}

UnionLeague unionLeague(const Arena& arena, const std::vector<Army>& a, const Matrix& ma, const std::vector<Army>& b,
                        const Matrix& mb, const Matrix& cross) {
    UnionLeague u;
    std::unordered_map<std::string, int> index;
    for (std::size_t i = 0; i < a.size(); ++i) {
        index.emplace(armyKey(arena, a[i]), static_cast<int>(i));
        u.armies.push_back(a[i]);
        u.armies.back().origin += " (A)";
    }
    std::vector<int> ub(b.size());  // index of B's armies in the union
    std::vector<bool> fromA(b.size(), false);
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto it = index.find(armyKey(arena, b[i]));
        if (it != index.end()) {
            ub[i] = it->second;
            fromA[i] = true;
            continue;
        }
        ub[i] = static_cast<int>(u.armies.size());
        u.armies.push_back(b[i]);
        u.armies.back().origin += " (B)";
    }
    const std::size_t n = u.armies.size();
    u.payoff.assign(n, std::vector<double>(n, 0));
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < a.size(); ++j) u.payoff[i][j] = ma[i][j];
    for (std::size_t i = 0; i < b.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j)
            if (!(fromA[i] && fromA[j])) u.payoff[ub[i]][ub[j]] = mb[i][j];
        if (fromA[i]) continue;
        for (std::size_t j = 0; j < a.size(); ++j) {
            u.payoff[ub[i]][j] = cross[i][j];
            u.payoff[j][ub[i]] = -cross[i][j];
        }
    }
    u.meta = solveNash(u.payoff);
    return u;
}

void plateauState(const std::vector<Evaluation>& history, const StopRules& rules, double& best, int& stale) {
    best = std::numeric_limits<double>::infinity();
    stale = 0;
    for (std::size_t t = 0; t < history.size(); ++t) {
        const std::size_t k = std::min<std::size_t>(StopRules::smoothing, t + 1);
        double v = 0;
        for (std::size_t s = t + 1 - k; s <= t; ++s) v += std::max(history[s].xA, history[s].xB);
        v /= static_cast<double>(k);
        if (v < best - rules.delta) {
            best = v;
            stale = 0;
        } else {
            ++stale;
        }
    }
}

std::string checkStop(const std::vector<Evaluation>& history, const StopRules& rules) {
    if (history.empty()) return "";
    const Evaluation& e = history.back();
    char buf[200];
    if (e.xA < rules.eps && e.xB < rules.eps) {
        std::snprintf(buf, sizeof buf, "converged: xA %+.3f and xB %+.3f are below eps %.3f", e.xA, e.xB, rules.eps);
        return buf;
    }
    double best;
    int stale;
    plateauState(history, rules, best, stale);
    if (stale >= std::max(1, rules.patience)) {
        std::snprintf(buf, sizeof buf,
                      "plateau: max(xA, xB) (mean of the last %d evaluations) has not dropped below %.3f - %.3f "
                      "for %d evaluations",
                      StopRules::smoothing, best, rules.delta, stale);
        return buf;
    }
    return "";
}

std::string evaluationsCsv(const std::vector<Evaluation>& history) {
    std::ostringstream out;
    out << "iteration,gainA,gainB,xA,xB,leagueA,leagueB,supportA,supportB,supportUnion,seconds\n";
    char buf[256];
    for (const auto& e : history) {
        std::snprintf(buf, sizeof buf, "%d,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%.1f\n", e.iteration, e.gainA, e.gainB,
                      e.xA, e.xB, e.leagueA, e.leagueB, e.supportA, e.supportB, e.supportUnion, e.seconds);
        out << buf;
    }
    return out.str();
}

bool parseEvaluationsCsv(const std::string& text, std::vector<Evaluation>& history) {
    history.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.rfind("iteration", 0) == 0) continue;
        Evaluation e;
        if (std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%d,%lf", &e.iteration, &e.gainA, &e.gainB, &e.xA,
                        &e.xB, &e.leagueA, &e.leagueB, &e.supportA, &e.supportB, &e.supportUnion, &e.seconds) != 11)
            return false;
        history.push_back(e);
    }
    return true;
}

}  // namespace arena
