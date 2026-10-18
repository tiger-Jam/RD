#include "app/config.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util/text.h"

namespace bmj {

bool AppConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::string_view l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(trim(l.substr(0, eq)));
        std::string val(trim(l.substr(eq + 1)));
        int iv = std::atoi(val.c_str());
        if (key == "lr2_root") lr2_root = val;
        else if (key == "play_skin") play_skin = val;
        else if (key == "result_skin") result_skin = val;
        else if (key == "sound_set") sound_set = val;
        else if (key == "skin_res") skin_res = val;
        else if (key == "song_dir") song_dirs.push_back(val);
        else if (key == "table") tables.push_back(val);
        else if (key == "hi_speed") hi_speed = iv;
        else if (key == "judge_timing") judge_timing = iv;
        else if (key == "gauge") gauge = iv;
        else if (key == "gas") gas = iv != 0;
        else if (key == "fps") fps = iv;
        else if (key == "vsync") vsync = iv != 0;
        else if (key.rfind("skin_custom ", 0) == 0) {
            // skin_custom <スキンのパス> = 選択,選択,...
            std::vector<int> sel;
            std::istringstream in(val);
            std::string part;
            while (std::getline(in, part, ',')) sel.push_back(std::atoi(part.c_str()));
            skin_custom[std::string(trim(std::string_view(key).substr(12)))] = sel;
        }
    }
    return true;
}

bool AppConfig::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "# BMjoy の設定\n";
    f << "lr2_root = " << lr2_root << "\n";
    f << "play_skin = " << play_skin << "\n";
    f << "result_skin = " << result_skin << "\n";
    f << "sound_set = " << sound_set << "\n";
    f << "skin_res = " << skin_res << "\n";
    for (const std::string& d : song_dirs) f << "song_dir = " << d << "\n";
    for (const std::string& t : tables) f << "table = " << t << "\n";
    f << "hi_speed = " << hi_speed << "\n";
    f << "judge_timing = " << judge_timing << "\n";
    f << "gauge = " << gauge << "  # 0=GROOVE 1=HARD 2=DEATH 3=EASY 4=P-ATTACK 5=G-ATTACK\n";
    f << "gas = " << (gas ? 1 : 0) << "\n";
    f << "fps = " << fps << "  # 0=モニタに合わせる -1=上限なし それ以外=その値\n";
    f << "vsync = " << (vsync ? 1 : 0) << "\n";
    for (const auto& [skin, sel] : skin_custom) {
        f << "skin_custom " << skin << " =";
        for (size_t i = 0; i < sel.size(); ++i) f << (i ? "," : " ") << sel[i];
        f << "\n";
    }
    return static_cast<bool>(f);
}

std::string config_path() {
    char* dir = SDL_GetPrefPath(nullptr, "BMjoy");
    std::string p = dir ? std::string(dir) + "config.txt" : "config.txt";
    SDL_free(dir);
    return p;
}

std::string table_cache_dir() {
    char* dir = SDL_GetPrefPath(nullptr, "BMjoy");
    std::string p = dir ? std::string(dir) + "tables" : "tables";
    SDL_free(dir);
    return p;
}

std::string song_db_path() {
    char* dir = SDL_GetPrefPath(nullptr, "BMjoy");
    std::string p = dir ? std::string(dir) + "songs.db" : "songs.db";
    SDL_free(dir);
    return p;
}

}  // namespace bmj
