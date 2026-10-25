#include "util/data_dir.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string pref_dir() {
    char* d = SDL_GetPrefPath(nullptr, "BMjoy");
    std::string p = d ? d : "";
    SDL_free(d);
    return p;
}

// その場所に書けるか（フォルダを作って、試しにファイルを 1 つ書いて消す）
bool writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / ".bmjoy_write_test";
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) return false;
        f << "ok";
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

}  // namespace

std::string base_dir() {
    const char* b = SDL_GetBasePath();
    return b ? b : "";
}

std::string data_dir() {
    static const std::string dir = [] {
        const std::string base = base_dir();
        std::string d = pref_dir();
        if (!base.empty() && writable(fs::u8path(base) / "LR2files" / "Config"))
            d = (fs::u8path(base) / "LR2files").string() + static_cast<char>(fs::path::preferred_separator);
        std::error_code ec;
        for (const char* sub : {"Config", "Database", "Replay"}) fs::create_directories(fs::u8path(d) / sub, ec);
        return d;
    }();
    return dir;
}

bool migrate_old_data() {
    const fs::path old = fs::u8path(pref_dir()), now = fs::u8path(data_dir());
    std::error_code ec;
    if (old.empty() || fs::equivalent(old, now, ec)) return false;
    if (fs::exists(now / "Config" / "config.txt", ec) || !fs::exists(old / "config.txt", ec)) return false;
    // 前の置き場 → 新しい置き場（ファイルは上書きしない）
    struct Move {
        const char* from;
        fs::path to;
    };
    const Move moves[] = {{"config.txt", now / "Config" / "config.txt"},     {"keyconfig.txt", now / "Config" / "keyconfig.txt"},
                          {"songs.db", now / "Database" / "songs.db"},       {"lr2_courses.txt", now / "Database" / "lr2_courses.txt"},
                          {"tables", now / "Database" / "Table"},            {"replay", now / "Replay"},
                          {"inputlog", now / "Replay" / "InputLog"},         {"mods", now / "Mods"},
                          {"moddata", now / "Mods" / "ModData"}};
    for (const Move& m : moves) {
        const fs::path src = old / m.from;
        if (!fs::exists(src, ec)) continue;
        fs::create_directories(fs::is_directory(src, ec) ? m.to : m.to.parent_path(), ec);
        if (fs::is_directory(src, ec))
            fs::copy(src, m.to, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        else
            fs::copy_file(src, m.to, fs::copy_options::skip_existing, ec);
        if (ec) std::printf("前の置き場から写せない: %s（%s）\n", src.string().c_str(), ec.message().c_str());
    }
    std::printf("設定と記録を %s から %s へ写した（前の置き場は残してある）\n", old.string().c_str(), now.string().c_str());
    return true;
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
    std::string abs = (fs::u8path(base) / p).lexically_normal().string();
    // "." のときは末尾の区切りを落とさない（素材のフォルダは区切りまでで覚えている）
    return abs;
}

}  // namespace bmj
