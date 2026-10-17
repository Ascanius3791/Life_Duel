// life_battle: serves the 1v1 colour battle to the browser. All simulation
// happens here in C++; the browser only handles placement and rendering.
#include "gol/battle.hpp"
#include "gol/patterns.hpp"
#include "http_server.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using web::jsonEscape;
using web::Request;
using web::Response;

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
// Sessions: one running battle each, kept in least-recently-used order.
// ---------------------------------------------------------------------------
struct Session {
    std::mutex mutex;
    gol::Battle battle;
    explicit Session(gol::Battle b) : battle(std::move(b)) {}
};

class Sessions {
public:
    std::string add(std::shared_ptr<Session> session) {
        std::lock_guard lock(mutex_);
        std::string id = newId();
        order_.push_front(id);
        map_[id] = {std::move(session), order_.begin()};
        while (map_.size() > kMaxSessions) {
            map_.erase(order_.back());
            order_.pop_back();
        }
        return id;
    }

    std::shared_ptr<Session> get(const std::string& id) {
        std::lock_guard lock(mutex_);
        const auto it = map_.find(id);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second.position);
        return it->second.session;
    }

private:
    static constexpr std::size_t kMaxSessions = 64;

    std::string newId() {
        static const char hex[] = "0123456789abcdef";
        std::string id(16, '0');
        for (auto& c : id) c = hex[rng_() & 15];
        return id;
    }

    struct Entry {
        std::shared_ptr<Session> session;
        std::list<std::string>::iterator position;
    };
    std::mutex mutex_;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> map_;
    std::mt19937_64 rng_{std::random_device{}()};
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

std::string frameJson(const std::string& id, const gol::Battle& b, bool includeBoard) {
    std::ostringstream o;
    o << "{\"id\":\"" << id << "\",\"gen\":" << b.generation() << ",\"maxGen\":" << b.config().maxGenerations
      << ",\"red\":" << b.redCount() << ",\"blue\":" << b.blueCount() << ",\"grey\":" << b.greyCount()
      << ",\"finished\":" << (b.finished() ? "true" : "false") << ",\"winner\":\"" << gol::toString(b.winner())
      << "\",\"reason\":\"" << gol::toString(b.endReason()) << "\",\"period\":" << b.period()
      << ",\"redScore\":" << b.redScore() << ",\"blueScore\":" << b.blueScore();
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
        appendHistory(o, "red", gens, b.redHistory());
        o << ',';
        appendHistory(o, "blue", gens, b.blueHistory());
        o << ',';
        appendHistory(o, "grey", gens, b.greyHistory());
        o << '}';
    }
    o << '}';
    return o.str();
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
std::string findWebDir(const char* argv0) {
    std::string exeDir = argv0;
    const auto slash = exeDir.rfind('/');
    exeDir = slash == std::string::npos ? "." : exeDir.substr(0, slash);
    // Prefer the source tree so edits to the web files show up without rebuilding.
    for (const std::string& dir : {std::string(GOL_WEB_DIR), exeDir + "/web", std::string("battle/web")}) {
        if (::access((dir + "/index.html").c_str(), R_OK) == 0) return dir;
    }
    return {};
}

void openBrowser(const std::string& url) {
    std::thread([url] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Linux desktop, WSL, macOS - whichever exists first. (explorer.exe reports
        // failure even when it succeeds, so only the existence of a tool is checked.)
        for (const char* tool : {"wslview", "xdg-open", "explorer.exe", "open"}) {
            const std::string name = tool;
            if (std::system(("command -v " + name + " >/dev/null 2>&1").c_str()) != 0) continue;
            if (std::system((name + " '" + url + "' >/dev/null 2>&1").c_str()) != 0 && name != "explorer.exe") continue;
            return;
        }
        std::puts("  (could not open a browser automatically)");
    }).detach();
}

void usage() {
    std::puts(
        "life_battle - Game of Life colour battle in your browser\n"
        "\n"
        "  --port N        port to listen on (default 8080)\n"
        "  --host ADDR     address to bind (default 127.0.0.1, use 0.0.0.0 for LAN)\n"
        "  --web DIR       directory containing index.html\n"
        "  --no-browser    do not open a browser window");
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::string host = "127.0.0.1";
    std::string webDir = findWebDir(argv[0]);
    bool launchBrowser = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--web" && i + 1 < argc) webDir = argv[++i];
        else if (arg == "--no-browser") launchBrowser = false;
        else {
            usage();
            return arg == "--help" || arg == "-h" ? 0 : 1;
        }
    }
    if (webDir.empty()) {
        std::fprintf(stderr, "Could not find the web/ directory. Pass it with --web DIR.\n");
        return 1;
    }

    Sessions sessions;
    const std::string config = configJson();
    web::Server server;
    server.serveStatic(webDir);

    server.route("GET", "/api/config", [&](const Request&) { return Response::json(config); });

    server.route("GET", "/api/battlefield", [](const Request& r) {
        const int w = static_cast<int>(r.intParam("w", 260, 50, 1000));
        const int h = static_cast<int>(r.intParam("h", 128, 20, 600));
        const int count = static_cast<int>(r.intParam("count", 800, 0, 200000));
        const auto seed = static_cast<std::uint64_t>(r.intParam("seed", 1, 0, 1LL << 52));
        const auto board = gol::makeBattlefield(r.param("preset", "random_mirror"), w, h, count, seed);
        return Response::json("{\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) +
                              ",\"board\":\"" + gol::encodeBoard(board) + "\"}");
    });

    server.route("POST", "/api/battle", [&](const Request& r) {
        gol::BattleConfig cfg;
        cfg.width = static_cast<int>(r.intParam("w", 260, 50, 1000));
        cfg.height = static_cast<int>(r.intParam("h", 128, 20, 600));
        // maxgen=0: no limit, only elimination or repetition ends the battle.
        cfg.maxGenerations = static_cast<int>(r.intParam("maxgen", 2000, 0, 10000000));
        cfg.variant = r.param("variant") == "contagion" ? gol::Variant::Contagion : gol::Variant::BirthOnly;
        std::vector<std::uint8_t> cells(static_cast<std::size_t>(cfg.width) * cfg.height);
        if (!gol::decodeBoard(r.body, cells)) return Response::error(400, "board does not match the size");
        auto session = std::make_shared<Session>(gol::Battle(cfg, std::move(cells)));
        const std::string id = sessions.add(session);
        std::lock_guard lock(session->mutex);
        return Response::json(frameJson(id, session->battle, true));
    });

    // Advance a running battle. Either `n` generations or as many as fit into
    // `ms` milliseconds (used by the max-speed and skip-to-end modes).
    server.route("GET", "/api/step", [&](const Request& r) {
        const std::string id = r.param("id");
        auto session = sessions.get(id);
        if (!session) return Response::error(404, "unknown battle");
        std::lock_guard lock(session->mutex);
        if (r.query.count("ms")) session->battle.advanceFor(static_cast<double>(r.intParam("ms", 20, 1, 2000)));
        else session->battle.advance(static_cast<int>(r.intParam("n", 1, 0, 100000)));
        return Response::json(frameJson(id, session->battle, r.param("board", "1") != "0"));
    });

    if (!server.listen(host, port)) return 1;

    const std::string url = "http://" + std::string(host == "0.0.0.0" ? "localhost" : host) + ":" +
                            std::to_string(port) + "/";
    std::printf("Game of Life - Colour Battle\n  serving %s\n  open %s in your browser (Ctrl+C to quit)\n",
                webDir.c_str(), url.c_str());
    std::fflush(stdout);
    if (launchBrowser) openBrowser(url);
    server.run();
}
