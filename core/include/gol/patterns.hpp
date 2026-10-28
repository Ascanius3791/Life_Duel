#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gol {

struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    bool operator<(const Point& o) const { return y != o.y ? y < o.y : x < o.x; }
};

struct Pattern {
    std::string id;           // stable machine name, e.g. "gosper_gun"
    std::string name;         // display name
    std::string category;     // "Still life", "Oscillator", "Spaceship", "Gun", "Methuselah", ...
    std::string description;
    int width = 0;
    int height = 0;
    std::vector<Point> cells;  // live cells, coordinates in [0,width) x [0,height)
};

// Parses the body of a run-length-encoded pattern ("bo$2bo$3o!").
// Header lines ("x = 3, y = 3") and comments ("#...") are skipped.
std::vector<Point> parseRle(std::string_view rle);

// Rotates clockwise by 90° `quarterTurns` times, then optionally mirrors
// horizontally. The result is normalised to start at (0,0).
Pattern transformed(const Pattern& pattern, int quarterTurns, bool mirror);

// The pattern after `generations` steps of B3/S23 on an unbounded plane (no
// wrap, no edges), normalised to its bounding box like every library pattern.
// Lets a player pick the phase of e.g. a gun before placing it. Rotating or
// mirroring commutes with this. A pattern that dies out has no cells and size 0.
Pattern advanced(const Pattern& pattern, int generations);

// All well-known patterns shipped with the program.
const std::vector<Pattern>& patternLibrary();
const Pattern* findPattern(std::string_view id);

// Patterns available in the 1v1 battle mode together with their price, which
// is the pattern's number of live cells.
struct ArsenalItem {
    std::string patternId;
    int cost = 0;  // = findPattern(patternId)->cells.size()
};
const std::vector<ArsenalItem>& battleArsenal();

}  // namespace gol
