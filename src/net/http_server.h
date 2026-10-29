// 小さな HTTP サーバー（GET だけ。譜面を貸す配信に使う）。つなぐたびにスレッドを 1 本立て、1 回答えたら切る
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace bmj {

struct HttpRequest {
    std::string method, path;               // path は ? の手前（パーセントエンコードのまま）
    std::map<std::string, std::string> headers;  // 名前は小文字
};

struct HttpResponse {
    int status = 200;
    std::string type = "text/plain; charset=utf-8";
    std::string body;
    std::string file;  // 空でなければ、本文の代わりにこのファイルを送る
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    ~HttpServer() { stop(); }
    // port で待ち受ける（0 なら空いている番号）。すべての口で待つ（loopback_only なら、この PC の中からだけ）
    bool start(int port, Handler handler, std::string& error, bool loopback_only = false);
    void stop();
    int port() const { return port_; }

private:
    void serve(intptr_t sock);
    Handler handler_;
    intptr_t listen_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
    std::thread thread_;
};

// URL のパーセントエンコードを戻す（%xx だけ。+ はそのまま）。おかしな並びなら false
bool url_decode(const std::string& in, std::string& out);
// パーセントエンコードする（英数字と - _ . ~ / 以外）
std::string url_encode(const std::string& in);

}  // namespace bmj
