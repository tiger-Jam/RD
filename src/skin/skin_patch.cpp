#include "skin/skin_patch.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/hash.h"

namespace fs = std::filesystem;

namespace rd {

namespace {
std::string g_dir;
constexpr const char* kPrefix = "rdpatch:";
}  // namespace

void set_skin_patch_dir(const std::string& dir) { g_dir = dir; }
const std::string& skin_patch_dir() { return g_dir; }

std::string skin_patch_file(const std::string& path) {
    if (g_dir.empty() || path.rfind(kPrefix, 0) != 0) return {};
    std::string rel = path.substr(std::char_traits<char>::length(kPrefix));
    for (char& c : rel)
        if (c == '\\') c = '/';
    if (rel.find("..") != std::string::npos) return {};
    return (fs::u8path(g_dir) / fs::u8path(rel)).string();
}

// 差分の書き方（UTF-8）：
//   # で始まる行は説明
//   @ <始め> <終わり>   元の行の [始め, 終わり)（0 から数える）を、続く + の行で置き換える（始め = 終わりなら、その前に足す）
//   - <行>             元の行（合っているか確かめる）
//   + <行>             新しい行
bool apply_skin_patch(const std::string& bytes, std::vector<std::string>& lines) {
    if (g_dir.empty()) return false;
    const std::string hash = sha256_hex(bytes);
    std::ifstream f(fs::u8path(g_dir) / (hash + ".patch"), std::ios::binary);
    if (!f) return false;
    struct Hunk {
        size_t begin = 0, end = 0;
        std::vector<std::string> old_lines, new_lines;
    };
    std::vector<Hunk> hunks;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 2 && line[0] == '@') {
            Hunk h;
            std::istringstream in(line.substr(1));
            in >> h.begin >> h.end;
            hunks.push_back(std::move(h));
        } else if (!hunks.empty() && line.size() >= 2 && (line[0] == '-' || line[0] == '+') && line[1] == ' ') {
            (line[0] == '-' ? hunks.back().old_lines : hunks.back().new_lines).push_back(line.substr(2));
        }
    }
    // 確かめてから当てる（後ろから当てると、前の番号がずれない）
    for (const Hunk& h : hunks) {
        if (h.begin > h.end || h.end > lines.size() || h.end - h.begin != h.old_lines.size()) return false;
        for (size_t i = 0; i < h.old_lines.size(); ++i)
            if (lines[h.begin + i] != h.old_lines[i]) return false;
    }
    for (auto it = hunks.rbegin(); it != hunks.rend(); ++it) {
        lines.erase(lines.begin() + static_cast<long>(it->begin), lines.begin() + static_cast<long>(it->end));
        lines.insert(lines.begin() + static_cast<long>(it->begin), it->new_lines.begin(), it->new_lines.end());
    }
    std::printf("スキンの差分を当てた（%s）\n", hash.substr(0, 12).c_str());
    return true;
}

}  // namespace rd
