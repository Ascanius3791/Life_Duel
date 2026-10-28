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

Pattern advanced(const Pattern& pattern, int generations) {
    Pattern p = pattern;
    normalise(p);
    for (int g = 0; g < generations && !p.cells.empty(); ++g) {
        // Dense neighbour counts over the bounding box plus a one cell margin:
        // nothing can be born further out. Cell (x, y) sits at (x + 1, y + 1).
        const int w = p.width + 2, h = p.height + 2;
        std::vector<unsigned char> alive(static_cast<std::size_t>(w) * h, 0), count(alive.size(), 0);
        for (const auto& c : p.cells) {
            const int cx = c.x + 1, cy = c.y + 1;
            alive[static_cast<std::size_t>(cy) * w + cx] = 1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) ++count[static_cast<std::size_t>(cy + dy) * w + cx + dx];
        }
        std::vector<Point> next;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (count[i] == 3 || (count[i] == 2 && alive[i])) next.push_back({x, y});
            }
        p.cells = std::move(next);
        normalise(p);
    }
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
        make("self_assembling_gun", "Self-assembling glider gun", "Gun",
             "21 cells that build a Gosper glider gun by themselves (its right block sits one cell lower) within 40 "
             "generations. From then on one glider every 30 generations; the first one leaves after about 60.",
             "26bo$8bobo16b2ob2o2bo$2o7bo17bobo4b2o$bo4b2o$8b3o!"),
        make("mwss_gun", "MWSS gun (p165)", "Gun",
             "Gliders always fly diagonally; this gun shoots middleweight spaceships straight east instead, one "
             "every 165 generations, made by a collision of three gliders. 563 cells, too big for Life Duel. "
             "By Dietrich Leithner, David Bell and Jason Summers (2002).",
             "125b3o$125bobo$125b3o6bo$125b3o5b3o$125b3o$125b3o$125bobo5b3o$125b3o$133bobo$133bobo$$133b3o$111boo6"
             "boo$110bobbo4bobbo$110bobbo4bobbo11b3o$110bobbo4bobbo12bo$111boo6boo$$137bobboo4boobbo$136bo3b3obb3o"
             "3bo$137bobboo4boobbo7$139bo$139b3o4bobobbobo$138bob4obobbobbobboboo$120bobo23bobobbobo$120boo$121bo$"
             "139b3o$139b3o$44b3o93bo$45bo94bo$45bo94bo$44b3o92bobo$$44b3o$44b3o92bobo$140bo$44b3o93bo$45bo94bo$45"
             "bo93b3o$44b3o92b3o$$51boo6boo$49bo4bobbo4bo$49bo4bobbo4bo$49bo4bobbo4bo4bo$51boo6boo5b3o8bo$65bobobo"
             "6b3o$65bobobo5bobobo$66b3o6bobobo$67bo8b3o$77bo$$67bo$66b3o8bo$17bo47bobobo6b3o$16b3o46bobobo5bobobo"
             "$15bobobo46b3o6bobobo$15bobobo47bo8b3o$16b3o12bo45bo$17bo11boo$30boo$$17bo79b3o$16b3o80bo$15bobobo59"
             "bo18bo$15bobobo59bobo$16b3o49bo10boo$17bo49boo$67bobo$78b3o$bbobbo4bobbo64bo$3obb6obb3o63bo$bbobbo4b"
             "obbo$$81boo$82boo$81bo$$48bo$46boo$47boo$58bobobbobo$54boobobbobbobboboo$58bobobbobo$$22b3o52b3o$$21"
             "bo3bo50bo3bo$21bo3bo50bo3bo$$22b3o52b3o$88b3o$88b3o$22b3o52b3o9bo$89bo$21bo3bo50bo3bo8bo$21bo3bo17bo"
             "bo30bo3bo7bobo$44boo$22b3o19bo32b3o$88bobo$89bo$8bobboboobobbo69bo$8b4oboob4o69bo$8bobboboobobbo68b3"
             "o$37boo6boo41b3o$35bo4bobbo4bo$35bo4bobbo4bo$35bo4bobbo4bo$37boo6boo$$51b3o66boo$52bo66boo$52bo68bo$"
             "51b3o$$51b3o$38bobo10b3o$39boo$39bo11b3o$52bo$52bo$51b3o26$159bo$159b3o4bobobbobo$158bob4obobbobbobb"
             "oboo$166bobobbobo3$159b3o$159b3o$160bo$160bo$81bo78bo$79bobo77bobo$80boo$$159bobo$160bo$160bo$160bo$"
             "159b3o$159b3o23$117b3o$119bo$118bo$100boo6boo$99bobbo4bobbo$99bobbo4bobbo$99bobbo4bobbo$100boo6boo$1"
             "14b3o$$113bo3bo$113bo3bo$$114b3o3$114b3o$$113bo3bo$113bo3bo$$114b3o!"),
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
    // A piece costs exactly its live cells, so buying a pattern is never cheaper
    // or dearer than drawing it from single cells.
    static const std::vector<ArsenalItem> arsenal = [] {
        std::vector<ArsenalItem> items;
        for (const char* id : {"cell", "block", "beehive", "eater1", "blinker", "pulsar", "pentadecathlon", "glider",
                               "lwss", "mwss", "hwss", "copperhead", "r_pentomino", "diehard", "acorn", "gosper_gun",
                               "simkin_gun", "self_assembling_gun"})
            items.push_back({id, static_cast<int>(findPattern(id)->cells.size())});
        return items;
    }();
    return arsenal;
}

}  // namespace gol
