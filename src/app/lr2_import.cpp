#include "app/lr2_import.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

#include "app/config.h"
#include "app/skin_catalog.h"
#include "db/difficulty_table.h"
#include "db/lr2_export.h"
#include "sqlite3.h"
#include "util/data_dir.h"
#include "util/hash.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace rd {

namespace {
std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

int import_lr2_skin_custom(const std::string& lr2_dir, const std::string& skin_root, AppConfig& cfg) {
    fs::path dir = fs::path(lr2_dir) / "LR2files" / "SkinCustomize";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    SkinCatalog cat;
    cat.scan(skin_root.empty() ? lr2_dir : skin_root);
    fs::path root = fs::path(skin_root.empty() ? lr2_dir : skin_root);
    static const std::regex config_re("<config_(\\d+)>\\s*(-?\\d+)\\s*</config_"), file_re("<filename_(\\d+)>([^<]*)</filename_");
    int count = 0;
    for (const auto& [type, list] : cat.entries())
        for (const SkinEntry& e : list) {
            const bool play_skin = type >= 0 && type <= 4;  // プレイスキン（位置の調整も引き継ぐ）
            if (e.header.customs.empty() && !play_skin) continue;
            // LR2 はスキンを「LR2files\\Theme\\…」の形（CP932）で覚え、その MD5 をファイル名にする
            std::u8string rel = fs::path(e.path).lexically_relative(root).u8string();
            std::string key(rel.begin(), rel.end());
            std::replace(key.begin(), key.end(), '/', '\\');
            std::string xml = read_all(dir / (md5_hex(utf8_to_cp932(key)) + ".xml"));
            if (xml.empty()) continue;
            xml = to_utf8_auto(xml);
            if (play_skin) {
                // 位置の調整（LR2: SkinCustomize の shift・rate・judge・size・dark・note_1p）
                auto num = [&](const char* group, const char* item, int def) {
                    std::smatch m;
                    std::regex re(std::string("<") + group + ">[\\s\\S]*?<" + item + ">\\s*(-?\\d+)\\s*</" + item + ">[\\s\\S]*?</" + group + ">");
                    return std::regex_search(xml, m, re) ? std::stoi(m[1].str()) : def;
                };
                SkinAdjust a;
                a.shift_x = num("shift", "x", 0), a.shift_y = num("shift", "y", 0);
                a.rate_x = num("rate", "x", 100), a.rate_y = num("rate", "y", 100);
                a.judge_x = num("judge", "x", 0), a.judge_y = num("judge", "y", 0);
                a.size_x = num("size", "x", 0), a.size_y = num("size", "y", 0);
                a.dark = std::clamp(num("dark", "type", 0), 0, 2);
                a.note_x = num("note_1p", "x", 0), a.note_y = num("note_1p", "y", 0);
                if (!a.is_default()) cfg.skin_adjust[e.path] = a;
                else cfg.skin_adjust.erase(e.path);
            }
            if (e.header.customs.empty()) {
                count++;
                continue;
            }
            std::map<int, long long> values;
            std::map<int, std::string> files;
            for (auto it = std::sregex_iterator(xml.begin(), xml.end(), config_re); it != std::sregex_iterator(); ++it)
                values[std::stoi((*it)[1].str())] = std::stoll((*it)[2].str());
            for (auto it = std::sregex_iterator(xml.begin(), xml.end(), file_re); it != std::sregex_iterator(); ++it)
                files[std::stoi((*it)[1].str())] = (*it)[2].str();
            // 項目の番号は #CUSTOMOPTION と #CUSTOMFILE で共通。#CUSTOMOPTION は選択肢の番号、#CUSTOMFILE は名前で残す。
            // 選択肢に無い値は既定のまま
            SkinHeader h = e.header;
            for (size_t i = 0; i < h.customs.size(); ++i) {
                SkinCustomItem& c = h.customs[i];
                if (!c.is_file) {
                    auto it = values.find(static_cast<int>(i));
                    if (it != values.end() && it->second >= c.op_start && it->second < c.op_start + static_cast<long long>(c.labels.size()))
                        c.selected = static_cast<int>(it->second - c.op_start);
                } else if (auto it = files.find(static_cast<int>(i)); it != files.end()) {
                    for (size_t j = 0; j < c.labels.size(); ++j)
                        if (lower(c.labels[j]) == lower(it->second)) c.selected = static_cast<int>(j);
                }
            }
            cfg.skin_custom[e.path] = skin_custom_choices(h);
            count++;
        }
    return count;
}

int import_lr2_settings(const std::string& xml, AppConfig& cfg) {
    // <name>数</name> を、その節（<play> など）の中から読む
    auto section = [&](const char* name) {
        const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
        size_t a = xml.find(open), b = a == std::string::npos ? a : xml.find(close, a);
        return a == std::string::npos || b == std::string::npos ? std::string() : xml.substr(a, b - a);
    };
    int count = 0;
    auto num = [&](const std::string& sec, const char* tag, auto& field, int lo, int hi) {
        const std::regex re(std::string("<") + tag + ">\\s*(-?\\d+)\\s*</" + tag + ">");
        std::smatch m;
        if (!std::regex_search(sec, m, re)) return;
        int v = std::clamp(std::atoi(m[1].str().c_str()), lo, hi);
        using T = std::decay_t<decltype(field)>;
        field = static_cast<T>(v);
        ++count;
    };
    const std::string play = section("play"), sound = section("sound"), select = section("select"), system = section("system");
    // 遊び方（LR2: config.xml の <play>）
    num(play, "hs", cfg.hi_speed, 10, 990);
    num(play, "hsmax", cfg.hs_max, 10, 990);
    num(play, "hsmin", cfg.hs_min, 10, 990);
    num(play, "hsmargin", cfg.hs_margin, 1, 100);
    num(play, "hstype", cfg.hs_fix, 0, 4);
    num(play, "shutter", cfg.lane_cover_v, 0, 100);
    num(play, "shuttertype", cfg.lane_cover, 0, 1);
    num(play, "shuttermargin", cfg.shutter_margin, 1, 100);
    num(play, "gauge", cfg.gauge, 0, 5);
    num(play, "random", cfg.random, 0, 5);
    num(play, "effect", cfg.effect, 0, 3);
    num(play, "autojudgeadjust", cfg.auto_adjust, 0, 2);
    num(play, "judgetime", cfg.judge_timing, -199, 199);
    num(play, "bga", cfg.bga, 0, 3);
    num(play, "bgasize", cfg.bga_size, 0, 1);
    num(play, "poorbga", cfg.poor_bga_ms, 0, 10000);
    num(play, "ghost", cfg.ghost, 0, 3);
    num(play, "scoregraph", cfg.score_graph, 0, 1);
    num(play, "target", cfg.target, 0, 8);
    if (cfg.target > 5) cfg.target = 0;  // 6〜8 は IR のターゲット（RD には無い）
    num(play, "defaulttarget", cfg.target_percent, 50, 100);
    num(play, "replaysave", cfg.replay_save, 0, 4);
    num(play, "basespeed", cfg.base_speed, 10, 1000);
    num(play, "disableleftclickexit", cfg.no_right_click_exit, 0, 1);
    // 音（<sound>）
    num(sound, "volumeflag", cfg.volume_on, 0, 1);
    num(sound, "volumemaster", cfg.vol_master, 0, 100);
    num(sound, "volumekey", cfg.vol_key, 0, 100);
    num(sound, "volumebgm", cfg.vol_bgm, 0, 100);
    num(sound, "eqflag", cfg.sound_fx.eq_on, 0, 1);
    for (int i = 0; i < 7; ++i) num(sound, ("eqp" + std::to_string(i)).c_str(), cfg.sound_fx.eq[i], -100, 100);
    num(sound, "pitchflag", cfg.sound_fx.pitch_on, 0, 1);
    num(sound, "pitchtype", cfg.sound_fx.pitch_type, 0, 2);
    num(sound, "pitchp", cfg.sound_fx.pitch, -100, 100);
    for (int i = 0; i < 3; ++i) {
        FxSlot& f = cfg.sound_fx.fx[i];
        const std::string n = "_" + std::to_string(i);
        num(sound, ("fxflag" + n).c_str(), f.on, 0, 1);
        num(sound, ("fxtype" + n).c_str(), f.type, 0, 100);
        num(sound, ("fxtarget" + n).c_str(), f.target, 0, 100);
        num(sound, ("fxp1" + n).c_str(), f.p1, -1000, 1000);
        num(sound, ("fxp2" + n).c_str(), f.p2, -1000, 1000);
    }
    cfg.sound_fx.clamp();
    // 選曲（<select>）。ソートと鍵盤数の絞り込みは、RD では beatoraja の並びなので写さない
    num(select, "difficulty", cfg.select_difficulty, 0, 5);
    num(select, "searchmax", cfg.search_max, 0, 100000);
    num(select, "speedfirst", cfg.select_speed_first, 1, 5000);
    num(select, "speednext", cfg.select_speed_next, 1, 5000);
    num(select, "folderlamp", cfg.folder_lamp, 0, 1);
    num(select, "preview", cfg.preview, 0, 1);
    num(select, "levelbarflash_5", cfg.levelbar_5, 1, 99);
    num(select, "levelbarflash_7", cfg.levelbar_7, 1, 99);
    num(select, "levelbarflash_9", cfg.levelbar_9, 1, 99);
    // 画面など（<system>）
    num(system, "vsync", cfg.vsync, 0, 1);
    num(system, "inputinterval", cfg.input_interval, 0, 1000);
    num(system, "titleflash", cfg.title_flash, 0, 100000);
    return count;
}

namespace {

// DirectInput のキー番号 → SDL のスキャンコード（LR2 のキーボードの割り当て）
SDL_Scancode dik_to_scancode(int dik) {
    static const std::map<int, SDL_Scancode> table = {
        {0x01, SDL_SCANCODE_ESCAPE}, {0x02, SDL_SCANCODE_1}, {0x03, SDL_SCANCODE_2}, {0x04, SDL_SCANCODE_3},
        {0x05, SDL_SCANCODE_4}, {0x06, SDL_SCANCODE_5}, {0x07, SDL_SCANCODE_6}, {0x08, SDL_SCANCODE_7},
        {0x09, SDL_SCANCODE_8}, {0x0A, SDL_SCANCODE_9}, {0x0B, SDL_SCANCODE_0}, {0x0C, SDL_SCANCODE_MINUS},
        {0x0D, SDL_SCANCODE_EQUALS}, {0x0E, SDL_SCANCODE_BACKSPACE}, {0x0F, SDL_SCANCODE_TAB}, {0x10, SDL_SCANCODE_Q},
        {0x11, SDL_SCANCODE_W}, {0x12, SDL_SCANCODE_E}, {0x13, SDL_SCANCODE_R}, {0x14, SDL_SCANCODE_T},
        {0x15, SDL_SCANCODE_Y}, {0x16, SDL_SCANCODE_U}, {0x17, SDL_SCANCODE_I}, {0x18, SDL_SCANCODE_O},
        {0x19, SDL_SCANCODE_P}, {0x1A, SDL_SCANCODE_LEFTBRACKET}, {0x1B, SDL_SCANCODE_RIGHTBRACKET},
        {0x1C, SDL_SCANCODE_RETURN}, {0x1D, SDL_SCANCODE_LCTRL}, {0x1E, SDL_SCANCODE_A}, {0x1F, SDL_SCANCODE_S},
        {0x20, SDL_SCANCODE_D}, {0x21, SDL_SCANCODE_F}, {0x22, SDL_SCANCODE_G}, {0x23, SDL_SCANCODE_H},
        {0x24, SDL_SCANCODE_J}, {0x25, SDL_SCANCODE_K}, {0x26, SDL_SCANCODE_L}, {0x27, SDL_SCANCODE_SEMICOLON},
        {0x28, SDL_SCANCODE_APOSTROPHE}, {0x29, SDL_SCANCODE_GRAVE}, {0x2A, SDL_SCANCODE_LSHIFT},
        {0x2B, SDL_SCANCODE_BACKSLASH}, {0x2C, SDL_SCANCODE_Z}, {0x2D, SDL_SCANCODE_X}, {0x2E, SDL_SCANCODE_C},
        {0x2F, SDL_SCANCODE_V}, {0x30, SDL_SCANCODE_B}, {0x31, SDL_SCANCODE_N}, {0x32, SDL_SCANCODE_M},
        {0x33, SDL_SCANCODE_COMMA}, {0x34, SDL_SCANCODE_PERIOD}, {0x35, SDL_SCANCODE_SLASH}, {0x36, SDL_SCANCODE_RSHIFT},
        {0x37, SDL_SCANCODE_KP_MULTIPLY}, {0x38, SDL_SCANCODE_LALT}, {0x39, SDL_SCANCODE_SPACE},
        {0x3A, SDL_SCANCODE_CAPSLOCK}, {0x3B, SDL_SCANCODE_F1}, {0x3C, SDL_SCANCODE_F2}, {0x3D, SDL_SCANCODE_F3},
        {0x3E, SDL_SCANCODE_F4}, {0x3F, SDL_SCANCODE_F5}, {0x40, SDL_SCANCODE_F6}, {0x41, SDL_SCANCODE_F7},
        {0x42, SDL_SCANCODE_F8}, {0x43, SDL_SCANCODE_F9}, {0x44, SDL_SCANCODE_F10}, {0x45, SDL_SCANCODE_NUMLOCKCLEAR},
        {0x46, SDL_SCANCODE_SCROLLLOCK}, {0x47, SDL_SCANCODE_KP_7}, {0x48, SDL_SCANCODE_KP_8}, {0x49, SDL_SCANCODE_KP_9},
        {0x4A, SDL_SCANCODE_KP_MINUS}, {0x4B, SDL_SCANCODE_KP_4}, {0x4C, SDL_SCANCODE_KP_5}, {0x4D, SDL_SCANCODE_KP_6},
        {0x4E, SDL_SCANCODE_KP_PLUS}, {0x4F, SDL_SCANCODE_KP_1}, {0x50, SDL_SCANCODE_KP_2}, {0x51, SDL_SCANCODE_KP_3},
        {0x52, SDL_SCANCODE_KP_0}, {0x53, SDL_SCANCODE_KP_PERIOD}, {0x57, SDL_SCANCODE_F11}, {0x58, SDL_SCANCODE_F12},
        {0x70, SDL_SCANCODE_INTERNATIONAL2},  // カナ
        {0x73, SDL_SCANCODE_INTERNATIONAL1},  // ろ（＼_）
        {0x79, SDL_SCANCODE_INTERNATIONAL4},  // 変換
        {0x7B, SDL_SCANCODE_INTERNATIONAL5},  // 無変換
        {0x7D, SDL_SCANCODE_INTERNATIONAL3},  // ￥
        {0x90, SDL_SCANCODE_KP_EQUALS}, {0x91, SDL_SCANCODE_LEFTBRACKET},  // @（JIS）は [ の位置
        {0x92, SDL_SCANCODE_APOSTROPHE},                                  // :（JIS）
        {0x9C, SDL_SCANCODE_KP_ENTER}, {0x9D, SDL_SCANCODE_RCTRL}, {0xB5, SDL_SCANCODE_KP_DIVIDE},
        {0xB8, SDL_SCANCODE_RALT}, {0xC7, SDL_SCANCODE_HOME}, {0xC8, SDL_SCANCODE_UP}, {0xC9, SDL_SCANCODE_PAGEUP},
        {0xCB, SDL_SCANCODE_LEFT}, {0xCD, SDL_SCANCODE_RIGHT}, {0xCF, SDL_SCANCODE_END}, {0xD0, SDL_SCANCODE_DOWN},
        {0xD1, SDL_SCANCODE_PAGEDOWN}, {0xD2, SDL_SCANCODE_INSERT}, {0xD3, SDL_SCANCODE_DELETE},
        {0xDB, SDL_SCANCODE_LGUI}, {0xDC, SDL_SCANCODE_RGUI}, {0xDD, SDL_SCANCODE_APPLICATION}};
    auto it = table.find(dik);
    return it != table.end() ? it->second : SDL_SCANCODE_UNKNOWN;
}

}  // namespace

bool import_lr2_keyconfig(const std::string& path, KeyConfig& out) {
    std::string xml = read_all(fs::u8path(path));
    if (xml.empty()) return false;
    // LR2 の keyNN → RD のボタン（1P の分。key10 / key11 は皿の 2 方向、key12 が START、key13 が SELECT）
    const std::pair<const char*, int> slots[] = {{"key01", kKey1},     {"key02", kKey1 + 1}, {"key03", kKey1 + 2},
                                                 {"key04", kKey1 + 3}, {"key05", kKey1 + 4}, {"key06", kKey1 + 5},
                                                 {"key07", kKey1 + 6}, {"key10", kScratchA}, {"key11", kScratchB},
                                                 {"key12", kStart},    {"key13", kSelect}};
    KeyConfig k = out;
    clear_bindings(k);
    bool any = false;
    static const std::regex id_re("<id>\\s*(\\d+)\\s*</id>");
    for (const auto& [tag, button] : slots) {
        const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        size_t a = xml.find(open), b = a == std::string::npos ? a : xml.find(close, a);
        if (a == std::string::npos || b == std::string::npos) continue;
        const std::string body = xml.substr(a, b - a);
        for (auto it = std::sregex_iterator(body.begin(), body.end(), id_re); it != std::sregex_iterator(); ++it) {
            const int id = std::atoi((*it)[1].str().c_str());
            Binding bind;
            if (id > 0 && id < 0x100) {
                const SDL_Scancode sc = dik_to_scancode(id);
                if (sc == SDL_SCANCODE_UNKNOWN) continue;
                bind.type = Binding::Type::Key, bind.index = sc;
            } else if (id >= 0x100 && id < 0x200) {
                // パッド：32 個ずつ。1 台目はどのパッドでもよいことにし、2 台目からは機器の番号を付ける
                const int pad = (id - 0x100) / 0x20, bit = (id - 0x100) % 0x20;
                bind.device = pad == 0 ? 0 : pad + 1;
                if (bit == 0) bind.type = Binding::Type::JoyAxis, bind.index = 1, bind.dir = 1;        // 下
                else if (bit == 1) bind.type = Binding::Type::JoyAxis, bind.index = 0, bind.dir = -1;  // 左
                else if (bit == 2) bind.type = Binding::Type::JoyAxis, bind.index = 0, bind.dir = 1;   // 右
                else if (bit == 3) bind.type = Binding::Type::JoyAxis, bind.index = 1, bind.dir = -1;  // 上
                else bind.type = Binding::Type::JoyButton, bind.index = bit - 4;
            } else if (id >= 0x200 && id < 0x200 + 128) {
                bind.type = Binding::Type::Midi, bind.index = id - 0x200;
            } else {
                continue;
            }
            std::vector<Binding>& v = k.buttons[button];
            if (std::find(v.begin(), v.end(), bind) == v.end() && static_cast<int>(v.size()) < kKeySlots) v.push_back(bind), any = true;
        }
    }
    if (!any) return false;
    out = k;
    return true;
}

// LR2 の Score/<ID>.db ごとに、RD に同じ ID のプレイヤーを作り、その人の記録として取り込む（複数のアカウントも全部）
void import_lr2_players(const std::filesystem::path& score_dir, const std::string& songs_db, Lr2ImportResult& res,
                        const std::function<void(const std::string&)>& status) {
    std::string err;
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(score_dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".db") continue;
            const std::string id = to_utf8_auto(e.path().stem().string());
            if (!valid_player_id(id)) {
                std::printf("LR2 のプレイヤー %s は ID が RD で使えないので引き継がない\n", id.c_str());
                continue;
            }
            if (!fs::exists(fs::u8path(data_dir() + "Player/" + id), ec)) {
                if (!create_player(id, player_id(), &err)) {
                    std::printf("プレイヤー %s を作れない：%s\n", id.c_str(), err.c_str());
                    continue;
                }
                res.new_players.push_back(id);
            }
            if (status) status("LR2 のプレイヤー " + id + " の記録を取り込んでいます");
            SongDb pdb;
            int s = pdb.open(songs_db, id) ? pdb.import_lr2_scores(e.path().string(), &err) : -1;
            std::printf("LR2 のプレイヤー %s：記録 %d\n", id.c_str(), s);
            if (s >= 0) res.scores += s;
            res.players.push_back(id);
        }
        std::sort(res.players.begin(), res.players.end());
        // LR2 で使っていたプレイヤーが Score に無ければ（一度も遊んでいない）、その ID でも作る
        if (!res.player.empty() && valid_player_id(res.player) &&
            std::find(res.players.begin(), res.players.end(), res.player) == res.players.end()) {
            if (!fs::exists(fs::u8path(data_dir() + "Player/" + res.player), ec) && create_player(res.player, player_id(), &err))
                res.new_players.push_back(res.player);
            res.players.push_back(res.player);
        }
        if (res.player.empty() && !res.players.empty()) res.player = res.players.front();
    }
}

