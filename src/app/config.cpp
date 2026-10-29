#include "app/config.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "skin/skin_compose.h"
#include "sqlite3.h"
#include "util/data_dir.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

// skin_custom の値：項目ごとにカンマで区切る。#CUSTOMOPTION は番号、#CUSTOMFILE は "名前"（中の " は "" と書く）。
// 古い形式（#CUSTOMFILE も番号）もそのまま読める
std::vector<SkinCustomChoice> parse_skin_custom(const std::string& val) {
    std::vector<SkinCustomChoice> out;
    size_t i = 0;
    while (!val.empty()) {
        while (i < val.size() && val[i] == ' ') ++i;
        SkinCustomChoice c;
        if (i < val.size() && val[i] == '"') {
            for (++i; i < val.size(); ++i) {
                if (val[i] != '"') c.file.push_back(val[i]);
                else if (i + 1 < val.size() && val[i + 1] == '"') c.file.push_back('"'), ++i;
                else break;
            }
            if (c.file.empty()) c.index = -1;  // 名前が空の選択は、既定を使う
            while (i < val.size() && val[i] != ',') ++i;
        } else {
            size_t end = val.find(',', i);
            if (end == std::string::npos) end = val.size();
            c.index = std::atoi(val.substr(i, end - i).c_str());
            i = end;
        }
        out.push_back(c);
        if (i >= val.size()) break;
        ++i;  // ','
    }
    return out;
}

