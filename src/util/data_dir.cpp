#include "util/data_dir.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace rd {

std::string base_dir() {
    const char* b = SDL_GetBasePath();
    return b ? b : "";
}

namespace {

constexpr const char* kDataDirName = "RDfiles";

// OS の設定フォルダ（実行ファイルのフォルダに書けないときだけ使う）
std::string pref_dir() {
    char* d = SDL_GetPrefPath(nullptr, "RD");
    std::string p = d ? d : "";
    SDL_free(d);
    return p;
}

// その場所に書けるか（フォルダを作って、試しにファイルを 1 つ書いて消す）
bool writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / ".rd_write_test";
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) return false;
        f << "ok";
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

// 前の名前（BMjoyfiles・LR2files）の置き場が RD のものなら、RDfiles に名前を変える。
// RD のものかは、RD だけが作るもの（songs.db・Player）で見る。LR2 の中に置かれた RD（横に LR2body.exe がある）なら
// LR2 のフォルダなので名前は変えず、そのまま使う。名前を変えられなければ（ほかのプロセスが開いているなど）前の名前のまま使う
fs::path data_root(const fs::path& base) {
    std::error_code ec;
    const fs::path now = base / kDataDirName;
    if (fs::exists(now, ec)) return now;
    // BMjoyfiles は RD の前の名前（BMjoy）のときの置き場。それより前は LR2files
    fs::path old = base / "BMjoyfiles";
    if (!fs::is_directory(old, ec)) old = base / "LR2files";
    if (!fs::is_directory(old, ec)) return now;
    const bool ours = fs::exists(old / "Database" / "songs.db", ec) || fs::is_directory(old / "Player", ec);
    if (!ours) return now;
    if (fs::exists(base / "LR2body.exe", ec)) return old;
    fs::rename(old, now, ec);
    if (!ec) return now;
    std::printf("置き場の名前を変えられない（%s）。前の名前のまま使う\n", ec.message().c_str());
    return old;
}

}  // namespace

std::string data_dir() {
    static const std::string dir = [] {
        const std::string base = base_dir();
        std::string d = pref_dir();
        if (!base.empty()) {
            const fs::path root = data_root(fs::u8path(base));
            if (writable(root / "Config")) d = root.string() + static_cast<char>(fs::path::preferred_separator);
        }
        std::error_code ec;
        for (const char* sub : {"Config", "Database", "Replay"}) fs::create_directories(fs::u8path(d) / sub, ec);
        return d;
    }();
    return dir;
}

std::string data_dir_name() {
    const fs::path d = fs::u8path(data_dir()).parent_path();
    if (fs::u8path(base_dir()).lexically_normal() == d.parent_path().lexically_normal() / "") return d.filename().string();
    return kDataDirName;
}

std::string to_portable(const std::string& path) {
    if (path.empty()) return path;
    const fs::path base = fs::u8path(base_dir()).lexically_normal();
    const fs::path p = fs::u8path(path).lexically_normal();
    if (base.empty() || !p.is_absolute()) return path;
    const fs::path rel = p.lexically_relative(base);
    if (rel.empty()) return path;
    const std::string r = rel.string();
    if (r == ".") return ".";
    if (r.rfind("..", 0) == 0) return path;  // 実行ファイルのフォルダの外は絶対パスのまま
    return r;
}

std::string from_portable(const std::string& path) {
    if (path.empty()) return path;
    const fs::path p = fs::u8path(path);
    if (p.is_absolute() || p.has_root_name()) return path;
    const std::string base = base_dir();
    if (base.empty()) return path;
    fs::path rel = p;
    // 前の名前（LR2files・BMjoyfiles）で書いた RD の置き場の中のパスは、今の置き場に読み替える
    if (auto it = p.begin(); it != p.end()) {
        std::string first = it->string();
        for (char& c : first) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // （実行ファイルの横の LR2files は、外部ツールのための書き出しの置き場なので、設定が指すことはない）
        if ((first == "lr2files" || first == "bmjoyfiles") && data_dir_name() == kDataDirName) {
            rel = kDataDirName;
            for (++it; it != p.end(); ++it) rel /= *it;
        }
    }
    std::string abs = (fs::u8path(base) / rel).lexically_normal().string();
    // "." のときは末尾の区切りを落とさない（素材のフォルダは区切りまでで覚えている）
    return abs;
}

namespace {
std::string g_player;
}  // namespace

const std::string& player_id() { return g_player; }
void set_player_id(const std::string& id) { g_player = id; }

std::string player_dir() { return data_dir() + "Player/" + g_player + "/"; }

std::vector<std::string> list_players() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(data_dir() + "Player"), ec))
        if (e.is_directory(ec)) {
            const auto u8 = e.path().filename().u8string();  // ID は UTF-8 で持つ
            out.emplace_back(u8.begin(), u8.end());
        }
    std::sort(out.begin(), out.end());
    return out;
}

bool valid_player_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id == "." || id == "..") return false;
    for (unsigned char c : id)
        if (c < 0x20 || std::string_view("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
    return id.front() != ' ' && id.back() != ' ' && id.back() != '.';
}

}  // namespace rd
