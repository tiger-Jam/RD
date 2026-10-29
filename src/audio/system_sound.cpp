#include "audio/system_sound.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include "skin/lr2_skin.h"
#include "util/text.h"

namespace rd {

namespace {

struct Entry {
    const char* name;
    int id;
};
// 長い名前を先に比べる（#SELECT と #EXSELECT、#DECIDE と #EXDECIDE など）
const Entry kEntries[] = {
    {"EXSELECT", kSysExSelect}, {"EXDECIDE", kSysExDecide}, {"SELECT", kSysSelect}, {"DECIDE", kSysDecide},
    {"FOLDER_OPEN", kSysFolderOpen}, {"FOLDER_CLOSE", kSysFolderClose}, {"PANEL_OPEN", kSysPanelOpen},
    {"PANEL_CLOSE", kSysPanelClose}, {"OPTION_CHANGE", kSysOptionChange}, {"DIFFICULTY", kSysDifficulty},
    {"SCREENSHOT", kSysScreenshot}, {"COURSECLEAR", kSysCourseClear}, {"COURSEFAIL", kSysCourseFail},
    {"CLEAR", kSysClear}, {"FAIL", kSysFail}, {"STOP", kSysStop}, {"MINE", kSysMine}, {"SCRATCH", kSysScratch},
};

// LR2のパスを解決する。見つからなければ拡張子を wav ↔ ogg に替えて探す（LR2: IsAltSoundExist）
std::string resolve_sound(const std::string& root, const std::string& p) {
    std::string r = resolve_lr2_path(root, p);
    if (!r.empty()) return r;
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) return {};
    for (const char* ext : {".ogg", ".wav"}) {
        r = resolve_lr2_path(root, p.substr(0, dot) + ext);
        if (!r.empty()) return r;
    }
    return {};
}

}  // namespace

namespace {

// サウンドセットの行を読む。#CUSTOMFILE は custom(検索パターン, 既定の名前)、音の行は sound(音の番号, パス) を呼ぶ
void read_lr2ss(const std::string& lr2ss_path, const std::function<void(const std::string&, const std::string&)>& custom,
                const std::function<void(int, const std::string&)>& sound) {
    std::ifstream f(lr2ss_path, std::ios::binary);
    if (!f) return;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::istringstream in(to_utf8_auto(bytes));
    std::string line;
    while (std::getline(in, line)) {
        std::string_view l = trim(line);
        if (l.empty() || l.rfind("//", 0) == 0) continue;
        if (l[0] == '#') l.remove_prefix(1);  // 末尾の「保険」の行は # が無い
        std::vector<std::string> cols;
        std::string cur;
        for (char c : l) {
            if (c == ',') cols.push_back(cur), cur.clear();
            else cur.push_back(c);
        }
        cols.push_back(cur);
        std::string cmd = to_upper_ascii(trim(cols[0]));
        if (cmd == "CUSTOMFILE" && cols.size() >= 4) {
            custom(std::string(trim(cols[2])), std::string(trim(cols[3])));
            continue;
        }
        if (cols.size() < 2) continue;
        for (const Entry& e : kEntries)
            if (cmd == e.name) {
                sound(e.id, std::string(trim(cols[1])));
                break;
            }
    }
}

// パスが #CUSTOMFILE の検索パターン（'*' の前まで）で始まるなら、その項目の番号
int custom_of(const std::vector<std::string>& patterns, const std::string& path) {
    for (size_t k = 0; k < patterns.size(); ++k)
        if (path.rfind(patterns[k].substr(0, patterns[k].find('*')), 0) == 0) return static_cast<int>(k);
    return -1;
}

}  // namespace

std::vector<Sample> load_system_sounds(const std::string& lr2_root, const std::string& lr2ss_path, uint32_t rate,
                                       const std::vector<std::string>& chosen) {
    std::vector<Sample> out(kSysSoundCount);
    std::vector<std::string> patterns, names;  // #CUSTOMFILE の検索パターンと、選んだ名前（無ければ既定の名前）
    std::vector<bool> done(kSysSoundCount, false);
    read_lr2ss(
        lr2ss_path,
        [&](const std::string& pat, const std::string& def) {
            const size_t k = patterns.size();
            patterns.push_back(pat);
            names.push_back(k < chosen.size() && !chosen[k].empty() ? chosen[k] : def);
        },
        [&](int id, std::string p) {
            if (done[id]) return;
            // #CUSTOMFILE の検索パターンで始まるパスは、'*' を選んだ名前に置き換える
            if (int k = custom_of(patterns, p); k >= 0 && !names[k].empty())
                if (size_t star = p.find('*'); star != std::string::npos) p.replace(star, 1, names[k]);
            std::string path = resolve_sound(lr2_root, p);
            std::string err;
            if (!path.empty() && decode_sound_file(path, rate, out[id], err)) done[id] = true;
        });
    return out;
}

std::string system_sound_file(const std::string& lr2_root, const std::string& lr2ss_path, int id, const std::vector<std::string>& chosen) {
    std::vector<std::string> patterns, names;
    std::string out;
    read_lr2ss(
        lr2ss_path,
        [&](const std::string& pat, const std::string& def) {
            const size_t k = patterns.size();
            patterns.push_back(pat);
            names.push_back(k < chosen.size() && !chosen[k].empty() ? chosen[k] : def);
        },
        [&](int sid, std::string p) {
            if (sid != id || !out.empty()) return;
            if (int k = custom_of(patterns, p); k >= 0 && !names[k].empty())
                if (size_t star = p.find('*'); star != std::string::npos) p.replace(star, 1, names[k]);
            out = resolve_sound(lr2_root, p);
        });
    return out;
}

int system_sound_custom_item(const std::string& lr2ss_path, int id) {
    std::vector<std::string> patterns;
    int item = -2;  // まだ見ていない
    read_lr2ss(
        lr2ss_path, [&](const std::string& pat, const std::string&) { patterns.push_back(pat); },
        [&](int sid, const std::string& p) {
            if (sid == id && item == -2) item = custom_of(patterns, p);
        });
    return item < 0 ? -1 : item;
}

}  // namespace rd
