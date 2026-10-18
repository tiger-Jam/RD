#include "db/difficulty_table.h"

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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "util/hash.h"
#include "util/text.h"
#include "yyjson.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

#if defined(_WIN32)

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Windowsでは OS の WinHTTP を使う（libcurl を同梱しなくて済む）
bool http_get(const std::string& url, std::string& body, std::string& error) {
    body.clear();
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
    HINTERNET ses = WinHttpOpen(L"BMjoy", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return error = "WinHTTP を開けない", false;
    WinHttpSetTimeouts(ses, 10000, 10000, 60000, 60000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    std::wstring target = std::wstring(path) + extra;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    DWORD code = 0, len = sizeof(code);
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string buf(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got)) break;
        body.append(buf.data(), got);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ok) return error = "接続できない（" + url + "）", false;
    if (code >= 400) return error = "HTTP " + std::to_string(code) + "（" + url + "）", false;
    return true;
}

#else

size_t write_cb(char* p, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(p, size * n);
    return size * n;
}

bool http_get(const std::string& url, std::string& body, std::string& error) {
    static const bool inited = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    (void)inited;
    CURL* c = curl_easy_init();
    if (!c) return error = "curl を初期化できない", false;
    body.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "BMjoy");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) return error = std::string(curl_easy_strerror(r)) + "（" + url + "）", false;
    if (code >= 400) return error = "HTTP " + std::to_string(code) + "（" + url + "）", false;
    return true;
}

#endif

std::string json_str(yyjson_val* obj, const char* key) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (yyjson_is_str(v)) return yyjson_get_str(v);
    if (yyjson_is_int(v)) return std::to_string(yyjson_get_int(v));
    if (yyjson_is_real(v)) return std::to_string(yyjson_get_real(v));
    return {};
}

// BOMを落とす（UTF-8のBOM付きJSONを出す表がある）
std::string strip_bom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

bool looks_like_json(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '{' || c == '[';
    }
    return false;
}

std::string cache_base(const std::string& dir, const std::string& url) {
    return (fs::path(dir) / sha256_hex(url).substr(0, 16)).string();
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.find("://") != std::string::npos) return rel;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return rel;
    size_t host_end = base.find('/', scheme + 3);
    std::string origin = host_end == std::string::npos ? base : base.substr(0, host_end);
    if (rel.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + rel;
    if (!rel.empty() && rel[0] == '/') return origin + rel;
    std::string dir = host_end == std::string::npos ? base + "/" : base.substr(0, base.rfind('/') + 1);
    // クエリは落とす
    size_t q = dir.find('?');
    if (q != std::string::npos) dir = dir.substr(0, dir.rfind('/', q) + 1);
    std::string r = rel;
    while (r.rfind("./", 0) == 0) r.erase(0, 2);
    while (r.rfind("../", 0) == 0) {
        r.erase(0, 3);
        if (dir.size() > origin.size() + 1) {
            dir.pop_back();
            dir = dir.substr(0, dir.rfind('/') + 1);
        }
    }
    return dir + r;
}

std::string find_bmstable_meta(const std::string& html) {
    std::string low = html;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = 0;
    while ((pos = low.find("<meta", pos)) != std::string::npos) {
        size_t end = low.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = low.substr(pos, end - pos);
        if (tag.find("bmstable") != std::string::npos) {
            size_t c = tag.find("content");
            if (c != std::string::npos) {
                size_t q = tag.find_first_of("\"'", c);
                if (q != std::string::npos) {
                    char quote = tag[q];
                    size_t q2 = tag.find(quote, q + 1);
                    if (q2 != std::string::npos) return html.substr(pos + q + 1, q2 - q - 1);  // 元の大文字小文字で返す
                }
            }
        }
        pos = end;
    }
    return {};
}

