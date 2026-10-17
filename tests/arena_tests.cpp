// Tests for the bot-training building blocks.
#include "arena.hpp"
#include "league.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace arena;

int main() {
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

    if (failures == 0) std::printf("All arena tests passed.\n");
    return failures == 0 ? 0 : 1;
}
