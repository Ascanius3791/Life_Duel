#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

// A deliberately tiny HTTP/1.1 server (POSIX sockets, one thread per
// connection, `Connection: close`). Good enough to serve the battle UI on a
// local machine or LAN without pulling in external dependencies.
namespace web {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::string body;

    std::string param(const std::string& name, const std::string& fallback = "") const;
    long long intParam(const std::string& name, long long fallback, long long min, long long max) const;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;

    static Response json(std::string body, int status = 200);
    static Response error(int status, const std::string& message);
};

class Server {
public:
    using Handler = std::function<Response(const Request&)>;

    void route(const std::string& method, const std::string& path, Handler handler);
    // Files below `directory` are served for every GET that matches no route.
    void serveStatic(std::string directory) { staticDir_ = std::move(directory); }

    // Binds and listens; returns false (with a message on stderr) on failure.
    bool listen(const std::string& host, int port);
    // Accept loop. Never returns under normal operation.
    void run();

private:
    void handleConnection(int fd) const;
    Response dispatch(const Request& request) const;
    Response staticFile(const std::string& path) const;

    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };
    std::vector<Route> routes_;
    std::string staticDir_;
    int listenFd_ = -1;
};

std::string jsonEscape(const std::string& text);

}  // namespace web
