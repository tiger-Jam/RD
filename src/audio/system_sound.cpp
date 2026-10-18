#include "audio/system_sound.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "skin/lr2_skin.h"
#include "util/text.h"

namespace bmj {

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

std::vector<Sample> load_system_sounds(const std::string& lr2_root, const std::string& lr2ss_path, uint32_t rate) {
    std::vector<Sample> out(kSysSoundCount);
    std::ifstream f(lr2ss_path, std::ios::binary);
    if (!f) return out;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::istringstream in(to_utf8_auto(bytes));
    std::vector<std::pair<std::string, std::string>> custom;  // 検索パターン → 既定の名前
    std::vector<bool> done(kSysSoundCount, false);
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
            custom.push_back({std::string(trim(cols[2])), std::string(trim(cols[3]))});
            continue;
        }
        if (cols.size() < 2) continue;
        for (const Entry& e : kEntries) {
            if (cmd != e.name || done[e.id]) continue;
            std::string p(trim(cols[1]));
            // #CUSTOMFILE の検索パターンで始まるパスは、'*' を既定の名前に置き換える
            for (auto& [pat, chosen] : custom) {
                std::string prefix = pat.substr(0, pat.find('*'));
                if (!chosen.empty() && p.rfind(prefix, 0) == 0) {
                    size_t star = p.find('*');
                    if (star != std::string::npos) p.replace(star, 1, chosen);
                    break;
                }
            }
            std::string path = resolve_sound(lr2_root, p);
            std::string err;
            if (!path.empty() && decode_sound_file(path, rate, out[e.id], err)) done[e.id] = true;
            break;
        }
    }
    return out;
}

}  // namespace bmj