std::string format_skin_custom(const std::vector<SkinCustomChoice>& sel) {
    std::string out;
    for (size_t i = 0; i < sel.size(); ++i) {
        if (i) out += ",";
        if (sel[i].file.empty()) {
            out += std::to_string(sel[i].index);
            continue;
        }
        out += '"';
        for (char ch : sel[i].file) out += ch == '"' ? std::string("\"\"") : std::string(1, ch);
        out += '"';
    }
    return out;
}

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
        // 前の版は、この 2 つの値の後ろに説明を書いていた（「  #」から後ろは値ではない）
        auto bare = [](std::string v) {
            if (!v.empty() && v[0] == '#') return std::string();  // 値が空で、説明だけ
            if (size_t k = v.find("  #"); k != std::string::npos) v = std::string(trim(std::string_view(v).substr(0, k)));
            return v;
        };
        if (key == "lr2_root") lr2_root = from_portable(val);
        else if (key == "player") player = bare(val);
        else if (key == "lr2_player") lr2_player = bare(val);
        else if (key == "lr2_export") lr2_export = iv != 0;
        else if (key == "log_upload") log_upload = iv != 0;
        else if (key == "auto_update") auto_update = iv != 0;
        else if (key == "service_url") service_url = val;
        else if (key == "play_skin") play_skin = from_portable(val);
        else if (key == "result_skin") result_skin = from_portable(val);
        else if (key == "courseresult_skin") courseresult_skin = from_portable(val);
        else if (key == "select_skin") select_skin = from_portable(val);
        else if (key == "decide_skin") decide_skin = from_portable(val);
        else if (key == "keyconfig_skin") keyconfig_skin = from_portable(val);
        else if (key == "skinselect_skin") skinselect_skin = from_portable(val);
        else if (key == "sound_set") sound_set = from_portable(val);
        else if (key == "skin_res") skin_res = val;
        else if (key == "song_dir") song_dirs.push_back(from_portable(val));
        else if (key == "custom_folder_dir") custom_folder_dirs.push_back(from_portable(val));
        else if (key == "mod") enabled_mods.push_back(val);
        else if (key == "custom_gauge") custom_gauge = val;
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
        else if (key == "auto_adjust") auto_adjust = iv;
        else if (key == "target") target = std::clamp(iv, 0, 5);
        else if (key == "target_percent") target_percent = std::clamp(iv, 50, 100);
        else if (key == "ghost") ghost = std::clamp(iv, 0, 3);
        else if (key == "score_graph") score_graph = iv != 0;
        else if (key == "bga_size") bga_size = std::clamp(iv, 0, 1);
        else if (key == "replay_save") replay_save = std::clamp(iv, 0, 4);
        else if (key == "no_right_click_exit") no_right_click_exit = iv != 0;
        else if (key == "folder_lamp") folder_lamp = iv != 0;
        else if (key == "watch_folders") watch_folders = iv != 0;
        else if (key == "title_flash") title_flash = std::max(0, iv);
        else if (key == "volume_on") volume_on = iv != 0;
        else if (key == "vol_master") vol_master = std::clamp(iv, 0, 100);
        else if (key == "vol_key") vol_key = std::clamp(iv, 0, 100);
        else if (key == "vol_bgm") vol_bgm = std::clamp(iv, 0, 100);
        else if (key == "eq_on") sound_fx.eq_on = iv != 0;
        else if (key.size() == 4 && key.rfind("eq_", 0) == 0 && key[3] >= '0' && key[3] <= '6') sound_fx.eq[key[3] - '0'] = iv;
        else if (key == "pitch_on") sound_fx.pitch_on = iv != 0;
        else if (key == "pitch") sound_fx.pitch = iv;
        else if (key == "pitch_type") sound_fx.pitch_type = iv;
        else if (key.size() > 4 && key.rfind("fx", 0) == 0 && key[2] >= '0' && key[2] <= '2' && key[3] == '_') {
            FxSlot& s = sound_fx.fx[key[2] - '0'];
            std::string k = key.substr(4);
            if (k == "on") s.on = iv != 0;
            else if (k == "type") s.type = iv;
            else if (k == "target") s.target = iv;
            else if (k == "p1") s.p1 = iv;
            else if (k == "p2") s.p2 = iv;
        }
        else if (key == "select_difficulty") select_difficulty = std::clamp(iv, 0, 5);
        else if (key == "select_mode") select_mode = std::clamp(iv, 0, 7);
        else if (key == "select_sort") select_sort = std::clamp(iv, 0, 8);
        else if (key == "ln_mode") ln_mode = iv;
        else if (key == "fhs_green") fhs_green = iv;
        else if (key == "fhs_base") fhs_base = iv;
        else if (key == "gear_mode") gear_mode = iv;
        else if (key == "lift") lift = iv != 0;
        else if (key == "lift_v") lift_v = iv;
        else if (key == "effect") effect = iv;
        else if (key == "random") random = iv;
        else if (key == "seven_key") seven_key = iv != 0;
        else if (key == "gauge") gauge = iv;
        else if (key == "gas") gas = iv != 0;
        else if (key == "fps") fps = iv;
        else if (key == "vsync") vsync = iv != 0;
        else if (key == "fullscreen") fullscreen = iv != 0;
        else if (key == "window_scale") window_scale = iv;
        else if (key == "db_update") db_update = iv;
        else if (key == "select_speed_first") select_speed_first = iv;
        else if (key == "preview") preview = iv != 0;
        else if (key == "sound_16bit") sound_16bit = iv != 0;
        else if (key == "low_memory") low_memory = iv != 0;
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
        else if (key == "serve") serve = iv != 0;
        else if (key == "serve_port") serve_port = iv;
        else if (key == "serve_key") serve_key = val;
        else if (key == "borrow_url") borrow_url = val;
        else if (key == "borrow_key") borrow_key = val;
        else if (key == "borrow_cache_mb") borrow_cache_mb = iv;
        else if (key.rfind("skin_custom ", 0) == 0)
            skin_custom[from_portable(std::string(trim(std::string_view(key).substr(12))))] = parse_skin_custom(val);
        else if (key.rfind("skin_parts ", 0) == 0) {
            // skin_parts <スキンのパス> = size=lane|screen;<差し込み口>=<スキンのパス>[|<項目の番号>=<選んだ名前>…];…
            SkinParts p;
            std::istringstream in(val);
            std::string item;
            while (std::getline(in, item, ';')) {
                const size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                const std::string k(trim(std::string_view(item).substr(0, eq))), v(trim(std::string_view(item).substr(eq + 1)));
                if (k == "size") p.screen_scale = v == "screen";
                else if (!v.empty() && std::any_of(std::begin(kPlayPartSlots), std::end(kPlayPartSlots), [&](const PlayPartSlot& s) { return k == s.key; })) {
                    // 持ってこられない部品（前の版の判定ラインなど）は読まない
                    std::istringstream parts(v);
                    std::string piece;
                    std::getline(parts, piece, '|');
                    p.from[k] = from_portable(std::string(trim(piece)));
                    while (std::getline(parts, piece, '|')) {
                        const size_t e = piece.find('=');
                        if (e == std::string::npos || e == 0) continue;
                        p.custom[k][std::atoi(piece.substr(0, e).c_str())] = std::string(trim(std::string_view(piece).substr(e + 1)));
                    }
                }
            }
            skin_parts[from_portable(std::string(trim(std::string_view(key).substr(11))))] = p;
        } else if (key.rfind("skin_adjust ", 0) == 0) {
            // skin_adjust <スキンのパス> = 位置x,位置y,拡大x,拡大y,判定x,判定y,大きさx,大きさy,制限,ノーツx,ノーツy,判定の大きさ,ノーツだけの縦
            SkinAdjust a;
            int* f[] = {&a.shift_x, &a.shift_y, &a.rate_x, &a.rate_y, &a.judge_x, &a.judge_y, &a.size_x, &a.size_y, &a.dark,
                        &a.note_x, &a.note_y, &a.judge_scale, &a.note_fine_y};
            std::istringstream in(val);
            std::string item;
            for (int i = 0; i < 13 && std::getline(in, item, ','); ++i) *f[i] = std::atoi(item.c_str());
            skin_adjust[from_portable(std::string(trim(std::string_view(key).substr(12))))] = a;
        }
    }
    sound_fx.clamp();
    return true;
}

