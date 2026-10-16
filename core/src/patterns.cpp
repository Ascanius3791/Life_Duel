#include "gol/patterns.hpp"

#include <algorithm>
#include <cctype>
#include <climits>

namespace gol {

std::vector<Point> parseRle(std::string_view rle) {
    std::vector<Point> cells;
    int x = 0;
    int y = 0;
    int count = 0;
    bool lineStart = true;

    for (std::size_t i = 0; i < rle.size(); ++i) {
        const char c = rle[i];
        if (lineStart && (c == '#' || c == 'x')) {  // comment or header line
            while (i < rle.size() && rle[i] != '\n') ++i;
            continue;
        }
        lineStart = c == '\n';
        if (std::isdigit(static_cast<unsigned char>(c))) {
            count = count * 10 + (c - '0');
            continue;
        }
        const int n = count == 0 ? 1 : count;
        count = 0;
        if (c == 'b' || c == '.') {
            x += n;
        } else if (c == 'o' || c == 'A') {
            for (int k = 0; k < n; ++k) cells.push_back({x++, y});
        } else if (c == '$') {
            y += n;
            x = 0;
        } else if (c == '!') {
            break;
        }
    }
    return cells;
}

namespace {

void normalise(Pattern& p) {
    if (p.cells.empty()) {
        p.width = p.height = 0;
        return;
    }
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const auto& c : p.cells) {
        minX = std::min(minX, c.x);
        minY = std::min(minY, c.y);
        maxX = std::max(maxX, c.x);
        maxY = std::max(maxY, c.y);
    }
    for (auto& c : p.cells) {
        c.x -= minX;
        c.y -= minY;
    }
    std::sort(p.cells.begin(), p.cells.end());
    p.width = maxX - minX + 1;
    p.height = maxY - minY + 1;
}

Pattern make(std::string id, std::string name, std::string category, std::string description,
             std::string_view rle) {
    Pattern p{std::move(id), std::move(name), std::move(category), std::move(description), 0, 0, parseRle(rle)};
    normalise(p);
    return p;
}

}  // namespace

Pattern transformed(const Pattern& pattern, int quarterTurns, bool mirror) {
    Pattern p = pattern;
    quarterTurns = ((quarterTurns % 4) + 4) % 4;
    for (int t = 0; t < quarterTurns; ++t) {
        for (auto& c : p.cells) c = {-c.y, c.x};  // 90° clockwise (y axis points down)
    }
    if (mirror) {
        for (auto& c : p.cells) c.x = -c.x;
    }
    normalise(p);
    return p;
}

namespace {
// All moving patterns in the library are stored so that they travel (or shoot)
// towards the east, which is the direction the red player attacks.
Pattern facingEast(const Pattern& p, int quarterTurns, bool mirror) { return transformed(p, quarterTurns, mirror); }
}  // namespace

