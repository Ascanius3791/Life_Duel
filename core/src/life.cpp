#include "gol/life.hpp"

#include <algorithm>
#include <random>

namespace gol {

LifeGrid::LifeGrid(int width, int height, bool wrap)
    : width_(std::max(1, width)),
      height_(std::max(1, height)),
      wrap_(wrap),
      cells_(static_cast<std::size_t>(width_) * height_, 0),
      next_(cells_.size(), 0) {}

bool LifeGrid::alive(int x, int y) const {
    return inBounds(x, y) && cells_[index(x, y)] != 0;
}

std::uint16_t LifeGrid::age(int x, int y) const {
    return inBounds(x, y) ? cells_[index(x, y)] : 0;
}

void LifeGrid::set(int x, int y, bool alive) {
    if (wrap_) {
        x = ((x % width_) + width_) % width_;
        y = ((y % height_) + height_) % height_;
    } else if (!inBounds(x, y)) {
        return;
    }
    std::uint16_t& cell = cells_[index(x, y)];
    if (alive && cell == 0) {
        cell = 1;
        ++population_;
    } else if (!alive && cell != 0) {
        cell = 0;
        --population_;
    }
}

void LifeGrid::clear() {
    std::fill(cells_.begin(), cells_.end(), 0);
    population_ = 0;
    generation_ = 0;
}

void LifeGrid::randomize(double density, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::bernoulli_distribution coin(std::clamp(density, 0.0, 1.0));
    population_ = 0;
    for (auto& cell : cells_) {
        cell = coin(rng) ? 1 : 0;
        population_ += cell;
    }
    generation_ = 0;
}

void LifeGrid::step() {
    const int w = width_;
    const int h = height_;
    std::size_t population = 0;

    for (int y = 0; y < h; ++y) {
        // Row indices of the neighbours; -1 marks a row outside a non-wrapping grid.
        const int yu = y > 0 ? y - 1 : (wrap_ ? h - 1 : -1);
        const int yd = y < h - 1 ? y + 1 : (wrap_ ? 0 : -1);
        const std::uint16_t* rows[3] = {
            yu >= 0 ? &cells_[static_cast<std::size_t>(yu) * w] : nullptr,
            &cells_[static_cast<std::size_t>(y) * w],
            yd >= 0 ? &cells_[static_cast<std::size_t>(yd) * w] : nullptr,
        };
        std::uint16_t* out = &next_[static_cast<std::size_t>(y) * w];

        for (int x = 0; x < w; ++x) {
            const int xl = x > 0 ? x - 1 : (wrap_ ? w - 1 : -1);
            const int xr = x < w - 1 ? x + 1 : (wrap_ ? 0 : -1);
            int n = 0;
            for (int r = 0; r < 3; ++r) {
                const std::uint16_t* row = rows[r];
                if (!row) continue;
                if (xl >= 0) n += row[xl] != 0;
                if (r != 1) n += row[x] != 0;
                if (xr >= 0) n += row[xr] != 0;
            }
            const std::uint16_t self = rows[1][x];
            if (self != 0) {
                out[x] = (n == 2 || n == 3) ? static_cast<std::uint16_t>(self == 0xFFFF ? self : self + 1) : 0;
            } else {
                out[x] = n == 3 ? 1 : 0;
            }
            population += out[x] != 0;
        }
    }

    cells_.swap(next_);
    population_ = population;
    ++generation_;
}

}  // namespace gol
