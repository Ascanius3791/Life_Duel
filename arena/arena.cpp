#include "arena.hpp"

#include "gol/fast_battle.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <sstream>
#include <thread>

namespace arena {

namespace {

int pick(Rng& rng, int lo, int hi) {  // inclusive range
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}
bool chance(Rng& rng, double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

}  // namespace

Arena::Arena(Settings settings)
    : settings_(std::move(settings)),
      zones_(gol::zonesFor(settings_.width)),
      grey_(gol::makeBattlefield(settings_.preset, settings_.width, settings_.height, settings_.count,
                                 settings_.fieldSeed)),
      arsenal_(gol::battleArsenal()) {
    const int w = settings_.width, h = settings_.height;
    auto at = [&](int x, int y) { return grey_[static_cast<std::size_t>(y) * w + x]; };
    bool mirrored = true, rotated = true;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (at(x, y) != at(w - 1 - x, y)) mirrored = false;
            if (at(x, y) != at(w - 1 - x, h - 1 - y)) rotated = false;
        }
    symmetry_ = mirrored ? Symmetry::Mirror : rotated ? Symmetry::Rotation : Symmetry::None;

    for (const auto& item : arsenal_) {
        const gol::Pattern* p = gol::findPattern(item.patternId);
        for (int rot = 0; rot < 4; ++rot)
            for (int m = 0; m < 2; ++m) shapes_.push_back(gol::transformed(*p, rot, m == 1));
    }
}

const gol::Pattern& Arena::shape(int item, int rot, bool mirror) const {
    return shapes_[static_cast<std::size_t>(item) * 8 + (rot & 3) * 2 + (mirror ? 1 : 0)];
}

int Arena::findItem(const std::string& patternId) const {
    for (int i = 0; i < itemCount(); ++i)
        if (arsenal_[i].patternId == patternId) return i;
    return -1;
}

int Arena::cost(const Army& army) const {
    int sum = 0;
    for (const auto& p : army.pieces) sum += cost(p.item);
    return sum;
}

bool Arena::legal(const Army& army) const {
    if (cost(army) > settings_.budget) return false;
    for (const auto& p : army.pieces) {
        const auto& s = shape(p);
        if (p.x < 0 || p.y < 0 || p.x + s.width > zones_.leftEnd || p.y + s.height > settings_.height) return false;
    }
    return true;
}

void Arena::clampInto(Piece& p) const {
    const auto& s = shape(p);
    p.x = std::max(0, std::min(zones_.leftEnd - s.width, p.x));
    p.y = std::max(0, std::min(settings_.height - s.height, p.y));
}

void Arena::repair(Army& army, Rng& rng) const {
    auto& ps = army.pieces;
    for (auto& p : ps) clampInto(p);
    ps.erase(std::remove_if(ps.begin(), ps.end(),
                            [&](const Piece& p) {
                                const auto& s = shape(p);
                                return s.width > zones_.leftEnd || s.height > settings_.height;
                            }),
             ps.end());
    while (!ps.empty() && cost(army) > settings_.budget)
        ps.erase(ps.begin() + pick(rng, 0, static_cast<int>(ps.size()) - 1));
}

Piece Arena::randomPiece(int item, Rng& rng) const {
    Piece p;
    p.item = item;
    p.rot = pick(rng, 0, 3);
    p.mirror = chance(rng, 0.5);
    const auto& s = shape(p);
    p.x = pick(rng, 0, std::max(0, zones_.leftEnd - s.width));
    p.y = pick(rng, 0, std::max(0, settings_.height - s.height));
    return p;
}

Army Arena::randomArmy(Rng& rng) const {
    Army army;
    army.origin = "random";
    // Spend between 60% and 100% of the budget on uniformly chosen items.
    const int keep = static_cast<int>(settings_.budget * std::uniform_real_distribution<double>(0, 0.4)(rng));
    int left = settings_.budget;
    while (left > keep) {
        std::vector<int> affordable;
        for (int i = 0; i < itemCount(); ++i)
            if (cost(i) <= left) affordable.push_back(i);
        if (affordable.empty()) break;
        const int item = affordable[pick(rng, 0, static_cast<int>(affordable.size()) - 1)];
        army.pieces.push_back(randomPiece(item, rng));
        left -= cost(item);
    }
    repair(army, rng);
    return army;
}

Army Arena::mutate(const Army& source, Rng& rng) const {
    Army army = source;
    auto& ps = army.pieces;
    int ops = 1;
    while (ops < 4 && chance(rng, 0.5)) ++ops;

    for (int op = 0; op < ops; ++op) {
        const int left = settings_.budget - cost(army);
        const int roll = ps.empty() ? 70 : pick(rng, 0, 99);  // an empty army can only buy
        Piece* p = ps.empty() ? nullptr : &ps[pick(rng, 0, static_cast<int>(ps.size()) - 1)];

        if (roll < 30) {  // nudge a piece a few cells
            int dx = 0, dy = 0;
            while (dx == 0 && dy == 0) {
                dx = pick(rng, -3, 3);
                dy = pick(rng, -3, 3);
            }
            p->x += dx;
            p->y += dy;
        } else if (roll < 40) {  // move a piece anywhere
            const Piece q = randomPiece(p->item, rng);
            p->x = q.x;
            p->y = q.y;
        } else if (roll < 55) {  // turn or mirror a piece around its centre
            const auto& before = shape(*p);
            const int cx = p->x + before.width / 2, cy = p->y + before.height / 2;
            if (chance(rng, 0.7)) p->rot = (p->rot + pick(rng, 1, 3)) % 4;
            else p->mirror = !p->mirror;
            const auto& after = shape(*p);
            p->x = cx - after.width / 2;
            p->y = cy - after.height / 2;
        } else if (roll < 65) {  // replace a piece by another affordable one at the same spot
            std::vector<int> options;
            for (int i = 0; i < itemCount(); ++i)
                if (i != p->item && cost(i) <= left + cost(p->item)) options.push_back(i);
            if (!options.empty()) {
                const auto& before = shape(*p);
                const int cx = p->x + before.width / 2, cy = p->y + before.height / 2;
                p->item = options[pick(rng, 0, static_cast<int>(options.size()) - 1)];
                const auto& after = shape(*p);
                p->x = cx - after.width / 2;
                p->y = cy - after.height / 2;
            }
        } else if (roll < 80) {  // buy a new piece
            std::vector<int> affordable;
            for (int i = 0; i < itemCount(); ++i)
                if (cost(i) <= left) affordable.push_back(i);
            if (!affordable.empty()) {
                const Piece q = randomPiece(affordable[pick(rng, 0, static_cast<int>(affordable.size()) - 1)], rng);
                p = nullptr;  // the push may move the pieces
                ps.push_back(q);
            }
        } else if (roll < 90) {  // sell a piece
            ps.erase(ps.begin() + (p - ps.data()));
        } else if (roll < 95) {  // copy a piece next to itself
            if (cost(p->item) <= left) {
                Piece q = *p;
                const auto& s = shape(q);
                if (chance(rng, 0.5)) q.y += (chance(rng, 0.5) ? 1 : -1) * (s.height + pick(rng, 1, 6));
                else q.x += (chance(rng, 0.5) ? 1 : -1) * (s.width + pick(rng, 1, 6));
                p = nullptr;
                ps.push_back(q);
            }
        } else {  // shift the whole army up or down
            const int dy = pick(rng, -8, 8);
            for (auto& q : ps) q.y += dy;
        }
        for (auto& q : ps) clampInto(q);
    }
    repair(army, rng);
    return army;
}

Army Arena::crossover(const Army& a, const Army& b, Rng& rng) const {
    Army child;
    const int cut = pick(rng, 1, settings_.height - 1);
    auto centreY = [&](const Piece& p) { return p.y + shape(p).height / 2; };
    for (const auto& p : a.pieces)
        if (centreY(p) < cut) child.pieces.push_back(p);
    for (const auto& p : b.pieces)
        if (centreY(p) >= cut) child.pieces.push_back(p);
    repair(child, rng);
    return child;
}

Piece Arena::toBlue(const Piece& p) const {
    const auto& s = shape(p);
    Piece q = p;
    q.x = settings_.width - p.x - s.width;
    if (symmetry_ == Symmetry::Rotation) {
        q.rot = (p.rot + 2) % 4;  // a half turn commutes with the mirror
        q.y = settings_.height - p.y - s.height;
    } else {
        q.mirror = !p.mirror;
    }
    return q;
}

std::vector<std::uint8_t> Arena::board(const Army& red, const Army& blue) const {
    const int w = settings_.width;
    std::vector<std::uint8_t> cells = grey_;
    auto put = [&](const Piece& p, gol::Cell colour) {
        for (const auto& c : shape(p).cells) {
            const int x = p.x + c.x, y = p.y + c.y;
            if (x >= 0 && x < w && y >= 0 && y < settings_.height) cells[static_cast<std::size_t>(y) * w + x] = colour;
        }
    };
    for (const auto& p : red.pieces) put(p, gol::Red);
    for (const auto& p : blue.pieces) put(toBlue(p), gol::Blue);
    return cells;
}

MatchResult Arena::play(const Army& red, const Army& blue) const {
    const gol::BattleConfig config{settings_.width, settings_.height, settings_.variant, settings_.maxGenerations};
    gol::FastBattle battle(config, board(red, blue));
    battle.runToEnd();
    MatchResult r;
    r.winner = battle.winner();
    r.reason = battle.endReason();
    r.generation = battle.generation();
    r.redScore = battle.redScore();
    r.blueScore = battle.blueScore();
    const double outcome = r.winner == gol::Winner::Red ? 1 : r.winner == gol::Winner::Blue ? -1 : 0;
    const double total = r.redScore + r.blueScore;
    const double margin = total > 0 ? (r.redScore - r.blueScore) / total : 0;
    r.score = 0.7 * outcome + 0.3 * margin;
    return r;
}

double Arena::payoff(const Army& a, const Army& b) const {
    if (symmetry_ != Symmetry::None) return play(a, b).score;
    return (play(a, b).score - play(b, a).score) / 2;
}

std::string formatPieces(const Arena& arena, const Army& army) {
    std::string out;
    for (const auto& p : army.pieces) {
        if (!out.empty()) out += ';';
        out += arena.itemId(p.item) + '@' + std::to_string(p.x) + ',' + std::to_string(p.y) + ",r" +
               std::to_string(p.rot) + ",m" + (p.mirror ? "1" : "0");
    }
    return out;
}

std::string armyKey(const Arena& arena, const Army& army) {
    std::vector<std::string> tokens;
    for (const auto& p : army.pieces) {
        Army one;
        one.pieces.push_back(p);
        tokens.push_back(formatPieces(arena, one));
    }
    std::sort(tokens.begin(), tokens.end());
    std::string key;
    for (const auto& t : tokens) key += t + ';';
    return key;
}

bool parsePieces(const Arena& arena, const std::string& text, Army& army) {
    army.pieces.clear();
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ';')) {
        if (token.empty()) continue;
        const auto at = token.find('@');
        if (at == std::string::npos) return false;
        Piece p;
        p.item = arena.findItem(token.substr(0, at));
        int mirror = 0;
        if (p.item < 0 ||
            std::sscanf(token.c_str() + at + 1, "%d,%d,r%d,m%d", &p.x, &p.y, &p.rot, &mirror) != 4)
            return false;
        p.rot &= 3;
        p.mirror = mirror != 0;
        army.pieces.push_back(p);
    }
    return true;
}

