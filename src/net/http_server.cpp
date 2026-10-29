#include "net/http_server.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace bmj {

namespace {

constexpr int kMaxActive = 32;

#if defined(_WIN32)
void close_socket(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
#else
void close_socket(intptr_t s) { ::close(static_cast<int>(s)); }
#endif

bool send_all(intptr_t s, const char* p, size_t n) {
    while (n > 0) {
        const int k = ::send(static_cast<decltype(socket(0, 0, 0))>(s), p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        p += k, n -= static_cast<size_t>(k);
    }
    return true;
}

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

}  // namespace

bool url_decode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size() || !std::isxdigit(static_cast<unsigned char>(in[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(in[i + 2])))
            return false;
        out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
        i += 2;
    }
    return true;
}

std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') out += static_cast<char>(c);
        else out += '%', out += hex[c >> 4], out += hex[c & 15];
    }
    return out;
}

bool HttpServer::start(int port, Handler handler, std::string& error) {
    stop();
#if defined(_WIN32)
    static const bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa) return error = "Winsock を始められない", false;
#endif
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) return error = "ソケットを作れない", false;
#else
    if (s < 0) return error = "ソケットを作れない", false;
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        close_socket(static_cast<intptr_t>(s));
        return error = "ポート " + std::to_string(port) + " で待ち受けられない（ほかのプログラムが使っているかもしれない）", false;
    }
    socklen_t len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    listen_ = static_cast<intptr_t>(s);
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) {
            auto c = accept(static_cast<decltype(socket(0, 0, 0))>(listen_), nullptr, nullptr);
#if defined(_WIN32)
            if (c == INVALID_SOCKET) {
#else
            if (c < 0) {
#endif
                if (!running_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // 同時につなぐ数には上限を置く（つなぎっぱなしで埋められないように）。超えたら答えずに切る
            if (active_ >= kMaxActive) {
                close_socket(static_cast<intptr_t>(c));
                continue;
            }
            ++active_;
            std::thread([this, c] {
                serve(static_cast<intptr_t>(c));
                --active_;
            }).detach();
        }
    });
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
#if defined(_WIN32)
    close_socket(listen_);
#else
    shutdown(static_cast<int>(listen_), SHUT_RDWR);
    close_socket(listen_);
#endif
    if (thread_.joinable()) thread_.join();
    // 答えている途中のものが終わるのを待つ（ハンドラが消える前に）
    for (int i = 0; i < 500 && active_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void HttpServer::serve(intptr_t sock) {
    const auto s = static_cast<decltype(socket(0, 0, 0))>(sock);
#if defined(_WIN32)
    DWORD tv = 15000;
#else
    timeval tv{15, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    // 頭（空行まで）を読む。大きすぎる頭は断る
    std::string head;
    char buf[4096];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384) {
        const int k = recv(s, buf, sizeof(buf), 0);
        if (k <= 0) break;
        head.append(buf, static_cast<size_t>(k));
    }
    HttpRequest req;
    HttpResponse res;
    const size_t eol = head.find("\r\n");
    if (head.find("\r\n\r\n") == std::string::npos || eol == std::string::npos) {
        res.status = 400, res.body = "bad request\n";
    } else {
        const std::string line = head.substr(0, eol);
        const size_t a = line.find(' '), b = line.find(' ', a + 1);
        if (a == std::string::npos || b == std::string::npos) {
            res.status = 400, res.body = "bad request\n";
        } else {
            req.method = line.substr(0, a);
            req.path = line.substr(a + 1, b - a - 1);
            if (size_t q = req.path.find('?'); q != std::string::npos) req.path.resize(q);
            size_t p = eol + 2;
            while (p < head.size()) {
                const size_t e = head.find("\r\n", p);
                if (e == std::string::npos || e == p) break;
                const std::string h = head.substr(p, e - p);
                if (size_t c = h.find(':'); c != std::string::npos) {
                    std::string k = h.substr(0, c), v = h.substr(c + 1);
                    for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                    req.headers[k] = v;
                }
                p = e + 2;
            }
            if (req.method != "GET") res.status = 400, res.body = "GET only\n";
            else res = handler_(req);
        }
    }
    std::ifstream file;
    uint64_t size = res.body.size();
    if (res.status == 200 && !res.file.empty()) {
        std::error_code ec;
        size = fs::file_size(fs::u8path(res.file), ec);
        file.open(fs::u8path(res.file), std::ios::binary);
        if (ec || !file) res.status = 404, res.body = "not found\n", res.file.clear(), size = res.body.size();
    }
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\nContent-Type: " +
                      (res.file.empty() ? res.type : std::string("application/octet-stream")) +
                      "\r\nContent-Length: " + std::to_string(size) + "\r\nConnection: close\r\n\r\n";
    bool ok = send_all(sock, out.data(), out.size());
    if (ok && !res.file.empty()) {
        std::vector<char> chunk(1 << 20);
        while (ok && file) {
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (file.gcount() > 0) ok = send_all(sock, chunk.data(), static_cast<size_t>(file.gcount()));
        }
    } else if (ok) {
        send_all(sock, res.body.data(), res.body.size());
    }
#if defined(_WIN32)
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    close_socket(sock);
}

}  // namespace bmj
