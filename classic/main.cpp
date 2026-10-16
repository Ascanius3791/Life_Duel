// life_classic: Conway's Game of Life with a native GUI.
#include "gol/life.hpp"
#include "gol/patterns.hpp"
#include "ui.hpp"

#include <unistd.h>

#include <SFML/Graphics.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float kSidebarWidth = 330.f;
constexpr float kSpeedMin = 1.f;
constexpr float kSpeedMax = 500.f;

std::string findAsset(const char* argv0, const std::string& name) {
    std::string exeDir = argv0;
    const auto slash = exeDir.rfind('/');
    exeDir = slash == std::string::npos ? "." : exeDir.substr(0, slash);
    for (const std::string& dir : {exeDir + "/assets", std::string(GOL_ASSET_DIR), std::string("assets"),
                                   std::string("/usr/share/fonts/truetype/dejavu")}) {
        const std::string path = dir + "/" + name;
        if (::access(path.c_str(), R_OK) == 0) return path;
    }
    return name;
}

// Colour of a living cell by age: fresh cells glow mint, old ones settle into violet.
sf::Color ageColour(std::uint16_t age) {
    struct Stop { float age; sf::Color colour; };
    static const Stop stops[] = {
        {1, {200, 255, 244}}, {3, {78, 230, 193}}, {12, {63, 167, 255}}, {60, {124, 108, 255}}, {250, {92, 64, 190}},
    };
    const float a = static_cast<float>(age);
    if (a <= stops[0].age) return stops[0].colour;
    for (std::size_t i = 1; i < std::size(stops); ++i) {
        if (a <= stops[i].age) {
            const float t = (a - stops[i - 1].age) / (stops[i].age - stops[i - 1].age);
            const auto& c0 = stops[i - 1].colour;
            const auto& c1 = stops[i].colour;
            auto lerp = [t](sf::Uint8 x, sf::Uint8 y) { return static_cast<sf::Uint8>(x + (y - x) * t); };
            return {lerp(c0.r, c1.r), lerp(c0.g, c1.g), lerp(c0.b, c1.b)};
        }
    }
    return std::rbegin(stops)->colour;
}