Lr2ImportResult import_from_lr2(const std::string& lr2_dir, AppConfig& cfg, SongDb& db,
                                const std::function<void(const std::string&)>& status, const std::string& skin_root) {
    Lr2ImportResult res;
    fs::path root = fs::path(lr2_dir) / "LR2files";
    std::string xml = read_all(root / "Config" / "config.xmh");
    if (xml.empty()) xml = read_all(root / "Config" / "config.xml");
    if (xml.empty()) return res.error = "LR2 の設定が見つからない（" + (root / "Config").string() + "）", res;
    xml = to_utf8_auto(xml);
    // 曲フォルダ（LR2 のカスタムフォルダの置き場は除く）
    std::vector<std::string> dirs, custom_dirs;
    static const std::regex path_re("<path>([^<]*)</path>");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), path_re); it != std::sregex_iterator(); ++it) {
        std::string p = (*it)[1].str();
        std::string low = p;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        // LR2files の下はカスタムフォルダの置き場（曲フォルダではない）
        if (low.find("lr2files") != std::string::npos) {
            custom_dirs.push_back(p);
            continue;
        }
        dirs.push_back(p);
    }
    std::smatch m;
    static const std::regex id_re("<player>\\s*<id>([^<]*)</id>");
    if (std::regex_search(xml, m, id_re)) res.player = m[1].str();
    // 今のプレイヤーと同じ ID の LR2 のプレイヤーがいれば、その人を引き継ぐ（LR2 で使っていたプレイヤーでなくても）
    if (std::error_code ec; !player_id().empty() && fs::exists(root / "Database" / "Score" / fs::u8path(player_id() + ".db"), ec))
        res.player = player_id();
    if (dirs.empty()) return res.error = "LR2 の設定に曲フォルダが無い", res;
    cfg.song_dirs = dirs;
    if (!res.player.empty()) cfg.lr2_player = res.player;  // LR2 の形で書き出すスコアも同じ名前にする
    cfg.custom_folder_dirs = custom_dirs;
    res.song_dirs = static_cast<int>(dirs.size());
    // 遊び方・音・選曲・画面の設定
    res.settings = import_lr2_settings(xml, cfg);
    // キーコンフィグ（RD のキーコンフィグを LR2 のものに置き換える）
    {
        KeyConfig k;
        if (!k.load(keyconfig_path())) k = KeyConfig::lr2_default();
        if (import_lr2_keyconfig((root / "Config" / "keyconfig.xml").string(), k)) res.keyconfig = k.save(keyconfig_path());
    }

    // 難易度表（すでに登録してあるものは足さない）
    for (const std::string& u : SongDb::lr2_table_urls((root / "Database" / "song.db").string()))
        if (std::find(cfg.tables.begin(), cfg.tables.end(), u) == cfg.tables.end()) cfg.tables.push_back(u), res.tables++;
    if (status) status("LR2 のスキンのカスタマイズを引き継いでいます");
    res.skins = import_lr2_skin_custom(lr2_dir, skin_root, cfg);
    if (status) status("LR2 の曲の情報を取り込んでいます");
    std::string err;
    int n = db.import_lr2_songs((root / "Database" / "song.db").string(), &err, {}, dirs);
    if (n < 0) return res.error = err, res;
    res.charts = n;
    if (status) status("LR2 の段位とコースを引き継いでいます");
    res.courses = std::max(0, import_lr2_courses((root / "Database" / "song.db").string()));
    if (status) status("LR2 のお気に入りを引き継いでいます");
    res.favorites = std::max(0, db.import_lr2_favorites((root / "Database" / "song.db").string()));
    import_lr2_players(root / "Database" / "Score", song_db_path(), res, status);
    std::printf("LR2 から引き継いだ：曲フォルダ %d、譜面 %d、記録 %d、プレイヤー %zu（使っていたのは %s）\n", res.song_dirs, res.charts,
                res.scores, res.players.size(), res.player.c_str());
    res.ok = true;
    return res;
}

