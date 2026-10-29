#include "net/borrow.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/hash.h"
#include "util/http.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

constexpr const char* kPrefix = "/bmjoy/v1/";
constexpr const char* kKeyHeader = "X-BMjoy-Key";
constexpr int kMaxFiles = 20000;  // 1 つのフォルダから返すファイルの数の上限
constexpr const char* kUsedMark = ".bmjoy-used";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ext_of(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    const size_t slash = name.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(name.substr(dot + 1));
}

// 相対パスとして安全か（.. ・絶対パス・ドライブ名・逆スラッシュを含まない）
bool safe_relative(const std::string& rel) {
    if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) return false;
    std::istringstream in(rel);
    std::string part;
    while (std::getline(in, part, '/'))
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}

HttpResponse text(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

// 合言葉を比べる（違う文字の位置で時間が変わらないように、最後まで比べる）
bool same_key(const std::string& a, const std::string& b) {
    unsigned diff = a.size() == b.size() ? 0u : 1u;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i % std::max<size_t>(1, b.size())]);
    return diff == 0;
}

std::string folder_id(const std::string& folder) { return sha256_hex(folder).substr(0, 16); }

HttpHeaders key_header(const BorrowSource& src) { return {{kKeyHeader, src.key}}; }

std::string base_url(const BorrowSource& src) {
    std::string u = src.url;
    while (!u.empty() && u.front() == ' ') u.erase(0, 1);
    if (u.find("://") == std::string::npos) u = "http://" + u;  // 「tiger-pc:7275」のように書いてもよい
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + kPrefix;
}

}  // namespace

bool borrow_chart_file(const std::string& name) {
    static const char* exts[] = {"bms", "bme", "bml", "bmson", "pms", "osu"};
    const std::string e = ext_of(name);
    return std::any_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; });
}

bool borrowable_file(const std::string& name) {
    static const char* exts[] = {"wav", "ogg", "flac", "mp3", "bmp", "png", "jpg", "jpeg", "gif", "mpg", "mpeg",
                                 "avi", "mp4", "wmv", "webm", "m4v", "mkv", "ogv", "txt"};
    const std::string e = ext_of(name);
    return borrow_chart_file(name) || std::any_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; });
}

void BorrowLibrary::build(const std::vector<ChartRecord>& charts) {
    paths_.clear(), by_hash_.clear(), folders_.clear(), list_.clear();
    for (const ChartRecord& c : charts) {
        if (c.sha256.empty() || c.sha256.rfind("md5:", 0) == 0) continue;  // LR2 から取り込んだだけの仮の譜面は貸さない
        const size_t i = paths_.size();
        paths_.push_back(c.path);
        if (!c.md5.empty()) by_hash_[lower(c.md5)] = i;
        by_hash_[lower(c.sha256)] = i;
        const std::string folder = fs::u8path(c.path).parent_path().string();
        folders_[folder_id(folder)] = folder;
        list_ += lower(c.md5) + "\t" + lower(c.sha256) + "\n";
    }
}

HttpResponse BorrowLibrary::handle(const HttpRequest& req, const std::string& key) const {
    // 合言葉が無い配信はしない
    auto k = req.headers.find("x-bmjoy-key");
    if (key.empty() || k == req.headers.end() || !same_key(k->second, key)) return text(403, "forbidden\n");
    if (req.path.rfind(kPrefix, 0) != 0) return text(404, "not found\n");
    std::string rest;
    if (!url_decode(req.path.substr(std::string(kPrefix).size()), rest)) return text(400, "bad path\n");
    if (rest == "charts") return text(200, list_);
    if (rest.rfind("chart/", 0) == 0) {
        auto it = by_hash_.find(lower(rest.substr(6)));
        if (it == by_hash_.end()) return text(404, "no such chart\n");
        const fs::path chart = fs::u8path(paths_[it->second]);
        const fs::path folder = chart.parent_path();
        std::string body = "id\t" + folder_id(folder.string()) + "\nname\t" + folder.filename().string() + "\nchart\t" +
                           chart.filename().string() + "\n";
        std::error_code ec;
        int n = 0;
        for (auto e = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
             !ec && e != fs::recursive_directory_iterator() && n < kMaxFiles; e.increment(ec)) {
            if (e.depth() > 4) {
                e.disable_recursion_pending();
                continue;
            }
            if (!e->is_regular_file(ec)) continue;
            const std::string rel = e->path().lexically_relative(folder).generic_string();
            if (!safe_relative(rel) || !borrowable_file(rel)) continue;
            body += "file\t" + std::to_string(e->file_size(ec)) + "\t" + rel + "\n";
            ++n;
        }
        return text(200, body);
    }
    if (rest.rfind("file/", 0) == 0) {
        const size_t slash = rest.find('/', 5);
        if (slash == std::string::npos) return text(404, "not found\n");
        auto f = folders_.find(rest.substr(5, slash - 5));
        const std::string rel = rest.substr(slash + 1);
        if (f == folders_.end() || !safe_relative(rel) || !borrowable_file(rel)) return text(404, "not found\n");
        // 曲フォルダの外を指していないか（リンクなどで外へ出ていないか）も確かめる
        std::error_code ec;
        const fs::path base = fs::weakly_canonical(fs::u8path(f->second), ec);
        const fs::path full = fs::weakly_canonical(fs::u8path(f->second) / fs::u8path(rel), ec);
        const std::string b = base.generic_string(), p = full.generic_string();
        if (ec || p.size() <= b.size() || p.compare(0, b.size(), b) != 0 || p[b.size()] != '/') return text(404, "not found\n");
        HttpResponse r;
        r.file = full.string();
        return r;
    }
    return text(404, "not found\n");
}

