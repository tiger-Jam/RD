#include "util/http.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace rd {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

// Windowsでは OS の WinHTTP を使う（libcurl を同梱しなくて済む）。post が null なら GET、あれば POST で送る
static bool http_request(const std::string& url, const std::string* post, const std::function<bool(const char*, size_t)>& sink,
                         std::string& error, const HttpHeaders& headers) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return error = "URLが正しくない（" + url + "）", false;
    HINTERNET ses = WinHttpOpen(L"RD", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return error = "WinHTTP を開けない", false;
    WinHttpSetTimeouts(ses, 10000, 10000, 60000, 60000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    std::wstring target = std::wstring(path) + extra;
    HINTERNET req = con ? WinHttpOpenRequest(con, post ? L"POST" : L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    std::wstring extra_headers;
    for (const auto& [k, v] : headers) extra_headers += widen(k + ": " + v + "\r\n");
    bool ok = req &&
              WinHttpSendRequest(req, extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str(),
                                 extra_headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 post ? const_cast<char*>(post->data()) : WINHTTP_NO_REQUEST_DATA,
                                 post ? static_cast<DWORD>(post->size()) : 0, post ? static_cast<DWORD>(post->size()) : 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    DWORD code = 0, len = sizeof(code);
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
    bool stopped = false;
    std::string buf;
    while (ok && code < 400) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            ok = false;
            break;
        }
        if (avail == 0) break;
        buf.resize(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got)) {
            ok = false;
            break;
        }
        if (!sink(buf.data(), got)) {
            stopped = true;
            break;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ok) return error = "接続できない（" + url + "）", false;
    if (code >= 400) return error = "HTTP " + std::to_string(code) + "（" + url + "）", false;
    if (stopped) return error = "途中でやめた（" + url + "）", false;
    return true;
}

#else

namespace {

struct Sink {
    const std::function<bool(const char*, size_t)>* f;
    bool stopped = false;
};

size_t write_cb(char* p, size_t size, size_t n, void* user) {
    Sink* s = static_cast<Sink*>(user);
    if (!(*s->f)(p, size * n)) {
        s->stopped = true;
        return 0;
    }
    return size * n;
}

}  // namespace

static bool http_request(const std::string& url, const std::string* post, const std::function<bool(const char*, size_t)>& sink,
                         std::string& error, const HttpHeaders& headers) {
    static const bool inited = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    (void)inited;
    CURL* c = curl_easy_init();
    if (!c) return error = "curl を初期化できない", false;
    curl_slist* hl = nullptr;
    for (const auto& [k, v] : headers) hl = curl_slist_append(hl, (k + ": " + v).c_str());
    Sink s{&sink};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  // 60 秒なにも来なければやめる（大きなファイルは時間で切らない）
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "RD");
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (post) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(post->size()));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    curl_slist_free_all(hl);
    if (code >= 400) return error = "HTTP " + std::to_string(code) + "（" + url + "）", false;
    if (s.stopped) return error = "途中でやめた（" + url + "）", false;
    if (r != CURLE_OK) return error = std::string(curl_easy_strerror(r)) + "（" + url + "）", false;
    return true;
}

#endif

bool http_get_stream(const std::string& url, const std::function<bool(const char*, size_t)>& sink, std::string& error,
                     const HttpHeaders& headers) {
    return http_request(url, nullptr, sink, error, headers);
}

bool http_get(const std::string& url, std::string& body, std::string& error, const HttpHeaders& headers) {
    body.clear();
    return http_request(url, nullptr, [&](const char* p, size_t n) { return body.append(p, n), true; }, error, headers);
}

bool http_post(const std::string& url, const std::string& data, std::string& reply, std::string& error, const HttpHeaders& headers) {
    reply.clear();
    return http_request(url, &data, [&](const char* p, size_t n) { return reply.append(p, n), true; }, error, headers);
}

}  // namespace rd
