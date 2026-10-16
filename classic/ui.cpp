#include "ui.hpp"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
sf::String utf8(const std::string& s) { return sf::String::fromUtf8(s.begin(), s.end()); }

sf::Color mix(sf::Color a, sf::Color b, float t) {
    auto lerp = [t](sf::Uint8 x, sf::Uint8 y) { return static_cast<sf::Uint8>(x + (y - x) * t); };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}
}  // namespace

void fillRoundedRect(sf::RenderTarget& target, sf::FloatRect rect, float radius, sf::Color color, sf::Color outline,
                     float outlineThickness) {
    radius = std::min({radius, rect.width / 2.f, rect.height / 2.f});
    constexpr int kSegments = 6;
    sf::ConvexShape shape(4 * (kSegments + 1));
    const sf::Vector2f centres[4] = {
        {rect.left + rect.width - radius, rect.top + radius},
        {rect.left + rect.width - radius, rect.top + rect.height - radius},
        {rect.left + radius, rect.top + rect.height - radius},
        {rect.left + radius, rect.top + radius},
    };
    std::size_t i = 0;
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s <= kSegments; ++s) {
            const float angle = (-90.f + 90.f * corner + 90.f * s / kSegments) * 3.14159265f / 180.f;
            shape.setPoint(i++, centres[corner] + sf::Vector2f(std::cos(angle), std::sin(angle)) * radius);
        }
    }
    shape.setFillColor(color);
    shape.setOutlineColor(outline);
    shape.setOutlineThickness(outlineThickness);
    target.draw(shape);
}

Context::Context(sf::RenderWindow& window, const sf::Font& regular, const sf::Font& bold)
    : window_(window), regular_(regular), bold_(bold) {}

void Context::beginFrame() {
    pressed_ = false;
    released_ = false;
    if (!down_) {
        captured_ = false;
        activeSlider_ = nullptr;
    }
}

void Context::handleEvent(const sf::Event& event) {
    if (event.type == sf::Event::MouseMoved) {
        mouse_ = {static_cast<float>(event.mouseMove.x), static_cast<float>(event.mouseMove.y)};
    } else if (event.type == sf::Event::MouseButtonPressed && event.mouseButton.button == sf::Mouse::Left) {
        pressed_ = true;
        down_ = true;
        mouse_ = {static_cast<float>(event.mouseButton.x), static_cast<float>(event.mouseButton.y)};
    } else if (event.type == sf::Event::MouseButtonReleased && event.mouseButton.button == sf::Mouse::Left) {
        released_ = true;
        down_ = false;
    }
}

bool Context::button(sf::FloatRect rect, const std::string& label, bool highlighted, bool enabled,
                     sf::Color highlightColor) {
    const bool over = enabled && hovered(rect);
    sf::Color fill = highlighted ? highlightColor : (over ? theme::hover : theme::panel2);
    if (highlighted && over) fill = mix(highlightColor, sf::Color::White, 0.12f);
    if (!enabled) fill = mix(theme::panel2, theme::background, 0.5f);
    fillRoundedRect(window_, rect, 8.f, fill, highlighted ? sf::Color::Transparent : theme::border, 1.f);

    const unsigned size = 14;
    const float w = textWidth(label, size, true);
    sf::Color color = enabled ? theme::text : theme::muted;
    if (highlighted && highlightColor == theme::accent2) color = sf::Color(27, 19, 0);
    text({std::round(rect.left + (rect.width - w) / 2.f), std::round(rect.top + (rect.height - size) / 2.f - 2.f)},
         label, size, color, true);

    if (over && pressed_) {
        captured_ = true;
        return true;
    }
    return false;
}

bool Context::slider(sf::FloatRect rect, float& t, sf::Color from, sf::Color to) {
    if (hovered(rect) && pressed_) {
        activeSlider_ = &t;
        captured_ = true;
    }
    bool changed = false;
    if (activeSlider_ == &t && down_) {
        const float nt = std::clamp((mouse_.x - rect.left) / rect.width, 0.f, 1.f);
        changed = nt != t;
        t = nt;
    }

    const float trackH = 8.f;
    const sf::FloatRect track{rect.left, rect.top + (rect.height - trackH) / 2.f, rect.width, trackH};
    fillRoundedRect(window_, track, trackH / 2.f, theme::background, theme::border, 1.f);

    // Gradient fill drawn as thin vertical slices.
    const float filled = track.width * t;
    sf::VertexArray grad(sf::TriangleStrip);
    const int steps = 24;
    for (int i = 0; i <= steps; ++i) {
        const float f = static_cast<float>(i) / steps;
        const float x = track.left + filled * f;
        const sf::Color c = mix(from, to, f * t);
        grad.append({{x, track.top + 1.f}, c});
        grad.append({{x, track.top + trackH - 1.f}, c});
    }
    window_.draw(grad);

    const float r = (activeSlider_ == &t || hovered(rect)) ? 10.f : 9.f;
    sf::CircleShape glow(r + 6.f);
    glow.setOrigin(r + 6.f, r + 6.f);
    glow.setPosition(track.left + filled, track.top + trackH / 2.f);
    sf::Color g = mix(from, to, t);
    g.a = 70;
    glow.setFillColor(g);
    window_.draw(glow);
    sf::CircleShape knob(r);
    knob.setOrigin(r, r);
    knob.setPosition(glow.getPosition());
    knob.setFillColor(sf::Color::White);
    window_.draw(knob);
    return changed;
}

bool Context::row(sf::FloatRect rect, const std::string& label, const std::string& tag, bool selected) {
    const bool over = hovered(rect);
    if (selected || over) {
        fillRoundedRect(window_, rect, 6.f, selected ? sf::Color(124, 108, 255, 60) : theme::panel2,
                        selected ? theme::accent : sf::Color::Transparent, selected ? 1.f : 0.f);
    }
    text({rect.left + 10.f, rect.top + (rect.height - 14.f) / 2.f - 2.f}, label, 14, theme::text);
    const float tw = textWidth(tag, 11);
    text({rect.left + rect.width - tw - 10.f, rect.top + (rect.height - 11.f) / 2.f - 1.f}, tag, 11, theme::muted);
    if (over && pressed_) {
        captured_ = true;
        return true;
    }
    return false;
}

void Context::text(sf::Vector2f position, const std::string& s, unsigned size, sf::Color color, bool bold) {
    sf::Text t(utf8(s), bold ? bold_ : regular_, size);
    t.setFillColor(color);
    t.setPosition(position);
    window_.draw(t);
}

float Context::textWidth(const std::string& s, unsigned size, bool bold) const {
    sf::Text t(utf8(s), bold ? bold_ : regular_, size);
    return t.getLocalBounds().width;
}

}  // namespace ui
