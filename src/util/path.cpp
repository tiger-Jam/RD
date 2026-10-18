#include "util/path.h"

#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace bmj {

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

DirIndex::DirIndex(std::string dir) : dir_(std::move(dir)) {
    std::error_code ec;
    // 譜面フォルダの直下と、1段下のサブフォルダまで見る
    for (auto it = fs::recursive_directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= 1) it.disable_recursion_pending();
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), dir_, ec).generic_string();
        files_.emplace(normalize_key(rel), rel);
    }
}

std::string DirIndex::resolve(const std::string& name, const std::vector<std::string>& alt_exts) const {
    std::string key = normalize_key(name);
    if (auto it = files_.find(key); it != files_.end()) return join_path(dir_, it->second);
    size_t dot = key.rfind('.');
    std::string stem = dot == std::string::npos ? key : key.substr(0, dot);
    for (const std::string& ext : alt_exts) {
        if (auto it = files_.find(stem + ext); it != files_.end()) return join_path(dir_, it->second);
    }
    return {};
}

}  // namespace bmj
