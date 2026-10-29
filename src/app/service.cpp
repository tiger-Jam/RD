#include "app/service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "app/config.h"
#include "util/data_dir.h"
#include "util/hash.h"
#include "util/http.h"
#include "util/log.h"
#include "util/version.h"
#include "yyjson.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// 受け取る側が、知らない送り主からのものを断るための合言葉（配る版に入っているので秘密ではない）
constexpr const char* kServiceKey = "bmjoy-7f3c1a9e";
constexpr size_t kMaxUpload = 32u << 20;  // 送るファイルの大きさの上限

std::string service_url(const AppConfig& cfg) {
    std::string u = cfg.service_url.empty() ? kServiceUrl : cfg.service_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// URL の問い合わせの値に使えるように、英数字と - _ . 以外を %XX にする
std::string url_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out += static_cast<char>(c);
        else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

}  // namespace

std::string client_id() {
    const fs::path p = fs::u8path(data_dir()) / "Config" / "client_id.txt";
    std::string id = read_file(p);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (id.size() == 16) return id;
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd());
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
    id = buf;
    std::ofstream(p, std::ios::binary) << id << "\n";
    return id;
}

void upload_logs_async(const AppConfig& cfg) {
    if (!cfg.log_upload) return;
    const std::string url = service_url(cfg) + "/log";
    std::thread([url] {
        const fs::path dir = fs::u8path(log_dir());
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string name = e.path().filename().string();
            // 書き途中のログ（動いているプロセスのもの）は送らない。ダンプは、同じ名前のログのプロセスで見る
            const std::string base = fs::u8path(name).stem().string() + ".txt";
            if (log_owner_alive(base)) continue;
            files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        const std::string id = client_id();
        int sent = 0;
        for (const fs::path& f : files) {
            std::string data = read_file(f);
            if (data.size() > kMaxUpload) data.resize(kMaxUpload);
            std::string reply, err;
            const std::string name = f.filename().string();
            if (!http_post(url + "?client=" + id + "&name=" + url_escape(name) + "&version=" + url_escape(kBmjVersion), data, reply, err,
                           {{"X-BMjoy-Key", kServiceKey}, {"Content-Type", "application/octet-stream"}})) {
                std::printf("ログを送れない: %s\n", err.c_str());
                return;  // つながらないときは、次に起動したときにまとめて送る
            }
            fs::rename(f, dir / "Sent" / f.filename(), ec);
            if (ec) fs::remove(f, ec);
            sent++;
        }
        if (sent) std::printf("ログを %d 個送った\n", sent);
    }).detach();
}

int compare_versions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> out;
        long cur = 0;
        bool any = false;
        for (char c : v) {
            if (c >= '0' && c <= '9') cur = cur * 10 + (c - '0'), any = true;
            else if (c == '.') out.push_back(cur), cur = 0, any = false;
            else return std::vector<long>{};  // 数字と . 以外（"dev" など）は比べられない
        }
        if (any) out.push_back(cur);
        return out;
    };
    const std::vector<long> x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

UpdateInfo check_update(const AppConfig& cfg) {
    UpdateInfo u;
    if (std::string(kBmjVersion) == "dev") return u.error = "開発中の版", u;
    std::string body;
    const std::string base = service_url(cfg);
    if (!http_get(base + "/update/latest.json", body, u.error)) return u;
    yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    auto str = [&](const char* k) {
        const char* v = root ? yyjson_get_str(yyjson_obj_get(root, k)) : nullptr;
        return std::string(v ? v : "");
    };
    u.version = str("version");
    u.url = str("url");
    u.sha256 = str("sha256");
    u.notes = str("notes");
    yyjson_doc_free(doc);
    if (u.version.empty() || u.url.empty() || u.sha256.size() != 64) return u.error = "更新の情報が読めない", u;
    if (u.url.find("://") == std::string::npos) u.url = base + "/update/" + u.url;  // 置き場からの相対
    u.available = compare_versions(u.version, kBmjVersion) > 0;
    return u;
}

void remove_old_update_files() {
    std::error_code ec;
    const fs::path base = fs::u8path(base_dir());
    if (base.empty()) return;
    for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        // データの置き場の中は見ない（曲やスキンが多い）
        if (it->is_directory(ec) && it.depth() == 0 && it->path().filename().string() == data_dir_name()) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".old") {
            std::error_code e2;
            fs::remove(it->path(), e2);
        }
    }
    fs::remove_all(fs::u8path(data_dir()) / "Update", ec);
}

bool apply_update(const UpdateInfo& u, const std::function<void(const std::string&)>& progress, std::string& error) {
#if defined(_WIN32)
    const fs::path work = fs::u8path(data_dir()) / "Update";
    const fs::path zip = work / "update.zip", out = work / "files";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(out, ec);
    progress("新しい版 " + u.version + " を落としています");
    std::string data;
    size_t got = 0, shown = 0;
    if (!http_get_stream(u.url, [&](const char* p, size_t n) {
            data.append(p, n);
            got += n;
            if (got - shown >= (1u << 20)) shown = got, progress("新しい版 " + u.version + " を落としています（" + std::to_string(got >> 20) + " MB）");
            return true;
        }, error))
        return false;
    if (sha256_hex(data) != u.sha256) return error = "落とした更新が壊れている（SHA-256 が合わない）", false;
    std::ofstream(zip, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
    data.clear();
    progress("新しい版を広げています");
    // zip は Windows に付いている tar で広げる
    std::wstring cmd = L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + out.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return error = "更新を広げられない（tar が動かない）", false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) return error = "更新を広げられない（tar が " + std::to_string(code) + " で終わった）", false;

    // 入れ替える。動いているファイルは消せないが名前は変えられるので、今のものを .old にどけてから置く
    progress("新しい版に入れ替えています");
    const fs::path base = fs::u8path(base_dir());
    std::vector<fs::path> placed;
    for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = it->path().lexically_relative(out);
        const fs::path dst = base / rel;
        std::error_code e2;
        fs::create_directories(dst.parent_path(), e2);
        if (fs::exists(dst, e2)) {
            fs::path old = dst;
            old += ".old";
            fs::remove(old, e2);
            if (fs::exists(old, e2)) old += std::to_string(GetTickCount64());  // 前の .old がまだ使われている
            fs::rename(dst, old, e2);
            if (e2) return error = "入れ替えられない：" + rel.string() + "（" + e2.message() + "）", false;
        }
        fs::rename(it->path(), dst, e2);
        if (e2) fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, e2);
        if (e2) return error = "置けない：" + rel.string() + "（" + e2.message() + "）", false;
        placed.push_back(rel.lexically_normal());
    }
    // 公式の MOD は丸ごと新しい版のものにする（新しい版で無くなった MOD のファイルを消す）
    for (auto it = fs::recursive_directory_iterator(base / "mods", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() == ".old") continue;
        const fs::path rel = it->path().lexically_relative(base).lexically_normal();
        if (std::find(placed.begin(), placed.end(), rel) == placed.end()) {
            std::error_code e2;
            fs::path old = it->path();
            old += ".old";
            fs::rename(it->path(), old, e2);
        }
    }
    return true;
#else
    (void)u;
    (void)progress;
    return error = "自動の更新は Windows だけ", false;
#endif
}

}  // namespace bmj
