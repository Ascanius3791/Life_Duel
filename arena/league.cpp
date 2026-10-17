#include "league.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace arena {

bool writeTextFile(const std::string& path, const std::string& content, std::string& error) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
        out << content;
        if (!out.flush()) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot rename " + tmp + " to " + path;
        return false;
    }
    return true;
}

bool readTextFile(const std::string& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

namespace {

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

}  // namespace

bool readLeague(const std::string& path, LeagueFile& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    out = LeagueFile{};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("army\t", 0) == 0) {
            const auto tab = line.find('\t', 5);
            if (tab == std::string::npos) {
                error = path + ":" + std::to_string(lineNo) + ": malformed army line";
                return false;
            }
            out.armies.emplace_back(line.substr(5, tab - 5), line.substr(tab + 1));
            continue;
        }
        std::istringstream ss(line);
        std::string key, value;
        ss >> key >> value;
        Settings& s = out.settings;
        try {
            if (key == "width") s.width = std::stoi(value);
            else if (key == "height") s.height = std::stoi(value);
            else if (key == "preset") s.preset = value;
            else if (key == "count") s.count = std::stoi(value);
            else if (key == "field_seed") s.fieldSeed = std::stoull(value);
            else if (key == "budget") s.budget = std::stoi(value);
            else if (key == "maxgen") s.maxGenerations = std::stoi(value);
            else if (key == "variant") {
                if (!parseVariant(value, s.variant)) throw std::invalid_argument("variant");
            } else {
                error = path + ":" + std::to_string(lineNo) + ": unknown key '" + key + "'";
                return false;
            }
        } catch (const std::exception&) {
            error = path + ":" + std::to_string(lineNo) + ": bad value for " + key;
            return false;
        }
    }
    return true;
}

bool writeLeague(const std::string& path, const Arena& arena, const std::vector<Army>& armies, std::string& error) {
    const Settings& s = arena.settings();
    std::ostringstream out;
    out << "# life_arena league v1\n"
        << "width " << s.width << "\nheight " << s.height << "\npreset " << s.preset << "\ncount " << s.count
        << "\nfield_seed " << s.fieldSeed << "\nbudget " << s.budget << "\nmaxgen " << s.maxGenerations
        << "\nvariant " << variantName(s.variant) << "\n";
    for (const auto& a : armies) {
        std::string origin = a.origin;  // tabs and line breaks would break the format
        for (char& c : origin)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out << "army\t" << origin << '\t' << formatPieces(arena, a) << '\n';
    }
    return writeTextFile(path, out.str(), error);
}

bool writeLeagueJson(const std::string& path, const Arena& arena, const std::vector<Army>& armies,
                     const std::vector<double>& meta, const std::vector<std::vector<double>>& payoff,
                     std::string& error, const std::string& extra) {
    const Settings& s = arena.settings();
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n  \"settings\": {\"width\": " << s.width << ", \"height\": " << s.height
        << ", \"preset\": " << jsonString(s.preset) << ", \"count\": " << s.count << ", \"fieldSeed\": "
        << s.fieldSeed << ", \"budget\": " << s.budget << ", \"maxGenerations\": " << s.maxGenerations
        << ", \"variant\": " << jsonString(variantName(s.variant)) << "},\n  \"armies\": [";
    auto pieces = [&](const Army& a, bool blue) {
        std::string list = "[";
        for (std::size_t k = 0; k < a.pieces.size(); ++k) {
            const Piece p = blue ? arena.toBlue(a.pieces[k]) : a.pieces[k];
            if (k) list += ", ";
            list += "{\"pattern\": " + jsonString(arena.itemId(p.item)) + ", \"x\": " + std::to_string(p.x) +
                    ", \"y\": " + std::to_string(p.y) + ", \"rot\": " + std::to_string(p.rot) +
                    ", \"mirror\": " + (p.mirror ? "true" : "false") + "}";
        }
        return list + "]";
    };
    for (std::size_t i = 0; i < armies.size(); ++i) {
        // Expected score against the mixture: how strong this army is on its own.
        double vsMeta = 0;
        for (std::size_t j = 0; j < armies.size() && i < payoff.size(); ++j)
            vsMeta += payoff[i][j] * (j < meta.size() ? meta[j] : 0);
        out << (i ? ",\n" : "\n") << "    {\"id\": " << i << ", \"origin\": " << jsonString(armies[i].origin)
            << ", \"weight\": " << number(i < meta.size() ? meta[i] : 0) << ", \"vsMeta\": " << number(vsMeta)
            << ", \"cost\": " << arena.cost(armies[i]) << ",\n     \"red\": " << pieces(armies[i], false)
            << ",\n     \"blue\": " << pieces(armies[i], true) << "}";
    }
    out << "\n  ]";
    if (!extra.empty()) out << ",\n  " << extra;
    out << "\n}\n";
    return writeTextFile(path, out.str(), error);
}

}  // namespace arena
