#include "app/bgm_set.h"

#include <cctype>

#include "app/skin_catalog.h"
#include "audio/sound_bank.h"
#include "audio/system_sound.h"
#include "util/data_dir.h"

namespace rd {

namespace {

// #CUSTOMFILE だけを数えた番号を、ヘッダの項目の番号にする（無ければ -1）
int header_index(const SkinHeader& h, int file_item) {
    int k = 0;
    for (size_t i = 0; i < h.customs.size(); ++i)
        if (h.customs[i].is_file && k++ == file_item) return static_cast<int>(i);
    return -1;
}

}  // namespace

bool same_bgm_key(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i] == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        char y = b[i] == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (x != y) return false;
    }
    return true;
}

std::vector<BgmChoice> bgm_choices(const std::string& lr2_root, int sys_id) {
    std::vector<BgmChoice> out;
    SkinCatalog catalog;
    catalog.scan(lr2_root);
    auto it = catalog.entries().find(10);
    if (it == catalog.entries().end()) return out;
    for (const SkinEntry& e : it->second) {
        const std::string base = to_portable(e.path);
        const std::string title = e.header.title.empty() ? e.path : e.header.title;
        const int item = system_sound_custom_item(e.path, sys_id);
        const int hi = header_index(e.header, item);
        if (hi < 0) {
            out.push_back({base + "|-1|", title});
            continue;
        }
        for (const std::string& label : e.header.customs[hi].labels)
            if (label != "RANDOM") out.push_back({base + "|" + std::to_string(item) + "|" + label, title + " / " + label});
    }
    return out;
}

bool load_bgm_choice(const std::string& lr2_root, const AppConfig& cfg, const std::string& key, int sys_id, uint32_t rate, Sample& out) {
    const size_t a = key.find('|'), b = a == std::string::npos ? a : key.find('|', a + 1);
    if (b == std::string::npos) return false;
    const std::string ss = from_portable(key.substr(0, a));
    const int item = std::atoi(key.substr(a + 1, b - a - 1).c_str());
    const std::string label = key.substr(b + 1);
    // そのサウンドセットのカスタマイズ（設定にあればそれ）に、選んだ候補を重ねる
    SkinHeader header;
    if (!read_skin_header(lr2_root, ss, header)) return false;
    apply_skin_custom(cfg, ss, header);
    std::vector<std::string> chosen;
    for (const SkinCustomItem& c : header.customs)
        if (c.is_file) chosen.push_back(c.selected >= 0 && c.selected < static_cast<int>(c.labels.size()) ? c.labels[c.selected] : "");
    if (item >= 0 && item < static_cast<int>(chosen.size())) chosen[item] = label;
    const std::string file = system_sound_file(lr2_root, ss, sys_id, chosen);  // その 1 曲だけを読む
    std::string err;
    return !file.empty() && decode_sound_file(file, rate, out, err);
}

}  // namespace rd
