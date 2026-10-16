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

// All well-known patterns shipped with the program.
const std::vector<Pattern>& patternLibrary();
const Pattern* findPattern(std::string_view id);

// Patterns available in the 1v1 battle mode together with their price.
struct ArsenalItem {
    std::string patternId;
    int cost = 0;
};
const std::vector<ArsenalItem>& battleArsenal();

}  // namespace gol