bool parse_table(const std::string& header_json, const std::string& data_json, DifficultyTable& out, std::string& error) {
    std::string h = strip_bom(header_json), d = strip_bom(data_json);
    yyjson_doc* hd = yyjson_read(h.data(), h.size(), 0);
    yyjson_doc* dd = yyjson_read(d.data(), d.size(), 0);
    bool ok = false;
    if (!hd || !yyjson_is_obj(yyjson_doc_get_root(hd))) {
        error = "header.json を読めない";
    } else if (!dd || !yyjson_is_arr(yyjson_doc_get_root(dd))) {
        error = "data.json を読めない";
    } else {
        yyjson_val* hr = yyjson_doc_get_root(hd);
        out.name = json_str(hr, "name");
        out.symbol = json_str(hr, "symbol");
        out.charts.clear();
        out.levels.clear();
        yyjson_val* order = yyjson_obj_get(hr, "level_order");
        if (yyjson_is_arr(order)) {
            size_t i, n;
            yyjson_val* v;
            yyjson_arr_foreach(order, i, n, v) {
                if (yyjson_is_str(v)) out.levels.push_back(yyjson_get_str(v));
                else if (yyjson_is_num(v)) out.levels.push_back(std::to_string(static_cast<int>(yyjson_get_num(v))));
            }
        }
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(yyjson_doc_get_root(dd), i, n, v) {
            if (!yyjson_is_obj(v)) continue;
            TableChart c;
            c.md5 = json_str(v, "md5");
            c.sha256 = json_str(v, "sha256");
            c.level = json_str(v, "level");
            c.title = json_str(v, "title");
            c.artist = json_str(v, "artist");
            c.url = json_str(v, "url");
            c.url_diff = json_str(v, "url_diff");
            for (char& ch : c.md5) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (c.md5.empty() && c.sha256.empty()) continue;
            out.charts.push_back(std::move(c));
        }
        // level_order が無ければ、出てきたレベルを数値順（数値でなければ出てきた順）に並べる
        if (out.levels.empty()) {
            std::vector<std::string> seen;
            for (const TableChart& c : out.charts)
                if (std::find(seen.begin(), seen.end(), c.level) == seen.end()) seen.push_back(c.level);
            std::stable_sort(seen.begin(), seen.end(), [](const std::string& a, const std::string& b) {
                char* ea;
                char* eb;
                double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
                bool na = *ea == 0 && !a.empty(), nb = *eb == 0 && !b.empty();
                if (na && nb) return x < y;
                return na && !nb;
            });
            out.levels = seen;
        }
        ok = true;
    }
    yyjson_doc_free(hd);
    yyjson_doc_free(dd);
    return ok;
}

namespace {

// URLから header.json と data.json の中身を取ってくる（URLはHTMLでもheader.jsonでもよい）
bool fetch_raw(const std::string& url, std::string& header, std::string& data, std::string& error) {
    std::string body;
    if (!http_get(url, body, error)) return false;
    std::string header_url = url;
    if (!looks_like_json(body)) {
        std::string meta = find_bmstable_meta(body);
        if (meta.empty()) return error = "bmstable のメタタグが無い（" + url + "）", false;
        header_url = resolve_url(url, meta);
        if (!http_get(header_url, body, error)) return false;
    }
    header = strip_bom(body);
    yyjson_doc* hd = yyjson_read(header.data(), header.size(), 0);
    std::string data_url = hd && yyjson_is_obj(yyjson_doc_get_root(hd)) ? json_str(yyjson_doc_get_root(hd), "data_url") : "";
    yyjson_doc_free(hd);
    if (data_url.empty()) return error = "header.json に data_url が無い", false;
    return http_get(resolve_url(header_url, data_url), data, error);
}

int64_t now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

bool fetch_table(const std::string& url, DifficultyTable& out, std::string& error) {
    std::string header, data;
    if (!fetch_raw(url, header, data, error)) return false;
    out.url = url;
    if (!parse_table(header, data, out, error)) return false;
    out.fetched = now_sec();
    out.from_cache = false;
    return true;
}

bool load_cached_table(const std::string& cache_dir, const std::string& url, DifficultyTable& out) {
    std::string base = cache_base(cache_dir, url);
    std::string header = read_file(base + ".header.json"), data = read_file(base + ".data.json");
    if (header.empty() || data.empty()) return false;
    std::string err;
    out.url = url;
    if (!parse_table(header, data, out, err)) return false;
    out.from_cache = true;
    std::ifstream meta(base + ".meta");
    std::string line;
    if (std::getline(meta, line)) out.fetched = std::atoll(line.c_str());
    return true;
}

DifficultyTable update_table(const std::string& cache_dir, const std::string& url) {
    DifficultyTable cached;
    bool have_cache = load_cached_table(cache_dir, url, cached);
    std::string header, data, error;
    DifficultyTable fresh;
    fresh.url = url;
    if (!fetch_raw(url, header, data, error) || !parse_table(header, data, fresh, error)) {
        // 取れなかった・壊れていたときは手元の版を使い続ける
        if (have_cache) {
            cached.warning = "更新できなかったので、手元の版を使う：" + error;
            return cached;
        }
        fresh.warning = "取得できない：" + error;
        return fresh;
    }
    if (have_cache && fresh.charts.size() * 2 < cached.charts.size()) {
        // 譜面が半分未満に減ったら、置き換えずに手元の版を使う（仕様書9章）
        cached.warning = "取得した表の譜面が " + std::to_string(cached.charts.size()) + " → " +
                         std::to_string(fresh.charts.size()) + " に減ったので、置き換えなかった";
        return cached;
    }
    fresh.fetched = now_sec();
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    std::string base = cache_base(cache_dir, url);
    std::ofstream(base + ".header.json", std::ios::binary) << header;
    std::ofstream(base + ".data.json", std::ios::binary) << data;
    std::ofstream(base + ".meta") << fresh.fetched << "\n" << url << "\n";
    return fresh;
}

}  // namespace bmj
