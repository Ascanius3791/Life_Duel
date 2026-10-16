#pragma once

#include <SFML/Graphics.hpp>

#include <string>

// A tiny immediate-mode widget layer on top of SFML: widgets are drawn and
// queried in the same call, e.g. `if (ui.button(rect, "Step")) step();`.
namespace ui {

namespace theme {
inline const sf::Color background{10, 13, 19};
inline const sf::Color board{13, 17, 25};
inline const sf::Color panel{19, 25, 39};
inline const sf::Color panel2{26, 33, 50};
inline const sf::Color hover{34, 43, 64};
inline const sf::Color border{37, 46, 67};
inline const sf::Color text{231, 234, 243};
inline const sf::Color muted{141, 149, 170};
inline const sf::Color accent{124, 108, 255};
inline const sf::Color accent2{255, 183, 3};
inline const sf::Color mint{78, 230, 193};
}  // namespace theme

void fillRoundedRect(sf::RenderTarget& target, sf::FloatRect rect, float radius, sf::Color color,
                     sf::Color outline = sf::Color::Transparent, float outlineThickness = 0.f);

class Context {
public:
    Context(sf::RenderWindow& window, const sf::Font& regular, const sf::Font& bold);

    // Input bookkeeping: call beginFrame() before polling events, feed every event to handleEvent().
    void beginFrame();
    void handleEvent(const sf::Event& event);

    bool button(sf::FloatRect rect, const std::string& label, bool highlighted = false, bool enabled = true,
                sf::Color highlightColor = theme::accent);
    // `t` is in [0,1]; returns true while the value changes.
    bool slider(sf::FloatRect rect, float& t, sf::Color from, sf::Color to);
    // Selectable list row; returns true when clicked.
    bool row(sf::FloatRect rect, const std::string& label, const std::string& tag, bool selected);

    void text(sf::Vector2f position, const std::string& s, unsigned size, sf::Color color, bool bold = false);
    float textWidth(const std::string& s, unsigned size, bool bold = false) const;

    bool hovered(sf::FloatRect rect) const { return rect.contains(mouse_); }
    sf::Vector2f mouse() const { return mouse_; }
    bool mousePressed() const { return pressed_; }
    bool mouseDown() const { return down_; }
    // True when the current press started on (or is held by) a widget.
    bool captured() const { return captured_; }
    sf::RenderWindow& window() { return window_; }

private:
    sf::RenderWindow& window_;
    const sf::Font& regular_;
    const sf::Font& bold_;
    sf::Vector2f mouse_;
    bool pressed_ = false;
    bool released_ = false;
    bool down_ = false;
    bool captured_ = false;
    const void* activeSlider_ = nullptr;
};

}  // namespace ui
