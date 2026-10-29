#include "util/path.h"

#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace rd {

namespace {

std::string lower_ascii(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// 名前の最後の部分（ファイル名）の拡張子を ext（"." を除いたもの）に替える。拡張子が無ければ足す
std::string replace_ext(const std::string& name, const std::string& ext) {
    size_t slash = name.find_last_of("/\\");
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return name + "." + ext;
    return name.substr(0, dot + 1) + ext;
}

}  // namespace

std::string normalize_key(const std::string& rel) {
    std::string k;
    k.reserve(rel.size());
    for (char c : rel) {
        if (c == '\\') c = '/';
        k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return k;
}

std::string parent_dir(const std::string& path) {
    return fs::path(path).parent_path().string();
}

std::string join_path(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    return (fs::path(dir) / fs::path(rel)).string();
}

DirIndex::DirIndex(std::string dir) : dir_(std::move(dir)) {}

const DirIndex::Listing& DirIndex::listing(const std::string& dir) const {
    auto [it, added] = cache_.try_emplace(dir);
    if (!added) return it->second;
    std::error_code ec;
    for (auto e = fs::directory_iterator(dir.empty() ? "." : dir, fs::directory_options::skip_permission_denied, ec);
         !ec && e != fs::directory_iterator(); e.increment(ec)) {
        std::string name = e->path().filename().string();
        // 大文字小文字だけが違う名前が並んでいたら、先に見つけたほうを使う
        if (e->is_directory(ec)) it->second.dirs.emplace(lower_ascii(name), name);
        else if (e->is_regular_file(ec)) it->second.files.emplace(lower_ascii(name), name);
    }
    return it->second;
}

std::string DirIndex::find_exact(const std::string& name) const {
    // 区切りで分けて、1 段ずつ大文字小文字を区別せずにたどる
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '/' || c == '\\') {
            parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (cur.empty()) return {};
    std::string path = dir_;
    for (const std::string& p : parts) {
        if (p.empty() || p == ".") continue;
        if (p == "..") {
            path = join_path(path, "..");
            continue;
        }
        const Listing& l = listing(path);
        auto it = l.dirs.find(lower_ascii(p));
        if (it == l.dirs.end()) return {};
        path = join_path(path, it->second);
    }
    const Listing& l = listing(path);
    auto it = l.files.find(lower_ascii(cur));
    if (it == l.files.end()) return {};
    return join_path(path, it->second);
}

std::string DirIndex::resolve(const std::string& name, const std::vector<std::string>& alt_exts) const {
    if (std::string p = find_exact(name); !p.empty()) return p;
    for (const std::string& ext : alt_exts) {
        std::string e = !ext.empty() && ext[0] == '.' ? ext.substr(1) : ext;
        if (std::string p = find_exact(replace_ext(name, e)); !p.empty()) return p;
    }
    return {};
}

std::string DirIndex::find_with_exts(const std::string& name, const std::vector<const char*>& exts) const {
    if (std::string p = find_exact(name); !p.empty()) return p;
    for (const char* ext : exts) {
        // LR2 は書かれた名前に ".ogg" などが含まれていれば（大文字小文字は区別する）、その拡張子は試さない
        if (name.find(std::string(".") + ext) != std::string::npos) continue;
        if (std::string p = find_exact(replace_ext(name, ext)); !p.empty()) return p;
    }
    return {};
}

std::string DirIndex::find_sound(const std::string& name) const {
    if (name.empty()) return {};
    if (std::string p = find_with_exts(name, {"ogg", "wav", "flac", "mp3"}); !p.empty()) return p;
    // 1 つ上のフォルダでは wav を ogg より先に試す（LR2: fileutil:796-825）
    return find_with_exts("../" + name, {"wav", "ogg", "flac", "mp3"});
}

std::string DirIndex::find_image(const std::string& name) const {
    if (name.empty()) return {};
    if (std::string p = find_with_exts(name, {"png", "jpg", "bmp"}); !p.empty()) return p;
    return find_with_exts("../" + name, {"png", "jpg", "bmp"});
}

}  // namespace rd
