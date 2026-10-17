#include "api_json.hpp"

#include "gol/patterns.hpp"

#include <cstdio>
#include <sstream>
#include <vector>

namespace web {

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace web

namespace duel {

using web::jsonEscape;

namespace {

struct BoardSize {
    const char* id;
    const char* name;
    int width;
    int height;
};
constexpr BoardSize kSizes[] = {
    {"small", "Small (200 x 100)", 200, 100},
    {"medium", "Medium (260 x 128)", 260, 128},
    {"large", "Large (360 x 176)", 360, 176},
};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
std::string patternJson(const gol::Pattern& p, int cost) {
    std::ostringstream o;
    o << "{\"id\":\"" << jsonEscape(p.id) << "\",\"name\":\"" << jsonEscape(p.name) << "\",\"category\":\""
      << jsonEscape(p.category) << "\",\"description\":\"" << jsonEscape(p.description) << "\",\"cost\":" << cost
      << ",\"width\":" << p.width << ",\"height\":" << p.height << ",\"cells\":[";
    for (std::size_t i = 0; i < p.cells.size(); ++i) {
        if (i) o << ',';
        o << '[' << p.cells[i].x << ',' << p.cells[i].y << ']';
    }
    o << "]}";
    return o.str();
}

}  // namespace

std::string configJson() {
    std::ostringstream o;
    o << "{\"arsenal\":[";
    bool first = true;
    for (const auto& item : gol::battleArsenal()) {
        const gol::Pattern* p = gol::findPattern(item.patternId);
        if (!p) continue;
        if (!first) o << ',';
        first = false;
        o << patternJson(*p, item.cost);
    }
    o << "],\"presets\":[";
    first = true;
    for (const auto& preset : gol::battlefieldPresets()) {
        if (!first) o << ',';
        first = false;
        o << "{\"id\":\"" << jsonEscape(preset.id) << "\",\"name\":\"" << jsonEscape(preset.name)
          << "\",\"description\":\"" << jsonEscape(preset.description)
          << "\",\"usesCount\":" << (preset.usesCount ? "true" : "false") << '}';
    }
    o << "],\"sizes\":[";
    first = true;
    for (const auto& s : kSizes) {
        if (!first) o << ',';
        first = false;
        o << "{\"id\":\"" << s.id << "\",\"name\":\"" << s.name << "\",\"width\":" << s.width
          << ",\"height\":" << s.height << '}';
    }
    o << "]}";
    return o.str();
}

std::string battlefieldJson(const std::string& preset, int width, int height, int count, std::uint64_t seed) {
    const auto board = gol::makeBattlefield(preset, width, height, count, seed);
    return "{\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"board\":\"" +
           gol::encodeBoard(board) + "\"}";
}

gol::BattleConfig battleConfig(int width, int height, int maxGenerations, const std::string& variant,
                               bool ignoreHome) {
    gol::BattleConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.maxGenerations = maxGenerations;
    cfg.variant = variant == "contagion" ? gol::Variant::Contagion : gol::Variant::BirthOnly;
    cfg.ignoreHome = ignoreHome;
    return cfg;
}

namespace {

// At most this many history points are sent; long (e.g. unlimited) battles are sampled evenly.
constexpr std::size_t kMaxHistoryPoints = 4000;

void appendHistory(std::ostringstream& o, const char* name, const std::vector<std::size_t>& gens,
                   const std::vector<int>& values) {
    o << '"' << name << "\":[";
    for (std::size_t i = 0; i < gens.size(); ++i) {
        if (i) o << ',';
        o << values[gens[i]];
    }
    o << ']';
}

}  // namespace

std::string frameJson(const std::string& id, const gol::Battle& b, bool includeBoard) {
    std::ostringstream o;
    o << "{\"id\":\"" << id << "\",\"gen\":" << b.generation() << ",\"maxGen\":" << b.config().maxGenerations
      << ",\"red\":" << b.redCount() << ",\"blue\":" << b.blueCount() << ",\"grey\":" << b.greyCount()
      << ",\"finished\":" << (b.finished() ? "true" : "false") << ",\"winner\":\"" << gol::toString(b.winner())
      << "\",\"reason\":\"" << gol::toString(b.endReason()) << "\",\"period\":" << b.period()
      << ",\"redScore\":" << b.redScore() << ",\"blueScore\":" << b.blueScore();
    if (b.config().ignoreHome) o << ",\"redHome\":" << b.redHome() << ",\"blueHome\":" << b.blueHome();
    if (includeBoard) o << ",\"board\":\"" << gol::encodeBoard(b.cells()) << '"';
    if (b.finished()) {
        const std::size_t n = b.redHistory().size();
        const std::size_t stride = (n + kMaxHistoryPoints - 1) / kMaxHistoryPoints;
        std::vector<std::size_t> gens;
        for (std::size_t g = 0; g < n; g += stride) gens.push_back(g);
        if (gens.back() != n - 1) gens.push_back(n - 1);
        o << ",\"history\":{\"gen\":[";
        for (std::size_t i = 0; i < gens.size(); ++i) o << (i ? "," : "") << gens[i];
        o << "],";
        appendHistory(o, "red", gens, b.redScoreHistory());
        o << ',';
        appendHistory(o, "blue", gens, b.blueScoreHistory());
        o << ',';
        appendHistory(o, "grey", gens, b.greyHistory());
        o << '}';
    }
    o << '}';
    return o.str();
}

}  // namespace duel