const std::vector<Pattern>& patternLibrary() {
    static const std::vector<Pattern> library = {
        make("cell", "Single cell", "Basic", "One living cell. Dies alone, but useful for drawing.", "o!"),

        // Still lifes
        make("block", "Block", "Still life", "The most common still life. Great as a cheap shield.", "2o$2o!"),
        make("beehive", "Beehive", "Still life", "Six cells that never change.", "b2o$o2bo$b2o!"),
        make("loaf", "Loaf", "Still life", "A seven cell still life.", "b2o$o2bo$bobo$2bo!"),
        make("boat", "Boat", "Still life", "Five cells, stable.", "2o$obo$bo!"),
        make("tub", "Tub", "Still life", "Four cells around an empty centre.", "bo$obo$bo!"),
        make("eater1", "Eater 1", "Still life",
             "The fishhook: eats gliders that hit it from the right side and survives.", "2o2b$obob$2bob$2b2o!"),

        // Oscillators
        make("blinker", "Blinker", "Oscillator", "Period 2. The smallest oscillator.", "3o!"),
        make("toad", "Toad", "Oscillator", "Period 2.", "b3o$3o!"),
        make("beacon", "Beacon", "Oscillator", "Period 2, two blocks that blink at their corners.",
             "2o2b$2o2b$2b2o$2b2o!"),
        make("pulsar", "Pulsar", "Oscillator", "Period 3, the famous symmetric oscillator.",
             "2b3o3b3o2b2$o4bobo4bo$o4bobo4bo$o4bobo4bo$2b3o3b3o2b2$2b3o3b3o2b$o4bobo4bo$o4bobo4bo$"
             "o4bobo4bo2$2b3o3b3o!"),
        make("pentadecathlon", "Pentadecathlon", "Oscillator", "Period 15. Emits sparks that can reflect gliders.",
             "2bo4bo2b$2ob4ob2o$2bo4bo!"),

        // Spaceships
        make("glider", "Glider", "Spaceship", "Travels diagonally, one cell every 4 generations.", "bo$2bo$3o!"),
        facingEast(make("lwss", "Lightweight spaceship", "Spaceship", "LWSS: flies orthogonally at c/2.",
             "bo2bo$o4b$o3bo$4o!"), 0, true),
        facingEast(make("mwss", "Middleweight spaceship", "Spaceship", "MWSS: flies orthogonally at c/2.",
             "3bo2b$bo3bo$o5b$o4bo$5o!"), 0, true),
        facingEast(make("hwss", "Heavyweight spaceship", "Spaceship", "HWSS: flies orthogonally at c/2.",
             "3b2o2b$bo4bo$o6b$o5bo$6o!"), 0, true),
        facingEast(make("copperhead", "Copperhead", "Spaceship", "A slow (c/10) but sturdy orthogonal spaceship.",
             "b2o2b2o$3b2o$3b2o$obo2bobo$o6bo2$o6bo$b2o2b2o$2b4o2$3b2o$3b2o!"), 1, false),

        // Methuselahs
        make("r_pentomino", "R-pentomino", "Methuselah", "Five cells that keep exploding for 1103 generations.",
             "b2o$2o$bo!"),
        make("diehard", "Diehard", "Methuselah", "Vanishes completely after 130 generations.",
             "6bob$2o6b$bo3b3o!"),
        make("acorn", "Acorn", "Methuselah", "Seven cells that grow for 5206 generations.", "bo5b$3bo3b$2o2b3o!"),
        make("thunderbird", "Thunderbird", "Methuselah", "Stabilises after 243 generations.", "3o2$bo$bo$bo!"),

        // Guns
        make("gosper_gun", "Gosper glider gun", "Gun", "The first known gun: one glider every 30 generations.",
             "24bo11b$22bobo11b$12b2o6b2o12b2o$11bo3bo4b2o12b2o$2o8bo5bo3b2o14b$2o8bo3bob2o4bobo11b$"
             "10bo5bo7bo11b$11bo3bo20b$12b2o!"),
        facingEast(make("simkin_gun", "Simkin glider gun", "Gun", "Small gun: one glider every 120 generations.",
             "2o5b2o$2o5b2o2$4b2o$4b2o5$22b2ob2o$21bo5bo$21bo6bo2b2o$21b3o3bo3b2o$26bo4$20b2o$20bo$"
             "21b3o$23bo!"), 2, false),
    };
    return library;
}

const Pattern* findPattern(std::string_view id) {
    for (const auto& p : patternLibrary()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const std::vector<ArsenalItem>& battleArsenal() {
    static const std::vector<ArsenalItem> arsenal = {
        {"cell", 1},          {"block", 3},       {"beehive", 4},  {"eater1", 6},
        {"blinker", 3},       {"pulsar", 16},     {"pentadecathlon", 12},
        {"glider", 5},        {"lwss", 9},        {"mwss", 12},    {"hwss", 15},
        {"copperhead", 18},   {"r_pentomino", 8}, {"diehard", 10}, {"acorn", 12},
        {"gosper_gun", 40},   {"simkin_gun", 40},
    };
    return arsenal;
}

}  // namespace gol
