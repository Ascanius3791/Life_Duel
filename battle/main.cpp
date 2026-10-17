// life_battle: serves the 1v1 colour battle to the browser. All simulation
// happens here in C++; the browser only handles placement and rendering.
#include "gol/battle.hpp"
#include "gol/patterns.hpp"
#include "api_json.hpp"
#include "http_server.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using duel::configJson;
using duel::frameJson;
using web::jsonEscape;
using web::Request;
using web::Response;

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
// Bots: leagues trained by life_arena. The server does not interpret them; it
// serves their JSON export verbatim and the browser picks the armies.
// ---------------------------------------------------------------------------
struct Bot {
    std::string id;    // file name without ".json" (and without the ".txt" of "league.txt.json")
    std::string path;  // the league's JSON export
};

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool readFile(const std::string& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

// `path` is a league's JSON export.
bool addBot(std::vector<Bot>& bots, const std::string& path) {
    if (::access(path.c_str(), R_OK) != 0) return false;
    std::string id = std::filesystem::path(path).filename().string();
    id.resize(id.size() - 5);
    if (endsWith(id, ".txt")) id.resize(id.size() - 4);
    const std::string base = id;
    for (int n = 2; std::any_of(bots.begin(), bots.end(), [&](const Bot& b) { return b.id == id; }); ++n)
        id = base + "-" + std::to_string(n);  // keep ids unique
    bots.push_back({id, path});
    return true;
}

std::vector<Bot> leaguesIn(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().string();
        if (entry.is_regular_file(ec) && endsWith(name, ".json")) files.push_back(name);
    }
    std::sort(files.begin(), files.end());
    std::vector<Bot> bots;
    for (const auto& f : files) addBot(bots, f);
    return bots;
}

std::string botsJson(const std::vector<Bot>& bots) {
    std::ostringstream o;
    o << "{\"bots\":[";
    for (std::size_t i = 0; i < bots.size(); ++i) {
        if (i) o << ',';
        o << "{\"id\":\"" << jsonEscape(bots[i].id) << "\"}";
    }
    o << "]}";
    return o.str();
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
std::string executableDir(const char* argv0) {
    const std::string exe = argv0;
    const auto slash = exe.rfind('/');
    return slash == std::string::npos ? "." : exe.substr(0, slash);
}

std::string findWebDir(const char* argv0) {
    // Prefer the source tree so edits to the web files show up without rebuilding.
    for (const std::string& dir : {std::string(GOL_WEB_DIR), executableDir(argv0) + "/web", std::string("battle/web")}) {
        if (::access((dir + "/index.html").c_str(), R_OK) == 0) return dir;
    }
    return {};
}

// The leagues shipped in assets/leagues, looked up the same way as the web files.
std::string findLeagueDir(const char* argv0) {
    for (const std::string& dir :
         {std::string(GOL_LEAGUE_DIR), executableDir(argv0) + "/leagues", std::string("assets/leagues")}) {
        if (::access(dir.c_str(), R_OK | X_OK) == 0) return dir;
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
        "  --league FILE   bot league trained by life_arena (FILE or FILE.json), repeatable.\n"
        "                  Default: every *.json in the shipped assets/leagues directory\n"
        "  --no-browser    do not open a browser window");
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::string host = "127.0.0.1";
    std::string webDir = findWebDir(argv[0]);
    bool launchBrowser = true;
    std::vector<Bot> bots;
    bool leaguesGiven = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--web" && i + 1 < argc) webDir = argv[++i];
        else if (arg == "--league" && i + 1 < argc) {
            // Either the JSON export or the league itself, next to which life_arena writes LEAGUE.json.
            std::string path = argv[++i];
            if (!endsWith(path, ".json")) path += ".json";
            leaguesGiven = true;
            if (!addBot(bots, path)) {
                std::fprintf(stderr, "Cannot read %s (the JSON export of a life_arena league).\n", path.c_str());
                return 1;
            }
        }
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

    std::string leagueDir;
    if (!leaguesGiven) {
        leagueDir = findLeagueDir(argv[0]);
        if (!leagueDir.empty()) bots = leaguesIn(leagueDir);
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
        return Response::json(duel::battlefieldJson(r.param("preset", "random_mirror"), w, h, count, seed));
    });

    server.route("POST", "/api/battle", [&](const Request& r) {
        // maxgen=0: no limit, only elimination or repetition ends the battle.
        // home=0: cells in their own starting area do not count for the score.
        const gol::BattleConfig cfg = duel::battleConfig(
            static_cast<int>(r.intParam("w", 260, 50, 1000)), static_cast<int>(r.intParam("h", 128, 20, 600)),
            static_cast<int>(r.intParam("maxgen", 2000, 0, 10000000)), r.param("variant"),
            r.intParam("home", 1, 0, 1) == 0);
        std::vector<std::uint8_t> cells(static_cast<std::size_t>(cfg.width) * cfg.height);
        if (!gol::decodeBoard(r.body, cells)) return Response::error(400, "board does not match the size");
        auto session = std::make_shared<Session>(gol::Battle(cfg, std::move(cells)));
        const std::string id = sessions.add(session);
        std::lock_guard lock(session->mutex);
        return Response::json(frameJson(id, session->battle, true));
    });

    server.route("GET", "/api/bots", [&](const Request&) { return Response::json(botsJson(bots)); });

    // A bot's league exactly as life_arena exported it. Read on every request,
    // so a league that is still being trained can be played against right away.
    server.route("GET", "/api/bot", [&](const Request& r) {
        const std::string id = r.param("id");
        for (const auto& b : bots) {
            if (b.id != id) continue;
            std::string content;
            if (!readFile(b.path, content)) return Response::error(500, "cannot read " + b.path);
            return Response::json(std::move(content));
        }
        return Response::error(404, "unknown bot");
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
    if (bots.empty()) std::printf("  no bot leagues found, vs Bot mode is off (see --league)\n");
    else std::printf("  %zu bot league%s%s%s\n", bots.size(), bots.size() == 1 ? "" : "s",
                     leagueDir.empty() ? "" : " from ", leagueDir.c_str());
    std::fflush(stdout);
    if (launchBrowser) openBrowser(url);
    server.run();
}