namespace {

// パスを比べるための形（区切りを / にそろえ、英字は小文字。Windows は大文字小文字を区別しないため）
std::string path_key(const fs::path& p) {
    std::string k = p.lexically_normal().generic_string();
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

// p が root の下なら、root からの相対パスを返す
bool under(const fs::path& p, const fs::path& root, fs::path& rel) {
    std::string kp = path_key(p), kr = path_key(root);
    if (kp.size() < kr.size() || kp.compare(0, kr.size(), kr) != 0 || (kp.size() > kr.size() && kp[kr.size()] != '/')) return false;
    rel = fs::path(p).lexically_normal().lexically_relative(fs::path(root).lexically_normal());
    if (rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0) {
        // 大文字小文字の違いで lexically_relative が使えないときは、文字数で切る
        std::string g = fs::path(p).lexically_normal().generic_string();
        rel = fs::path(g.substr(std::min(g.size(), path_key(root).size() + 1)));
    }
    return true;
}

}  // namespace

AssetCopyResult copy_lr2_assets(AppConfig& cfg, const std::string& dst_root, const std::function<void(const std::string&)>& status) {
    AssetCopyResult res;
    std::error_code ec;
    const fs::path src = cfg.lr2_root;
    const fs::path dst = dst_root;
    if (cfg.lr2_root.empty() || !fs::is_directory(src / "LR2files", ec)) return res.error = "LR2 のフォルダが見つからない：" + cfg.lr2_root, res;
    if (path_key(src) == path_key(dst)) return res.error = "すでに RD のフォルダの素材を使っている", res;

    // 写すもの：見た目と音にかかわるものだけ（曲・記録・IR のデータは RD が別に持つ）
    static const char* kDirs[] = {"Theme", "Sound", "Bgm", "WallPaper", "Config", "Mouse", "Movie", "Help"};
    auto copy_tree = [&](const fs::path& from, const fs::path& to) {
        if (!fs::is_directory(from, ec)) return;
        for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            fs::path rel = it->path().lexically_relative(from);
            fs::path out = to / rel;
            fs::create_directories(out.parent_path(), e2);
            // すでに同じ大きさ・同じ日時以降のものがあれば写さない（何度実行してもよい）
            if (fs::exists(out, e2) && fs::file_size(out, e2) == it->file_size(e2) &&
                fs::last_write_time(out, e2) >= it->last_write_time(e2))
                continue;
            if (fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, e2)) {
                // 読み取り専用の印は写さない（LR2 のファイルに付いていると、RD のフォルダを消すときに権限を求められる）
                fs::permissions(out, fs::perms::owner_write, fs::perm_options::add, e2);
                res.files++;
                res.bytes += static_cast<int64_t>(it->file_size(e2));
                if (status && res.files % 200 == 0) status("素材を写しています（" + std::to_string(res.files) + " ファイル）");
            } else {
                res.failed++;
            }
        }
        ec.clear();
    };
    // 写す先は RD の置き場（RDfiles）。スキンの中の「LR2files/…」は、読むときにここへ読み替える
    const fs::path files = dst / data_dir_name();
    for (const char* d : kDirs) copy_tree(src / "LR2files" / d, files / d);

    // カスタムフォルダの置き場：LR2 の LR2files の下にあるものは、同じ相対の場所に写して付け替える
    for (std::string& d : cfg.custom_folder_dirs) {
        fs::path p = d, rel;
        for (fs::path cur = p; !cur.empty() && cur != cur.parent_path(); cur = cur.parent_path()) {
            if (path_key(cur.filename()) != "lr2files") continue;
            if (!under(p, cur, rel)) break;
            copy_tree(p, files / rel);
            d = (files / rel).string();
            break;
        }
    }

    // 設定の中の素材の場所を、写した先に直す
    auto relocate = [&](std::string& path) {
        fs::path rel;
        if (path.empty() || path == "none") return;
        if (under(path, src / "LR2files", rel)) path = (files / rel).string();
        else if (under(path, src, rel)) path = (dst / rel).string();
    };
    for (std::string* p : {&cfg.play_skin, &cfg.result_skin, &cfg.courseresult_skin, &cfg.select_skin, &cfg.decide_skin,
                           &cfg.keyconfig_skin, &cfg.skinselect_skin, &cfg.sound_set})
        relocate(*p);
    auto relocate_keys = [&](auto& m) {
        std::remove_reference_t<decltype(m)> out;
        for (auto& [k, v] : m) {
            std::string nk = k;
            relocate(nk);
            out[nk] = v;
        }
        m.swap(out);
    };
    relocate_keys(cfg.skin_custom);
    relocate_keys(cfg.skin_adjust);
    cfg.lr2_root = dst.string();
    res.ok = true;
    return res;
}