std::vector<double> solveNash(const std::vector<std::vector<double>>& m, int iterations) {
    const std::size_t n = m.size();
    if (n == 0) return {};
    std::vector<double> regret(n, 0), x(n), average(n, 0), utility(n);
    for (int t = 1; t <= iterations; ++t) {
        double positive = 0;
        for (double r : regret) positive += r;
        for (std::size_t i = 0; i < n; ++i) x[i] = positive > 0 ? regret[i] / positive : 1.0 / n;
        // In a symmetric zero-sum game self-play is the same as two players
        // running identical regret matching, so the average converges to Nash.
        double value = 0;
        for (std::size_t i = 0; i < n; ++i) {
            double u = 0;
            for (std::size_t j = 0; j < n; ++j) u += m[i][j] * x[j];
            utility[i] = u;
            value += x[i] * u;
        }
        for (std::size_t i = 0; i < n; ++i) {
            regret[i] = std::max(0.0, regret[i] + utility[i] - value);
            average[i] += t * x[i];
        }
    }
    // Normalise, then drop negligible weights (regret matching leaves a little
    // probability on everything early on).
    for (int pass = 0; pass < 2; ++pass) {
        double sum = 0;
        for (double a : average) sum += a;
        for (auto& a : average) {
            a = sum > 0 ? a / sum : 1.0 / n;
            if (pass == 0 && a < 0.005) a = 0;
        }
    }
    return average;
}

void parallelFor(int n, int threads, const std::function<void(int)>& fn) {
    threads = std::max(1, std::min(threads, n));
    std::atomic<int> next{0};
    auto worker = [&] {
        for (int i = next++; i < n; i = next++) fn(i);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
}

const char* variantName(gol::Variant v) { return v == gol::Variant::Contagion ? "contagion" : "birth"; }

bool parseVariant(const std::string& s, gol::Variant& v) {
    if (s == "birth") v = gol::Variant::BirthOnly;
    else if (s == "contagion") v = gol::Variant::Contagion;
    else return false;
    return true;
}

}  // namespace arena