bool borrow_list(const BorrowSource& src, std::vector<std::pair<std::string, std::string>>& charts, std::string& error) {
    std::string body;
    if (!http_get(base_url(src) + "charts", body, error, key_header(src))) return false;
    charts.clear();
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t != std::string::npos) charts.emplace_back(line.substr(0, t), line.substr(t + 1));
    }
    return true;
}

bool borrow_info(const BorrowSource& src, const std::string& hash, BorrowFolder& out, std::string& error) {
    std::string body;
    if (!http_get(base_url(src) + "chart/" + url_encode(lower(hash)), body, error, key_header(src))) return false;
    out = BorrowFolder{};
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        const std::string k = line.substr(0, t), v = line.substr(t + 1);
        if (k == "id") out.id = v;
        else if (k == "name") out.name = v;
        else if (k == "chart") out.chart = v;
        else if (k == "file") {
            const size_t t2 = v.find('\t');
            if (t2 != std::string::npos && safe_relative(v.substr(t2 + 1)))
                out.files.emplace_back(std::atoll(v.substr(0, t2).c_str()), v.substr(t2 + 1));
        }
    }
    if (out.id.empty() || !safe_relative(out.chart) || out.id.find_first_not_of("0123456789abcdef") != std::string::npos)
        return error = "配信先の答えが読めない", false;
    return true;
}

bool borrow_fetch(const BorrowSource& src, const BorrowFolder& f, const std::string& dir, bool whole,
                  const std::function<bool(int64_t, int64_t)>& progress, std::string& error) {
    // 取るもの：譜面と、譜面でないファイル（もらうときは譜面も全部）
    std::vector<std::pair<int64_t, std::string>> want;
    int64_t total = 0, done = 0;
    for (const auto& [size, rel] : f.files) {
        if (!whole && borrow_chart_file(rel) && rel != f.chart) continue;
        want.emplace_back(size, rel);
        total += size;
    }
    std::error_code ec;
    const fs::path root = fs::u8path(dir);
    fs::create_directories(root, ec);
    for (const auto& [size, rel] : want) {
        const fs::path to = root / fs::u8path(rel);
        if (fs::is_regular_file(to, ec) && static_cast<int64_t>(fs::file_size(to, ec)) == size) {
            done += size;
            if (progress && !progress(done, total)) return error = "やめた", false;
            continue;
        }
        fs::create_directories(to.parent_path(), ec);
        fs::path tmp = to;
        tmp += ".part";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return error = "書けない: " + to.string(), false;
            const int64_t start = done;
            bool ok = http_get_stream(base_url(src) + "file/" + f.id + "/" + url_encode(rel),
                                      [&](const char* p, size_t n) {
                                          out.write(p, static_cast<std::streamsize>(n));
                                          done += static_cast<int64_t>(n);
                                          return static_cast<bool>(out) && (!progress || progress(done, total));
                                      },
                                      error, key_header(src));
            if (!ok) {
                out.close();
                fs::remove(tmp, ec);
                return false;
            }
            done = start + size;
        }
        fs::rename(tmp, to, ec);
        if (ec) return error = "置けない: " + to.string(), false;
    }
    borrow_touch(dir);
    return true;
}

void borrow_touch(const std::string& dir) { std::ofstream(fs::u8path(dir) / kUsedMark, std::ios::trunc) << "1"; }

void borrow_trim(const std::string& root, int64_t limit, const std::string& keep) {
    struct Item {
        fs::path dir;
        fs::file_time_type used;
        int64_t size = 0;
    };
    std::vector<Item> items;
    int64_t total = 0;
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(fs::u8path(root), ec)) {
        if (!d.is_directory(ec)) continue;
        Item it{d.path(), fs::last_write_time(d.path() / kUsedMark, ec), 0};
        if (ec) it.used = fs::file_time_type::min(), ec.clear();
        for (auto e = fs::recursive_directory_iterator(d.path(), ec); !ec && e != fs::recursive_directory_iterator(); e.increment(ec))
            if (e->is_regular_file(ec)) it.size += static_cast<int64_t>(e->file_size(ec));
        ec.clear();
        total += it.size;
        items.push_back(std::move(it));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.used < b.used; });
    const fs::path k = keep.empty() ? fs::path() : fs::u8path(keep).lexically_normal();
    for (const Item& it : items) {
        if (total <= limit) break;
        if (!keep.empty() && it.dir.lexically_normal() == k) continue;
        fs::remove_all(it.dir, ec);
        total -= it.size;
    }
}

}  // namespace bmj