int import_lr2_courses(const std::string& lr2_song_db) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(lr2_song_db.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    std::vector<TableCourse> courses;
    for (const char* table : {"grade", "expert"}) {
        std::string sql = std::string("SELECT title, hash FROM ") + table + " ORDER BY id";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            const unsigned char* h = sqlite3_column_text(st, 1);
            if (!t || !h) continue;
            TableCourse c;
            c.name = to_utf8_auto(reinterpret_cast<const char*>(t));
            if (std::string(table) == "grade") c.constraints = {"grade_mirror", "gauge_lr2"};
            // hash は「コースの印 32 文字」のあとに、ステージの譜面の md5 が 32 文字ずつ続く
            std::string hash = reinterpret_cast<const char*>(h);
            for (size_t i = 32; i + 32 <= hash.size(); i += 32) c.charts.push_back(TableChart{}), c.charts.back().md5 = hash.substr(i, 32);
            if (!c.charts.empty()) courses.push_back(std::move(c));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!write_lr2_courses(lr2_courses_path(), courses)) return -1;
    return static_cast<int>(courses.size());
}

void write_lr2_files(const SongDb& db, const AppConfig& cfg, bool songs) {
    if (!cfg.lr2_export) return;
    const uint64_t t0 = SDL_GetTicksNS();
    std::string err;
    if (!export_lr2_files(db, lr2_database_dir(), cfg.song_dirs, cfg.lr2_player, songs, &err))
        std::printf("LR2 の形で書き出せない: %s\n", err.c_str());
    else
        std::printf("LR2 の形で書き出した（%s）：%.0f ms\n", songs ? "曲とスコア" : "スコア", (SDL_GetTicksNS() - t0) / 1e6);
}

}  // namespace rd
