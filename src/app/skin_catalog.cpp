#include "app/skin_catalog.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// パスを比べられる形にする（大文字小文字と区切りの違いを無くす）
std::string key_of(const std::string& p) {
    std::error_code ec;
    fs::path q = fs::weakly_canonical(fs::path(p), ec);
    return lower((ec ? fs::path(p) : q).lexically_normal().generic_string());
}

}  // namespace

std::string* SkinCatalog::config_slot(AppConfig& cfg, int type) {
    switch (type) {
    case 0: return &cfg.play_skin;
    case 5: return &cfg.select_skin;
    case 6: return &cfg.decide_skin;
    case 7: return &cfg.result_skin;
    case 10: return &cfg.sound_set;
    default: return nullptr;
    }
}

const std::string* SkinCatalog::config_slot(const AppConfig& cfg, int type) {
    return config_slot(const_cast<AppConfig&>(cfg), type);
}

std::string SkinCatalog::default_path(int type) const {
    const char* rel = nullptr;
    switch (type) {
    case 0: rel = "LR2files/Theme/LR2/Play/play_7.lr2skin"; break;
    case 5: rel = "LR2files/Theme/LR2/Select/select.lr2skin"; break;
    case 6: rel = "LR2files/Theme/LR2/Decide/decide.lr2skin"; break;
    case 7: rel = "LR2files/Theme/LR2/Result/result.lr2skin"; break;
    case 10: rel = "LR2files/Sound/lr2.lr2ss"; break;
    default: return {};
    }
    return resolve_lr2_path(root_, rel, false);
}

void SkinCatalog::scan(const std::string& lr2_root) {
    root_ = lr2_root;
    by_type_.clear();
    if (lr2_root.empty()) return;
    auto add_dir = [&](const fs::path& dir, const char* ext) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ext) continue;
            SkinEntry e;
            e.path = it->path().string();
            if (read_skin_header(lr2_root, e.path, e.header) && e.header.type >= 0) by_type_[e.header.type].push_back(std::move(e));
        }
    };
    add_dir(fs::path(lr2_root) / "LR2files" / "Theme", ".lr2skin");
    add_dir(fs::path(lr2_root) / "LR2files" / "Sound", ".lr2ss");
    for (auto& [t, v] : by_type_)
        std::sort(v.begin(), v.end(), [](const SkinEntry& a, const SkinEntry& b) { return a.header.title < b.header.title; });
}

void SkinCatalog::select_type(int type, const AppConfig&) {
    type_ = type;
    scroll_ = 0;
}

const SkinEntry* SkinCatalog::current(const AppConfig& cfg) const {
    auto it = by_type_.find(type_);
    if (it == by_type_.end() || it->second.empty()) return nullptr;
    const std::string* slot = config_slot(cfg, type_);
    std::string want = slot && !slot->empty() ? *slot : default_path(type_);
    if (want.empty()) return &it->second.front();
    std::string k = key_of(want);
    for (const SkinEntry& e : it->second)
        if (key_of(e.path) == k) return &e;
    return &it->second.front();
}

bool SkinCatalog::step_skin(AppConfig& cfg, int dir) {
    auto it = by_type_.find(type_);
    std::string* slot = config_slot(cfg, type_);
    if (it == by_type_.end() || it->second.empty() || !slot) return false;
    const std::vector<SkinEntry>& list = it->second;
    const SkinEntry* cur = current(cfg);
    int n = static_cast<int>(list.size());
    int i = cur ? static_cast<int>(cur - list.data()) : 0;
    i = ((i + dir) % n + n) % n;
    *slot = list[i].path;
    scroll_ = 0;
    return true;
}

std::vector<int> SkinCatalog::selections(const SkinEntry& e, const AppConfig& cfg) const {
    std::vector<int> sel;
    auto it = cfg.skin_custom.find(e.path);
    for (size_t i = 0; i < e.header.customs.size(); ++i) {
        int n = static_cast<int>(e.header.customs[i].labels.size());
        int v = it != cfg.skin_custom.end() && i < it->second.size() ? it->second[i] : e.header.customs[i].selected;
        sel.push_back(n > 0 ? std::clamp(v, 0, n - 1) : 0);
    }
    return sel;
}

bool SkinCatalog::step_custom(AppConfig& cfg, int k, int dir) {
    const SkinEntry* e = current(cfg);
    if (!e) return false;
    size_t idx = static_cast<size_t>(scroll_ + k);
    if (idx >= e->header.customs.size()) return false;
    int n = static_cast<int>(e->header.customs[idx].labels.size());
    if (n <= 0) return false;
    std::vector<int> sel = selections(*e, cfg);
    sel[idx] = ((sel[idx] + dir) % n + n) % n;
    cfg.skin_custom[e->path] = sel;
    return true;
}

int SkinCatalog::scroll_max(const AppConfig& cfg) const {
    const SkinEntry* e = current(cfg);
    return e ? std::max(0, static_cast<int>(e->header.customs.size()) - 5) : 0;
}

void SkinCatalog::set_scroll(int v, const AppConfig& cfg) { scroll_ = std::clamp(v, 0, scroll_max(cfg)); }

std::string SkinCatalog::text(int id, const AppConfig& cfg) const {
    const SkinEntry* e = current(cfg);
    if (!e) return {};
    if (id == 50) return e->header.title;
    if (id == 51) return e->header.maker;
    if (id >= 100 && id <= 114) {
        size_t idx = static_cast<size_t>(scroll_ + (id % 10));
        if (idx >= e->header.customs.size()) return {};
        const SkinCustomItem& c = e->header.customs[idx];
        if (id < 110) return c.title;
        std::vector<int> sel = selections(*e, cfg);
        return c.labels.empty() ? std::string() : c.labels[sel[idx]];
    }
    return {};
}

int SkinCatalog::button(int t) const {
    if (t >= 170 && t <= 188) return type_ == t - 170 ? 1 : 0;
    return 0;
}

}  // namespace bmj
