#include "http_server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace web {

namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes = 16 * 1024 * 1024;

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> out;
    std::stringstream ss(q);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        const auto eq = pair.find('=');
        if (eq == std::string::npos) out[urlDecode(pair)] = "";
        else out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return out;
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

std::string mimeType(const std::string& path) {
    const auto dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "ico") return "image/x-icon";
    if (ext == "md") return "text/markdown; charset=utf-8";
    return "application/octet-stream";
}

bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

std::string Request::param(const std::string& name, const std::string& fallback) const {
    const auto it = query.find(name);
    return it == query.end() ? fallback : it->second;
}

long long Request::intParam(const std::string& name, long long fallback, long long min, long long max) const {
    const auto it = query.find(name);
    if (it == query.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return fallback;
    return std::clamp(v, min, max);
}

Response Response::json(std::string body, int status) {
    Response r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

Response Response::error(int status, const std::string& message) {
    return json("{\"error\":\"" + jsonEscape(message) + "\"}", status);
}

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

void Server::route(const std::string& method, const std::string& path, Handler handler) {
    routes_.push_back({method, path, std::move(handler)});
}

bool Server::listen(const std::string& host, int port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::perror("socket");
        return false;
    }
    const int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "Invalid host address: %s\n", host.c_str());
        return false;
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "Cannot bind %s:%d: %s\n", host.c_str(), port, std::strerror(errno));
        return false;
    }
    if (::listen(listenFd_, 64) < 0) {
        std::perror("listen");
        return false;
    }
    return true;
}

void Server::run() {
    for (;;) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            continue;
        }
        std::thread([this, fd] { handleConnection(fd); }).detach();
    }
}

void Server::handleConnection(int fd) const {
    timeval timeout{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    std::string data;
    char buffer[16384];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0 || data.size() > kMaxHeaderBytes) {
            ::close(fd);
            return;
        }
        data.append(buffer, static_cast<std::size_t>(n));
        headerEnd = data.find("\r\n\r\n");
    }

    Request request;
    std::istringstream head(data.substr(0, headerEnd));
    std::string target, version, line;
    head >> request.method >> target >> version;
    std::getline(head, line);
    std::size_t contentLength = 0;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "content-length") contentLength = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
    }

    Response response;
    if (contentLength > kMaxBodyBytes) {
        response = Response::error(413, "request body too large");
    } else {
        request.body = data.substr(headerEnd + 4);
        while (request.body.size() < contentLength) {
            const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.body.append(buffer, static_cast<std::size_t>(n));
        }
        const auto qm = target.find('?');
        request.path = urlDecode(target.substr(0, qm));
        if (qm != std::string::npos) request.query = parseQuery(target.substr(qm + 1));
        try {
            response = dispatch(request);
        } catch (const std::exception& e) {
            response = Response::error(500, e.what());
        }
    }

    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (request.method != "HEAD") out += response.body;
    sendAll(fd, out);
    ::close(fd);
}

Response Server::dispatch(const Request& request) const {
    for (const auto& r : routes_) {
        if (r.path == request.path) {
            if (r.method != request.method) return Response::error(405, "method not allowed");
            return r.handler(request);
        }
    }
    if ((request.method == "GET" || request.method == "HEAD") && !staticDir_.empty()) {
        return staticFile(request.path);
    }
    return Response::error(404, "not found");
}

Response Server::staticFile(const std::string& path) const {
    std::string relative = path == "/" ? "/index.html" : path;
    if (relative.find("..") != std::string::npos || relative.find('\0') != std::string::npos) {
        return Response::error(400, "invalid path");
    }
    std::ifstream file(staticDir_ + relative, std::ios::binary);
    if (!file) return Response::error(404, "not found");
    std::ostringstream content;
    content << file.rdbuf();
    Response r;
    r.contentType = mimeType(relative);
    r.body = content.str();
    return r;
}

}  // namespace web