bool AppConfig::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    write(f);
    return static_cast<bool>(f);
}

void AppConfig::write(std::ostream& f) const {
    f << "# BMjoy の設定\n";
    f << "# 今のプレイヤー（BMjoyfiles/Player/<ID>）\n";
    f << "player = " << player << "\n";
    f << "lr2_root = " << to_portable(lr2_root) << "\n";
    f << "lr2_export = " << (lr2_export ? 1 : 0) << "  # 外部ツールのために LR2 の形の曲データベースとスコアを Database に書き出す\n";
    f << "# ログを作者へ送る（1）・起動するときに新しい版を確かめて入れ替える（1）\n";
    f << "log_upload = " << (log_upload ? 1 : 0) << "\n";
    f << "auto_update = " << (auto_update ? 1 : 0) << "\n";
    if (!service_url.empty()) f << "service_url = " << service_url << "\n";
    f << "lr2_player = " << lr2_player << "\n";  // 書き出すスコアの名前（Database/Score/<名前>.db。空なら LR2 から引き継いだ ID かプレイヤーの ID）
    f << "play_skin = " << to_portable(play_skin) << "\n";
    f << "result_skin = " << to_portable(result_skin) << "\n";
    f << "courseresult_skin = " << to_portable(courseresult_skin) << "\n";
    f << "select_skin = " << to_portable(select_skin) << "\n";
    f << "decide_skin = " << to_portable(decide_skin) << "\n";
    f << "keyconfig_skin = " << to_portable(keyconfig_skin) << "\n";
    f << "skinselect_skin = " << to_portable(skinselect_skin) << "\n";
    f << "sound_set = " << to_portable(sound_set) << "\n";
    f << "skin_res = " << skin_res << "\n";
    for (const std::string& d : song_dirs) f << "song_dir = " << to_portable(d) << "\n";
    for (const std::string& d : custom_folder_dirs) f << "custom_folder_dir = " << to_portable(d) << "\n";
    for (const std::string& m : enabled_mods) f << "mod = " << m << "\n";
    f << "# カスタムゲージで遊ぶならその鍵（MOD の id/ゲージの id）\n";
    f << "custom_gauge = " << custom_gauge << "\n";
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
    f << "ln_mode = " << ln_mode << "  # LN の種類 0=LN 1=CN 2=HCN（譜面で決めていない LN だけ）\n";
    f << "auto_adjust = " << auto_adjust << "  # 判定タイミングの自動調整 0=OFF 1=ON 2=SILENT\n";
    f << "target = " << target << "  # ターゲット 0=既定 1=自己ベスト 2=AAA 3=AA 4=A 5=割合\n";
    f << "target_percent = " << target_percent << "  # 既定のターゲットの割合（50〜100）\n";
    f << "ghost = " << ghost << "  # ゴーストの位置 0=OFF 1=TYPE A 2=TYPE B 3=TYPE C\n";
    f << "bga_size = " << bga_size << "  # BGA の大きさ 0=NORMAL 1=EXTEND\n";
    f << "score_graph = " << (score_graph ? 1 : 0) << "  # スコアグラフ 0=OFF 1=ON\n";
    f << "replay_save = " << replay_save << "  # リプレイの保存 0=OFF 1=ALWAYS 2=HISCORE 3=CLEAR 4=FULLCOMBO\n";
    f << "volume_on = " << (volume_on ? 1 : 0) << "  # 音量の設定を使う 0/1\n";
    f << "vol_master = " << vol_master << "  # 音量 MASTER 0〜100\n";
    f << "vol_key = " << vol_key << "  # 音量 KEY 0〜100\n";
    f << "vol_bgm = " << vol_bgm << "  # 音量 BGM 0〜100\n";
    f << "eq_on = " << (sound_fx.eq_on ? 1 : 0) << "  # イコライザを使う 0/1\n";
    for (int i = 0; i < 7; ++i)
        f << "eq_" << i << " = " << sound_fx.eq[i] << "  # " << static_cast<int>(Equalizer::kFreq[i]) << " Hz（dB。-12〜12）\n";
    f << "pitch_on = " << (sound_fx.pitch_on ? 1 : 0) << "  # ピッチを使う 0/1\n";
    f << "pitch = " << sound_fx.pitch << "  # 半音 -12〜12\n";
    f << "pitch_type = " << sound_fx.pitch_type << "  # 0=FREQ（音程と速さ） 1=PITCH（音程だけ） 2=SPEED（速さだけ）\n";
    for (int i = 0; i < 3; ++i) {
        const FxSlot& s = sound_fx.fx[i];
        f << "fx" << i << "_on = " << (s.on ? 1 : 0) << "\n";
        f << "fx" << i << "_type = " << s.type << "  # 0=OFF 1=REVERB 2=ECHO 3=LPF 4=HPF 5=FLANGER 6=CHORUS 7=DISTORTION\n";
        f << "fx" << i << "_target = " << s.target << "  # 0=MASTER 1=KEY 2=BGM\n";
        f << "fx" << i << "_p1 = " << s.p1 << "\n";
        f << "fx" << i << "_p2 = " << s.p2 << "  # パラメータ 0〜100\n";
    }
    f << "select_difficulty = " << select_difficulty << "  # 選曲の難易度の絞り込み 0=ALL 1〜5\n";
    f << "select_mode = " << select_mode << "  # 選曲の鍵盤数の絞り込み 0=ALL 1=7KEY 2=14KEY 3=9KEY 4=5KEY 5=10KEY 6=SINGLE 7=DOUBLE\n";
    f << "select_sort = " << select_sort << "  # 選曲のソート 0=OFF 1=TITLE 2=ARTIST 3=BPM 4=LENGTH 5=LEVEL 6=CLEAR 7=SCORE 8=MISSCOUNT\n";
    f << "title_flash = " << title_flash << "  # 新曲とみなす時間（時間）\n";
    f << "watch_folders = " << (watch_folders ? 1 : 0) << "  # 1=曲フォルダを見張って、変わったフォルダを読み直す\n";
    f << "folder_lamp = " << (folder_lamp ? 1 : 0) << "  # 1=フォルダのバーにランプを付ける\n";
    f << "no_right_click_exit = " << (no_right_click_exit ? 1 : 0) << "  # 1=演奏中の右クリックで曲を終えない\n";
    f << "bga = " << bga << "  # BGA 0=OFF 1=ON 2=オートプレイのときだけ 3=NO MISS\n";
    f << "hs_mode = " << hs_mode << "  # ハイスピ 0=クラシック 1=フローティング\n";
    f << "fhs_green = " << fhs_green << "  # フローティングの緑数字（1/600 秒）\n";
    f << "fhs_base = " << fhs_base << "  # フローティングの基準の BPM 0=開始 1=最大 2=最小 3=一番長く使われる BPM\n";
    f << "gear_mode = " << gear_mode << "  # ギアチェン 0=カバー追従 1=緑数字に戻す\n";
    f << "lift = " << (lift ? 1 : 0) << "\n";
    f << "lift_v = " << lift_v << "  # LIFT の量 0〜100\n";
    f << "effect = " << effect << "  # 0=OFF 1=HIDDEN 2=SUDDEN 3=HID+SUD\n";
    f << "random = " << random << "  # 0=OFF 1=MIRROR 2=RANDOM 3=S-RANDOM 4=SCATTER 5=CONVERGE\n";
    f << "seven_key = " << (seven_key ? 1 : 0) << "  # 7k モード（7 鍵の譜面の皿を除く）\n";
    f << "gauge = " << gauge << "  # 0=GROOVE 1=HARD 2=DEATH 3=EASY 4=P-ATTACK 5=G-ATTACK\n";
    f << "gas = " << (gas ? 1 : 0) << "\n";
    f << "fps = " << fps << "  # 0=モニタに合わせる -1=上限なし それ以外=その値\n";
    f << "vsync = " << (vsync ? 1 : 0) << "\n";
    f << "fullscreen = " << (fullscreen ? 1 : 0) << "\n";
    f << "window_scale = " << window_scale << "  # ウィンドウの大きさ（%）。0 はスキンに合わせる\n";
    f << "db_update = " << db_update << "  # 0=起動時に曲フォルダを読み直す 1=手動（選曲画面の F8 で今のフォルダだけ）\n";
    f << "select_speed_first = " << select_speed_first << "\n";
    f << "preview = " << (preview ? 1 : 0) << "  # 選曲中に曲を鳴らす\n";
    f << "sound_16bit = " << (sound_16bit ? 1 : 0) << "  # 1=キー音を 16 ビットで持つ（メモリが半分）\n";
    f << "low_memory = " << (low_memory ? 1 : 0) << "  # 1=メモリを節約する（スキンの画像を二重に持たない・フォルダの中身を写さない）\n";
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
    f << "serve = " << (serve ? 1 : 0) << "  # 1 = 譜面を貸す（ログインしている間、裏で）\n";
    f << "serve_port = " << serve_port << "\n";
    f << "serve_key = " << serve_key << "\n";
    f << "borrow_url = " << borrow_url << "\n";
    f << "borrow_key = " << borrow_key << "\n";
    f << "borrow_cache_mb = " << borrow_cache_mb << "  # 借りた譜面の置き場の上限（MB）\n";
    for (const auto& [skin, sel] : skin_custom) f << "skin_custom " << to_portable(skin) << " = " << format_skin_custom(sel) << "\n";
    for (const auto& [skin, p] : skin_parts) {
        if (p.empty()) continue;
        f << "skin_parts " << to_portable(skin) << " = size=" << (p.screen_scale ? "screen" : "lane");
        for (const auto& [k, v] : p.from) {
            f << ";" << k << "=" << to_portable(v);
            if (auto c = p.custom.find(k); c != p.custom.end())
                for (const auto& [i, name] : c->second) f << "|" << i << "=" << name;
        }
        f << "\n";
    }
    for (const auto& [skin, a] : skin_adjust)
        if (!a.is_default())
            f << "skin_adjust " << to_portable(skin) << " = " << a.shift_x << "," << a.shift_y << "," << a.rate_x << "," << a.rate_y << ","
              << a.judge_x << "," << a.judge_y << "," << a.size_x << "," << a.size_y << "," << a.dark << "," << a.note_x << ","
              << a.note_y << "," << a.judge_scale << "," << a.note_fine_y << "\n";
}

