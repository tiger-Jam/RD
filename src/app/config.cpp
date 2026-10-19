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
        else if (key == "courseresult_skin") courseresult_skin = val;
        else if (key == "select_skin") select_skin = val;
        else if (key == "decide_skin") decide_skin = val;
        else if (key == "sound_set") sound_set = val;
        else if (key == "skin_res") skin_res = val;
        else if (key == "song_dir") song_dirs.push_back(val);
        else if (key == "table") tables.push_back(val);
        else if (key == "hi_speed") hi_speed = iv;
        else if (key == "judge_timing") judge_timing = iv;
        else if (key == "hs_min") hs_min = iv;
        else if (key == "hs_max") hs_max = iv;
        else if (key == "hs_margin") hs_margin = iv;
        else if (key == "hs_fix") hs_fix = iv;
        else if (key == "lane_cover") lane_cover = iv;
        else if (key == "lane_cover_v") lane_cover_v = iv;
        else if (key == "shutter_margin") shutter_margin = iv;
        else if (key == "hs_mode") hs_mode = iv;
        else if (key == "bga") bga = iv;
        else if (key == "fhs_green") fhs_green = iv;
        else if (key == "fhs_base") fhs_base = iv;
        else if (key == "gear_mode") gear_mode = iv;
        else if (key == "lift") lift = iv != 0;
        else if (key == "lift_v") lift_v = iv;
        else if (key == "effect") effect = iv;
        else if (key == "random") random = iv;
        else if (key == "assist") assist = iv != 0;
        else if (key == "gauge") gauge = iv;
        else if (key == "gas") gas = iv != 0;
        else if (key == "fps") fps = iv;
        else if (key == "vsync") vsync = iv != 0;
        else if (key == "fullscreen") fullscreen = iv != 0;
        else if (key == "window_scale") window_scale = iv;
        else if (key == "window_w") window_w = iv;
        else if (key == "window_h") window_h = iv;
        else if (key == "db_update") db_update = iv;
        else if (key == "select_speed_first") select_speed_first = iv;
        else if (key == "preview") preview = iv != 0;
        else if (key == "base_speed") base_speed = iv;
        else if (key == "poor_bga_ms") poor_bga_ms = iv;
        else if (key == "input_interval") input_interval = iv;
        else if (key == "search_max") search_max = iv;
        else if (key == "levelbar_5") levelbar_5 = iv;
        else if (key == "levelbar_7") levelbar_7 = iv;
        else if (key == "levelbar_9") levelbar_9 = iv;
        else if (key == "select_speed_next") select_speed_next = iv;
        else if (key == "joystick_directinput") joystick_directinput = iv != 0;
        else if (key == "audio_exclusive") audio_exclusive = iv != 0;
        else if (key == "audio_buffer") audio_buffer = iv;
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
    f << "courseresult_skin = " << courseresult_skin << "\n";
    f << "select_skin = " << select_skin << "\n";
    f << "decide_skin = " << decide_skin << "\n";
    f << "sound_set = " << sound_set << "\n";
    f << "skin_res = " << skin_res << "\n";
    for (const std::string& d : song_dirs) f << "song_dir = " << d << "\n";
    for (const std::string& t : tables) f << "table = " << t << "\n";
    f << "hi_speed = " << hi_speed << "\n";
    f << "judge_timing = " << judge_timing << "\n";
    f << "hs_min = " << hs_min << "\n";
    f << "hs_max = " << hs_max << "\n";
    f << "hs_margin = " << hs_margin << "\n";
    f << "hs_fix = " << hs_fix << "  # 0=OFF 1=MAX BPM 2=MIN BPM 3=AVERAGE 4=CONSTANT 5=MAIN BPM\n";
    f << "lane_cover = " << lane_cover << "  # 0=使わない 1=使う（SUDDEN+ のカバー）\n";
    f << "lane_cover_v = " << lane_cover_v << "  # カバーの量 0〜100\n";
    f << "shutter_margin = " << shutter_margin << "\n";
    f << "bga = " << bga << "  # BGA 0=OFF 1=ON 2=オートプレイのときだけ 3=NO MISS\n";
    f << "hs_mode = " << hs_mode << "  # ハイスピ 0=クラシック 1=フローティング\n";
    f << "fhs_green = " << fhs_green << "  # フローティングの緑数字（1/600 秒）\n";
    f << "fhs_base = " << fhs_base << "  # フローティングの基準の BPM 0=開始 1=最大 2=最小 3=一番長く使われる BPM\n";
    f << "gear_mode = " << gear_mode << "  # ギアチェン 0=カバー追従 1=緑数字に戻す\n";
    f << "lift = " << (lift ? 1 : 0) << "\n";
    f << "lift_v = " << lift_v << "  # LIFT の量 0〜100\n";
    f << "effect = " << effect << "  # 0=OFF 1=HIDDEN 2=SUDDEN 3=HID+SUD\n";
    f << "random = " << random << "  # 0=OFF 1=MIRROR 2=RANDOM 3=S-RANDOM 4=SCATTER 5=CONVERGE\n";
    f << "assist = " << (assist ? 1 : 0) << "  # 皿の自動演奏\n";
    f << "gauge = " << gauge << "  # 0=GROOVE 1=HARD 2=DEATH 3=EASY 4=P-ATTACK 5=G-ATTACK\n";
    f << "gas = " << (gas ? 1 : 0) << "\n";
    f << "fps = " << fps << "  # 0=モニタに合わせる -1=上限なし それ以外=その値\n";
    f << "vsync = " << (vsync ? 1 : 0) << "\n";
    f << "fullscreen = " << (fullscreen ? 1 : 0) << "\n";
    f << "window_scale = " << window_scale << "  # ウィンドウの大きさ（%）。0 はスキンに合わせる\n";
    f << "window_w = " << window_w << "\n";
    f << "window_h = " << window_h << "  # ウインドウサイズ。両方 1 以上なら解像度・拡大率より優先\n";
    f << "db_update = " << db_update << "  # 0=起動時に曲フォルダを読み直す 1=手動（選曲画面の F5）\n";
    f << "select_speed_first = " << select_speed_first << "\n";
    f << "preview = " << (preview ? 1 : 0) << "  # 選曲中に曲を鳴らす\n";
    f << "base_speed = " << base_speed << "\n";
    f << "poor_bga_ms = " << poor_bga_ms << "\n";
    f << "input_interval = " << input_interval << "\n";
    f << "search_max = " << search_max << "\n";
    f << "levelbar_5 = " << levelbar_5 << "\n";
    f << "levelbar_7 = " << levelbar_7 << "\n";
    f << "levelbar_9 = " << levelbar_9 << "\n";
    f << "select_speed_next = " << select_speed_next << "\n";
    f << "joystick_directinput = " << (joystick_directinput ? 1 : 0) << "  # Windows：1 にすると SDL の DirectInput も使う（起動が遅くなる環境がある）\n";
    f << "audio_exclusive = " << (audio_exclusive ? 1 : 0) << "  # 1 = Windows の WASAPI 排他モード（音の遅れが小さい）\n";
    f << "audio_buffer = " << audio_buffer << "  # 1 回分のフレーム数。0 = 自動\n";
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