std::string withSeparators(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

class App {
public:
    App(int width, int height, bool wrap, const char* argv0)
        : window_(sf::VideoMode(1440, 900), "Game of Life"),
          grid_(width, height, wrap),
          ui_(window_, regular_, bold_) {
        window_.setFramerateLimit(60);
        if (!regular_.loadFromFile(findAsset(argv0, "DejaVuSans.ttf")) ||
            !bold_.loadFromFile(findAsset(argv0, "DejaVuSans-Bold.ttf"))) {
            std::fprintf(stderr, "Could not load fonts from the assets directory.\n");
            std::exit(1);
        }
        speedT_ = std::log(30.f / kSpeedMin) / std::log(kSpeedMax / kSpeedMin);
        grid_.randomize(0.18, std::random_device{}());
        fitView();
    }

    // Renders one frame after a few generations and saves it (used for docs and testing).
    void screenshot(const std::string& path) {
        running_ = true;
        for (int i = 0; i < 40; ++i) grid_.step();
        selected_ = static_cast<int>(std::distance(gol::patternLibrary().data(), gol::findPattern("gosper_gun")));
        ui_.handleEvent(mouseMoved(420, 300));
        for (int i = 0; i < 3; ++i) {
            ui_.beginFrame();
            draw();
        }
        sf::Texture texture;
        texture.create(window_.getSize().x, window_.getSize().y);
        texture.update(window_);
        texture.copyToImage().saveToFile(path);
    }

    void run() {
        sf::Clock clock;
        while (window_.isOpen()) {
            const float dt = std::min(0.25f, clock.restart().asSeconds());
            ui_.beginFrame();
            sf::Event event;
            while (window_.pollEvent(event)) {
                ui_.handleEvent(event);
                onEvent(event);
            }
            simulate(dt);
            draw();
        }
    }

private:
    static sf::Event mouseMoved(int x, int y) {
        sf::Event e;
        e.type = sf::Event::MouseMoved;
        e.mouseMove = {x, y};
        return e;
    }

    // ----------------------------------------------------------------- input
    sf::FloatRect boardArea() const {
        const auto size = window_.getSize();
        return {0.f, 0.f, std::max(50.f, size.x - kSidebarWidth), static_cast<float>(size.y)};
    }

    sf::Vector2i cellAt(sf::Vector2f p) const {
        return {static_cast<int>(std::floor((p.x - offset_.x) / cellSize_)),
                static_cast<int>(std::floor((p.y - offset_.y) / cellSize_))};
    }

    void fitView() {
        const auto area = boardArea();
        cellSize_ = std::min((area.width - 40.f) / grid_.width(), (area.height - 40.f) / grid_.height());
        offset_ = {area.left + (area.width - cellSize_ * grid_.width()) / 2.f,
                   area.top + (area.height - cellSize_ * grid_.height()) / 2.f};
    }

    void zoom(float factor, sf::Vector2f around) {
        const float newSize = std::clamp(cellSize_ * factor, 0.5f, 64.f);
        const sf::Vector2f world = (around - offset_) / cellSize_;
        cellSize_ = newSize;
        offset_ = around - world * cellSize_;
    }

    const gol::Pattern* selectedPattern() const {
        return selected_ >= 0 ? &gol::patternLibrary()[static_cast<std::size_t>(selected_)] : nullptr;
    }

    gol::Pattern currentShape() const { return gol::transformed(*selectedPattern(), rotation_, mirror_); }

    void stamp(sf::Vector2i cell) {
        const gol::Pattern shape = currentShape();
        const int x0 = cell.x - shape.width / 2;
        const int y0 = cell.y - shape.height / 2;
        for (const auto& c : shape.cells) grid_.set(x0 + c.x, y0 + c.y, true);
    }

    // Paints along a line so fast mouse movements leave no gaps.
    void paintLine(sf::Vector2i a, sf::Vector2i b, bool alive) {
        int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
        const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            grid_.set(a.x, a.y, alive);
            if (a == b) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; a.x += sx; }
            if (e2 <= dx) { err += dx; a.y += sy; }
        }
    }

    void setSpeedT(float t) { speedT_ = std::clamp(t, 0.f, 1.f); }
    float speed() const { return kSpeedMin * std::pow(kSpeedMax / kSpeedMin, speedT_); }

    void onEvent(const sf::Event& e) {
        const auto mouse = ui_.mouse();
        const bool overBoard = boardArea().contains(mouse);
        switch (e.type) {
            case sf::Event::Closed:
                window_.close();
                break;
            case sf::Event::Resized:
                window_.setView(sf::View(sf::FloatRect(0.f, 0.f, static_cast<float>(e.size.width),
                                                       static_cast<float>(e.size.height))));
                break;
            case sf::Event::MouseWheelScrolled:
                if (overBoard) zoom(e.mouseWheelScroll.delta > 0 ? 1.15f : 1.f / 1.15f, mouse);
                else if (listRect_.contains(mouse)) listScroll_ -= static_cast<int>(e.mouseWheelScroll.delta);
                break;
            case sf::Event::MouseButtonPressed:
                if (!overBoard) break;
                if (e.mouseButton.button == sf::Mouse::Middle) {
                    panning_ = true;
                    panStart_ = mouse;
                } else if (e.mouseButton.button == sf::Mouse::Left) {
                    if (selectedPattern()) stamp(cellAt(mouse));
                    else {
                        painting_ = 1;
                        lastPaint_ = cellAt(mouse);
                        paintLine(lastPaint_, lastPaint_, true);
                    }
                } else if (e.mouseButton.button == sf::Mouse::Right) {
                    if (selectedPattern()) selected_ = -1;
                    else {
                        painting_ = -1;
                        lastPaint_ = cellAt(mouse);
                        paintLine(lastPaint_, lastPaint_, false);
                    }
                }
                break;
            case sf::Event::MouseButtonReleased:
                if (e.mouseButton.button == sf::Mouse::Middle) panning_ = false;
                if (e.mouseButton.button == sf::Mouse::Left || e.mouseButton.button == sf::Mouse::Right) painting_ = 0;
                break;
            case sf::Event::MouseMoved:
                if (panning_) {
                    offset_ += mouse - panStart_;
                    panStart_ = mouse;
                }
                if (painting_ != 0) {
                    const auto cell = cellAt(mouse);
                    paintLine(lastPaint_, cell, painting_ > 0);
                    lastPaint_ = cell;
                }
                break;
            case sf::Event::KeyPressed:
                onKey(e.key);
                break;
            default:
                break;
        }
    }

    void onKey(const sf::Event::KeyEvent& k) {
        using K = sf::Keyboard;
        switch (k.code) {
            case K::Space: running_ = !running_; break;
            case K::N: if (!running_) stepOnce(); break;
            case K::C: grid_.clear(); break;
            case K::X: grid_.randomize(0.18, std::random_device{}()); break;
            case K::R: rotation_ = (rotation_ + (k.shift ? 3 : 1)) % 4; break;
            case K::F: mirror_ = !mirror_; break;
            case K::G: showGrid_ = !showGrid_; break;
            case K::M: maxSpeed_ = !maxSpeed_; break;
            case K::Home: case K::Num0: fitView(); break;
            case K::Escape: selected_ = -1; break;
            case K::Add: case K::Equal: setSpeedT(speedT_ + 0.05f); break;
            case K::Subtract: case K::Hyphen: setSpeedT(speedT_ - 0.05f); break;
            case K::Left: offset_.x += 40.f; break;
            case K::Right: offset_.x -= 40.f; break;
            case K::Up: offset_.y += 40.f; break;
            case K::Down: offset_.y -= 40.f; break;
            default: break;
        }
    }

    // ------------------------------------------------------------ simulation
    void stepOnce() {
        grid_.step();
        rateSamples_.push_back({std::chrono::steady_clock::now(), grid_.generation()});
    }

    void simulate(float dt) {
        if (!running_) {
            accumulator_ = 0.f;
        } else if (maxSpeed_) {
            // Compute as many generations as fit into most of one frame.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
            do grid_.step();
            while (std::chrono::steady_clock::now() < deadline);
        } else {
            accumulator_ = std::min(accumulator_ + speed() * dt, speed() * 0.25f + 1.f);
            while (accumulator_ >= 1.f) {
                grid_.step();
                accumulator_ -= 1.f;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        rateSamples_.push_back({now, grid_.generation()});
        while (rateSamples_.size() > 2 && now - rateSamples_.front().time > std::chrono::seconds(1))
            rateSamples_.pop_front();
    }

    double measuredRate() const {
        if (rateSamples_.size() < 2) return 0;
        const auto& a = rateSamples_.front();
        const auto& b = rateSamples_.back();
        const double seconds = std::chrono::duration<double>(b.time - a.time).count();
        return seconds > 0 ? (b.generation - a.generation) / seconds : 0;
    }

    // --------------------------------------------------------------- drawing
    void draw() {
        window_.clear(ui::theme::background);
        drawBoard();
        drawSidebar();
        window_.display();
    }

    void drawBoard() {
        const auto area = boardArea();
        const float cs = cellSize_;
        const float W = grid_.width() * cs, H = grid_.height() * cs;

        sf::RectangleShape bg({W, H});
        bg.setPosition(offset_);
        bg.setFillColor(ui::theme::board);
        bg.setOutlineColor(ui::theme::border);
        bg.setOutlineThickness(1.f);
        window_.draw(bg);

        // Visible cell range
        const int x0 = std::max(0, static_cast<int>(std::floor((area.left - offset_.x) / cs)));
        const int y0 = std::max(0, static_cast<int>(std::floor((area.top - offset_.y) / cs)));
        const int x1 = std::min(grid_.width(), static_cast<int>(std::ceil((area.left + area.width - offset_.x) / cs)));
        const int y1 = std::min(grid_.height(), static_cast<int>(std::ceil((area.top + area.height - offset_.y) / cs)));

        if (showGrid_ && cs >= 6.f) {
            sf::VertexArray lines(sf::Lines);
            const sf::Color c(255, 255, 255, cs >= 12.f ? 14 : 8);
            for (int x = x0; x <= x1; ++x) {
                const float px = offset_.x + x * cs;
                lines.append({{px, offset_.y + y0 * cs}, c});
                lines.append({{px, offset_.y + y1 * cs}, c});
            }
            for (int y = y0; y <= y1; ++y) {
                const float py = offset_.y + y * cs;
                lines.append({{offset_.x + x0 * cs, py}, c});
                lines.append({{offset_.x + x1 * cs, py}, c});
            }
            window_.draw(lines);
        }

        const float gap = cs >= 5.f ? std::max(1.f, cs * 0.1f) : 0.f;
        cells_.clear();
        cells_.setPrimitiveType(sf::Quads);
        glow_.clear();
        glow_.setPrimitiveType(sf::Quads);
        const bool withGlow = cs >= 4.f;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const auto age = grid_.age(x, y);
                if (age == 0) continue;
                const sf::Color col = ageColour(age);
                const float px = offset_.x + x * cs + gap / 2.f, py = offset_.y + y * cs + gap / 2.f;
                const float s = std::max(cs - gap, 0.5f);
                quad(cells_, px, py, s, s, col);
                if (withGlow) {
                    sf::Color g = col;
                    g.a = 38;
                    const float pad = cs * 0.45f;
                    quad(glow_, px - pad, py - pad, s + 2 * pad, s + 2 * pad, g);
                }
            }
        }
        if (withGlow) window_.draw(glow_, sf::BlendAdd);
        window_.draw(cells_);

        // Pattern preview under the cursor
        const auto mouse = ui_.mouse();
        if (selectedPattern() && area.contains(mouse)) {
            const gol::Pattern shape = currentShape();
            const auto cell = cellAt(mouse);
            const int px0 = cell.x - shape.width / 2, py0 = cell.y - shape.height / 2;
            sf::VertexArray ghost(sf::Quads);
            for (const auto& c : shape.cells) {
                quad(ghost, offset_.x + (px0 + c.x) * cs + gap / 2.f, offset_.y + (py0 + c.y) * cs + gap / 2.f,
                     std::max(cs - gap, 1.f), std::max(cs - gap, 1.f), sf::Color(255, 209, 102, 150));
            }
            window_.draw(ghost);
            sf::RectangleShape box({shape.width * cs + 4.f, shape.height * cs + 4.f});
            box.setPosition(offset_.x + px0 * cs - 2.f, offset_.y + py0 * cs - 2.f);
            box.setFillColor(sf::Color::Transparent);
            box.setOutlineColor(sf::Color(255, 209, 102, 160));
            box.setOutlineThickness(1.f);
            window_.draw(box);
        }
    }

    static void quad(sf::VertexArray& va, float x, float y, float w, float h, sf::Color c) {
        va.append({{x, y}, c});
        va.append({{x + w, y}, c});
        va.append({{x + w, y + h}, c});
        va.append({{x, y + h}, c});
    }

    void drawSidebar() {
        using namespace ui::theme;
        const auto size = window_.getSize();
        const float X = size.x - kSidebarWidth;
        const float pad = 18.f;
        const float W = kSidebarWidth - 2 * pad;
        const float H = static_cast<float>(size.y);

        sf::RectangleShape panelBg({kSidebarWidth, H});
        panelBg.setPosition(X, 0.f);
        panelBg.setFillColor(panel);
        window_.draw(panelBg);
        sf::RectangleShape edge({1.f, H});
        edge.setPosition(X, 0.f);
        edge.setFillColor(border);
        window_.draw(edge);

        float y = 18.f;
        const float x = X + pad;
        ui_.text({x, y}, "Game of Life", 24, text, true);
        y += 32.f;
        ui_.text({x, y}, "Conway  \xC2\xB7  B3/S23", 12, muted);
        y += 28.f;

        // Stats card
        ui::fillRoundedRect(window_, {x, y, W, 74.f}, 10.f, panel2, border, 1.f);
        auto stat = [&](float sx, const char* label, const std::string& value, sf::Color c) {
            ui_.text({sx, y + 12.f}, label, 11, muted);
            ui_.text({sx, y + 32.f}, value, 20, c, true);
        };
        stat(x + 14.f, "GENERATION", withSeparators(grid_.generation()), text);
        stat(x + 14.f + W / 3.f + 6.f, "POPULATION", withSeparators(grid_.population()), mint);
        stat(x + 14.f + 2.f * W / 3.f + 4.f, "GEN / S", withSeparators(static_cast<std::uint64_t>(measuredRate() + 0.5)), accent2);
        y += 90.f;

        // Transport
        const float bw = (W - 8.f) / 2.f;
        if (ui_.button({x, y, bw, 36.f}, running_ ? "Pause" : "Play", running_)) running_ = !running_;
        if (ui_.button({x + bw + 8.f, y, bw, 36.f}, "Step", false, !running_)) stepOnce();
        y += 44.f;
        if (ui_.button({x, y, bw, 32.f}, "Clear")) grid_.clear();
        if (ui_.button({x + bw + 8.f, y, bw, 32.f}, "Randomize")) grid_.randomize(0.18, std::random_device{}());
        y += 48.f;

        // Speed
        char speedText[64];
        const float sp = speed();
        std::snprintf(speedText, sizeof speedText, sp < 10.f ? "%.1f gen/s" : "%.0f gen/s", sp);
        ui_.text({x, y}, "SPEED", 11, muted, true);
        const float stw = ui_.textWidth(maxSpeed_ ? "unlimited" : speedText, 13, true);
        ui_.text({x + W - stw, y - 2.f}, maxSpeed_ ? "unlimited" : speedText, 13, maxSpeed_ ? accent2 : text, true);
        y += 18.f;
        float t = speedT_;
        if (ui_.slider({x + 4.f, y, W - 8.f, 26.f}, t, sf::Color(63, 167, 255), sf::Color(255, 107, 181))) {
            setSpeedT(t);
            maxSpeed_ = false;
        }
        y += 34.f;
        if (ui_.button({x, y, W, 38.f}, maxSpeed_ ? "MAX SPEED  -  ON" : "MAX SPEED", maxSpeed_, true, accent2)) {
            maxSpeed_ = !maxSpeed_;
            if (maxSpeed_) running_ = true;
        }
        y += 46.f;

        const float tw = (W - 16.f) / 3.f;
        if (ui_.button({x, y, tw, 30.f}, "Grid", showGrid_)) showGrid_ = !showGrid_;
        if (ui_.button({x + tw + 8.f, y, tw, 30.f}, "Wrap", grid_.wraps())) grid_.setWrap(!grid_.wraps());
        if (ui_.button({x + 2 * (tw + 8.f), y, tw, 30.f}, "Fit view")) fitView();
        y += 46.f;

        // Pattern library
        ui_.text({x, y}, "PATTERNS", 11, muted, true);
        if (selectedPattern()) {
            const char* hint = "R rotate  \xC2\xB7  F flip";
            ui_.text({x + W - ui_.textWidth(hint, 11), y}, hint, 11, muted);
        }
        y += 20.f;

        const float footer = selectedPattern() ? 160.f : 112.f;
        listRect_ = {x, y, W, std::max(60.f, H - y - footer)};
        const auto& library = gol::patternLibrary();
        const float rowH = 28.f;
        const int visibleRows = static_cast<int>(listRect_.height / rowH);
        const int maxScroll = std::max(0, static_cast<int>(library.size()) - visibleRows);
        listScroll_ = std::clamp(listScroll_, 0, maxScroll);
        for (int i = 0; i < visibleRows && listScroll_ + i < static_cast<int>(library.size()); ++i) {
            const int index = listScroll_ + i;
            const auto& p = library[static_cast<std::size_t>(index)];
            if (ui_.row({x, y + i * rowH, W, rowH - 2.f}, p.name, p.category, index == selected_)) {
                selected_ = index == selected_ ? -1 : index;
            }
        }
        if (maxScroll > 0) {  // scrollbar
            const float trackH = visibleRows * rowH;
            const float thumbH = trackH * visibleRows / library.size();
            const float thumbY = y + (trackH - thumbH) * listScroll_ / maxScroll;
            ui::fillRoundedRect(window_, {x + W + 6.f, thumbY, 3.f, thumbH}, 1.5f, border);
        }
        y = listRect_.top + listRect_.height + 8.f;

        if (const gol::Pattern* p = selectedPattern()) {
            const auto lines = wrap(p->description, W - 20.f, 12);
            const float boxH = 12.f + 16.f * lines.size();
            ui::fillRoundedRect(window_, {x, y, W, boxH}, 8.f, panel2, border, 1.f);
            for (std::size_t i = 0; i < lines.size(); ++i) ui_.text({x + 10.f, y + 5.f + 16.f * i}, lines[i], 12, text);
            y += boxH + 10.f;
        }

        // Help
        const char* help[] = {
            "Left drag: draw   \xC2\xB7   Right drag: erase",
            "Wheel: zoom   \xC2\xB7   Middle drag / arrows: pan",
            "Space play  \xC2\xB7  N step  \xC2\xB7  M max  \xC2\xB7  +/- speed",
            "C clear  \xC2\xB7  X random  \xC2\xB7  G grid  \xC2\xB7  0 fit  \xC2\xB7  Esc",
        };
        for (const char* line : help) {
            ui_.text({x, y}, line, 11, muted);
            y += 17.f;
        }
    }

    std::vector<std::string> wrap(const std::string& s, float width, unsigned size) const {
        std::vector<std::string> lines;
        std::string line, word;
        for (std::size_t i = 0; i <= s.size(); ++i) {
            if (i < s.size() && s[i] != ' ') {
                word += s[i];
                continue;
            }
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && ui_.textWidth(candidate, size) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
            word.clear();
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    }

    struct RateSample {
        std::chrono::steady_clock::time_point time;
        std::uint64_t generation;
    };

    sf::RenderWindow window_;
    sf::Font regular_, bold_;
    gol::LifeGrid grid_;
    ui::Context ui_;

    float cellSize_ = 4.f;
    sf::Vector2f offset_;
    bool panning_ = false;
    sf::Vector2f panStart_;
    int painting_ = 0;  // 1 draw, -1 erase
    sf::Vector2i lastPaint_;

    bool running_ = false;
    bool maxSpeed_ = false;
    bool showGrid_ = true;
    float speedT_ = 0.5f;
    float accumulator_ = 0.f;
    std::deque<RateSample> rateSamples_;

    int selected_ = -1;
    int rotation_ = 0;
    bool mirror_ = false;
    int listScroll_ = 0;
    sf::FloatRect listRect_;

    sf::VertexArray cells_, glow_;
};

}  // namespace

int main(int argc, char** argv) {
    int width = 400, height = 250;
    bool wrap = true;
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width < 8 || height < 8 || width > 4000 ||
                height > 4000) {
                std::fprintf(stderr, "Invalid size, expected e.g. --size 400x250\n");
                return 1;
            }
        } else if (arg == "--no-wrap") {
            wrap = false;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else {
            std::puts("life_classic [--size WxH] [--no-wrap] [--screenshot file.png]");
            return arg == "--help" || arg == "-h" ? 0 : 1;
        }
    }
    App app(width, height, wrap, argv[0]);
    if (!screenshotPath.empty()) app.screenshot(screenshotPath);
    else app.run();
}
