#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gol {

// Classic Conway's Game of Life (B3/S23) on a fixed-size grid.
// The grid either wraps around at the edges (torus) or treats everything
// outside as permanently dead.
class LifeGrid {
public:
    LifeGrid(int width, int height, bool wrap = true);

    int width() const { return width_; }
    int height() const { return height_; }
    bool wraps() const { return wrap_; }
    void setWrap(bool wrap) { wrap_ = wrap; }

    bool alive(int x, int y) const;
    void set(int x, int y, bool alive);
    // Generations a cell has been alive without interruption (0 for dead cells).
    std::uint16_t age(int x, int y) const;

    void clear();
    void randomize(double density, std::uint64_t seed);

    void step();

    std::uint64_t generation() const { return generation_; }
    std::size_t population() const { return population_; }

private:
    int index(int x, int y) const { return y * width_ + x; }
    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }

    int width_;
    int height_;
    bool wrap_;
    std::vector<std::uint16_t> cells_;  // age of the cell; 0 = dead
    std::vector<std::uint16_t> next_;
    std::uint64_t generation_ = 0;
    std::size_t population_ = 0;
};

}  // namespace gol