std::string config_path() { return data_dir() + "Config/config.txt"; }

namespace {

// プレイヤーごとの設定の鍵（スキン・MOD・プレーのオプション・選曲の並べ方など）。ほかは全員で共通（曲フォルダ・難易度表・画面・音・貸し借りなど）
bool is_player_key(const std::string& key) {
    static const std::set<std::string> keys = {
        "lr2_player", "play_skin", "result_skin", "courseresult_skin", "select_skin", "decide_skin", "keyconfig_skin", "skinselect_skin",
        "sound_set", "mod", "custom_gauge", "hi_speed", "judge_timing", "hs_min", "hs_max", "hs_margin", "hs_fix", "lane_cover",
        "lane_cover_v", "shutter_margin", "ln_mode", "auto_adjust", "target", "target_percent", "ghost", "bga_size", "score_graph",
        "replay_save", "eq_on", "pitch_on", "pitch", "pitch_type", "select_difficulty", "select_mode", "select_sort",
        "no_right_click_exit", "bga", "hs_mode", "fhs_green", "fhs_base", "gear_mode", "lift", "lift_v", "effect", "random",
        "seven_key", "gauge", "gas", "base_speed", "poor_bga_ms", "select_speed_first", "select_speed_next"};
    if (keys.count(key)) return true;
    for (const char* prefix : {"eq_", "fx", "skin_custom ", "skin_parts ", "skin_adjust "})
        if (key.rfind(prefix, 0) == 0) return true;
    return false;
}

// プレイヤーを分ける前の置き場から、プレイヤーの置き場へ移す（キーコンフィグ・リプレイ・MOD が書いたファイル）
void move_into_player(const std::string& from, const std::string& to) {
    std::error_code ec;
    const fs::path f = fs::u8path(from), t = fs::u8path(to);
    if (!fs::exists(f, ec) || fs::exists(t, ec)) return;
    fs::create_directories(t.parent_path(), ec);
    fs::rename(f, t, ec);
    if (ec) std::printf("プレイヤーの置き場へ移せない: %s（%s）\n", from.c_str(), ec.message().c_str());
}

// 最初のプレイヤーの ID：LR2 から引き継いだ ID（前の songs.db にあれば）、無ければ player
std::string first_player_id(const AppConfig& cfg) {
    std::string id = cfg.lr2_player;
    sqlite3* db = nullptr;
    // 読むだけだが、WAL の songs.db は -shm を作れないと読めないので、書ける形で開く（無ければ作らない）
    if (id.empty() && sqlite3_open_v2((data_dir() + "Database/songs.db").c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT value FROM lr2_player WHERE name = 'id'", -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return valid_player_id(id) ? id : "player";
}

}  // namespace

bool load_config(AppConfig& cfg, const std::string& player) {
    const bool had = cfg.load(config_path());
    std::string id = player.empty() ? cfg.player : player;
    const bool fresh = id.empty() || !fs::is_directory(fs::u8path(data_dir() + "Player/" + id));
    if (!valid_player_id(id)) {
        // ID が無い：プレイヤーがいればその最初の人、いなければ最初のプレイヤーを作る（前の設定・記録はこの人のものにする）
        const std::vector<std::string> players = list_players();
        id = players.empty() ? first_player_id(cfg) : players.front();
    }
    set_player_id(id);
    cfg.player = id;
    std::error_code ec;
    fs::create_directories(fs::u8path(player_dir()), ec);
    if (fs::exists(fs::u8path(player_config_path()), ec)) {
        // プレイヤーの設定で置き換える（足していく一覧は先に空にする）
        cfg.enabled_mods.clear(), cfg.skin_custom.clear(), cfg.skin_parts.clear(), cfg.skin_adjust.clear();
        cfg.load(player_config_path());
    } else if (fresh) {
        // 初めてのプレイヤー：前の置き場のものを移し、設定を共通とプレイヤーに分けて書く
        move_into_player(data_dir() + "Config/keyconfig.txt", player_dir() + "keyconfig.txt");
        move_into_player(data_dir() + "Replay", player_dir() + "Replay");
        move_into_player(data_dir() + "Mods/ModData", player_dir() + "ModData");
        if (had) save_config(cfg);
    }
    return had;
}

bool save_config(const AppConfig& cfg) {
    std::ostringstream all;
    cfg.write(all);
    std::string common, mine = "# BMjoy のプレイヤー " + cfg.player + " の設定\n";
    std::istringstream in(all.str());
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        const std::string key = eq == std::string::npos ? std::string() : std::string(trim(std::string_view(line).substr(0, eq)));
        (!key.empty() && key[0] != '#' && is_player_key(key) ? mine : common) += line + "\n";
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(config_path()).parent_path(), ec);
    fs::create_directories(fs::u8path(player_config_path()).parent_path(), ec);
    std::ofstream g(fs::u8path(config_path()), std::ios::binary | std::ios::trunc);
    g << common;
    std::ofstream p(fs::u8path(player_config_path()), std::ios::binary | std::ios::trunc);
    p << mine;
    return static_cast<bool>(g) && static_cast<bool>(p);
}

namespace {
bool set_error(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}
}  // namespace

bool create_player(const std::string& id, const std::string& copy_from, std::string* error) {
    if (!valid_player_id(id)) return set_error(error, "その名前は使えません");
    const fs::path dir = fs::u8path(data_dir() + "Player/" + id);
    std::error_code ec;
    if (fs::exists(dir, ec)) return set_error(error, "同じ名前のプレイヤーがいます");
    fs::create_directories(dir, ec);
    if (ec) return set_error(error, "フォルダを作れません: " + ec.message());
    // 設定とキーコンフィグは、元にするプレイヤーのものを写す（スキンやキーを一から選び直さなくてよい）。記録は写さない
    if (!copy_from.empty())
        for (const char* f : {"config.txt", "keyconfig.txt"})
            fs::copy_file(fs::u8path(data_dir() + "Player/" + copy_from + "/" + f), dir / f, ec), ec.clear();
    return true;
}

bool delete_player(const std::string& id, std::string* error) {
    if (!valid_player_id(id)) return false;
    std::error_code ec;
    fs::remove_all(fs::u8path(data_dir() + "Player/" + id), ec);
    if (ec) return set_error(error, "消せません: " + ec.message());
    return true;
}

// プレイヤーごとのものの置き場（プレイヤーを分けていなければ、前の置き場）
namespace {
std::string replay_dir() { return player_id().empty() ? data_dir() + "Replay/" : player_dir() + "Replay/"; }
}  // namespace

std::string player_config_path() { return player_dir() + "config.txt"; }
std::string player_score_db_path() { return player_dir() + "score.db"; }

std::string replay_path(const std::string& sha256) { return replay_dir() + sha256 + ".bjr"; }

std::string input_log_path(const std::string& sha256, int64_t played_at) {
    return replay_dir() + "InputLog/" + sha256 + "/" + std::to_string(played_at) + ".bjr";
}

std::string official_mods_dir() { return base_dir() + "mods"; }

std::string mod_data_dir(const std::string& id) {
    return (player_id().empty() ? data_dir() + "Mods/ModData/" : player_dir() + "ModData/") + id;
}
std::string mod_settings_path(const std::string& id) { return mod_data_dir(id) + "/settings.json"; }

std::string lr2_courses_path() { return data_dir() + "Database/lr2_courses.txt"; }
// BMjoy の置き場（BMjoyfiles）とは分け、実行ファイルの横の LR2files に LR2 と同じ形で置く（外部ツールが LR2 のフォルダとして読む）
std::string lr2_database_dir() { return base_dir() + "LR2files/Database"; }


std::string table_cache_dir() { return data_dir() + "Database/Table"; }

std::string song_db_path() { return data_dir() + "Database/songs.db"; }

}  // namespace bmj
