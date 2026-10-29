#include "app/skin_catalog.h"

#include "skin/skin_compose.h"

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

void apply_skin_custom(const std::vector<SkinCustomChoice>& sel, SkinHeader& h) {
    for (size_t i = 0; i < sel.size() && i < h.customs.size(); ++i) {
        SkinCustomItem& c = h.customs[i];
        const SkinCustomChoice& v = sel[i];
        if (c.is_file && !v.file.empty()) {
            // 名前で探す。候補に無ければ既定のまま（LR2: mng:362-371）
            for (size_t j = 0; j < c.labels.size(); ++j)
                if (lower(c.labels[j]) == lower(v.file)) c.selected = static_cast<int>(j);
        } else if (v.file.empty() && v.index >= 0 && v.index < static_cast<int>(c.labels.size())) {
            c.selected = v.index;
        }
    }
}

void apply_skin_custom(const AppConfig& cfg, const std::string& skin_path, SkinHeader& h) {
    if (auto it = cfg.skin_custom.find(skin_path); it != cfg.skin_custom.end()) apply_skin_custom(it->second, h);
}

std::vector<SkinCustomChoice> skin_custom_choices(const SkinHeader& h) {
    std::vector<SkinCustomChoice> out;
    for (const SkinCustomItem& c : h.customs) {
        SkinCustomChoice v;
        if (c.is_file && c.selected >= 0 && c.selected < static_cast<int>(c.labels.size())) v.file = c.labels[c.selected];
        else v.index = c.is_file ? -1 : c.selected;  // -1：既定が候補に無いので既定のまま
        out.push_back(v);
    }
    return out;
}

std::string* SkinCatalog::config_slot(AppConfig& cfg, int type) {
    switch (type) {
    case 0: return &cfg.play_skin;
    case 5: return &cfg.select_skin;
    case 6: return &cfg.decide_skin;
    case 7: return &cfg.result_skin;
    case 8: return &cfg.keyconfig_skin;
    case 9: return &cfg.skinselect_skin;
    case 15: return &cfg.courseresult_skin;
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
    case 8: rel = "LR2files/Theme/LR2/KeyConfig/keyconfig.lr2skin"; break;
    case 9: rel = "LR2files/Theme/LR2/SkinSelect/skinselect.lr2skin"; break;
    case 15: rel = "LR2files/Theme/LR2/CourseResult/courseresult.lr2skin"; break;
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
    SkinHeader h = e.header;
    apply_skin_custom(cfg, e.path, h);
    std::vector<int> sel;
    for (const SkinCustomItem& c : h.customs) {
        int n = static_cast<int>(c.labels.size());
        // -1（#CUSTOMFILE の既定が候補に無い）はそのまま残す
        sel.push_back(n > 0 ? std::clamp(c.selected, -1, n - 1) : 0);
    }
    return sel;
}

bool SkinCatalog::step_custom(AppConfig& cfg, int k, int dir) {
    const SkinEntry* e = current(cfg);
    if (!e) return false;
    size_t idx = static_cast<size_t>(scroll_ + k);
    if (idx == 0 && part_rows() > 0) {
        // 部品の行の開け閉め（設定は変えない）
        parts_open_ = !parts_open_;
        set_scroll(scroll_, cfg);
        return true;
    }
    if (static_cast<int>(idx) < part_rows()) return step_part(cfg, *e, static_cast<int>(idx) - 1, dir);
    idx -= static_cast<size_t>(part_rows());
    if (idx >= e->header.customs.size()) return false;
    int n = static_cast<int>(e->header.customs[idx].labels.size());
    if (n <= 0) return false;
    std::vector<int> sel = selections(*e, cfg);
    sel[idx] = sel[idx] < 0 ? (dir > 0 ? 0 : n - 1) : ((sel[idx] + dir) % n + n) % n;
    SkinHeader h = e->header;
    for (size_t i = 0; i < sel.size(); ++i) h.customs[i].selected = sel[i];
    cfg.skin_custom[e->path] = skin_custom_choices(h);
    return true;
}

int SkinCatalog::scroll_max(const AppConfig& cfg) const {
    const SkinEntry* e = current(cfg);
    return e ? std::max(0, static_cast<int>(e->header.customs.size()) + part_rows() - 5) : 0;
}

int SkinCatalog::part_rows() const { return type_ != 0 ? 0 : parts_open_ ? kPlayPartSlotCount + 2 : 1; }

std::string SkinCatalog::skin_label(const SkinEntry& e) const {
    // 同じ名前のスキンが多い（W-mix の 1P・2P・DP など）ので、名前が重なればファイルの名前を付ける
    int same = 0;
    if (auto it = by_type_.find(0); it != by_type_.end())
        for (const SkinEntry& o : it->second) same += o.header.title == e.header.title;
    if (same <= 1) return e.header.title;
    return e.header.title + " (" + fs::u8path(e.path).stem().string() + ")";
}

const SkinEntry* SkinCatalog::play_entry(const std::string& path) const {
    auto it = by_type_.find(0);
    if (it == by_type_.end()) return nullptr;
    const std::string k = key_of(path);
    for (const SkinEntry& o : it->second)
        if (key_of(o.path) == k) return &o;
    return nullptr;
}

std::vector<int> SkinCatalog::part_customs(const SkinEntry& src, const AppConfig& cfg, const std::string& part) const {
    std::string key = src.path;
    for (int v : selections(src, cfg)) key += "," + std::to_string(v);
    auto it = part_customs_.find(key);
    if (it == part_customs_.end()) {
        // 画像は読まずに命令の並びだけ読み、部品の絵がどの #CUSTOMFILE で選ばれたファイルかを見る
        it = part_customs_.try_emplace(key).first;
        SkinHeader h = src.header;
        apply_skin_custom(cfg, src.path, h);
        Lr2Skin sk;
        std::string err;
        if (load_lr2_skin(root_, src.path, h, std::array<bool, 1000>{}, sk, err, nullptr, false))
            for (const PlayPartSlot& slot : kPlayPartSlots) {
                std::vector<int>& v = it->second[slot.key];
                for (int i : play_part_customs(sk, slot.key))
                    // 候補が RANDOM だけの項目は選べないので出さない
                    if (i < static_cast<int>(h.customs.size()) && h.customs[i].is_file && h.customs[i].labels.size() > 1) v.push_back(i);
            }
    }
    auto f = it->second.find(part);
    return f != it->second.end() ? f->second : std::vector<int>{};
}

std::string SkinCatalog::custom_label(const SkinEntry& src, const AppConfig& cfg, int item) const {
    const SkinCustomItem& c = src.header.customs[item];
    const int sel = selections(src, cfg)[item];
    if (sel >= 0 && sel < static_cast<int>(c.labels.size())) return c.labels[sel];
    return c.default_value.empty() ? std::string("RANDOM") : c.default_value;
}

std::string SkinCatalog::part_text(int row, bool value, const AppConfig& cfg, const SkinEntry& e) const {
    if (row == kPlayPartSlotCount) {
        if (!value) return "PARTS SIZE";
        auto it = cfg.skin_parts.find(e.path);
        return it != cfg.skin_parts.end() && it->second.screen_scale ? "SCREEN" : "LANE";
    }
    if (!value) return kPlayPartSlots[row].name;
    auto it = cfg.skin_parts.find(e.path);
    if (it == cfg.skin_parts.end()) return "-";
    const std::string key = kPlayPartSlots[row].key;
    auto f = it->second.from.find(key);
    if (f == it->second.from.end()) return "-";
    const SkinEntry* o = play_entry(f->second);
    if (!o) return fs::u8path(f->second).filename().string();
    // スキンの名前と、部品の絵の種類（「W-mix play AC / splash_red」）
    std::string s = skin_label(*o);
    auto c = it->second.custom.find(key);
    for (int i : part_customs(*o, cfg, key)) {
        std::string name = custom_label(*o, cfg, i);
        if (c != it->second.custom.end())
            if (auto n = c->second.find(i); n != c->second.end()) name = n->second;
        s += " / " + name;
    }
    return s;
}

bool SkinCatalog::step_part(AppConfig& cfg, const SkinEntry& e, int row, int dir) {
    AppConfig::SkinParts& p = cfg.skin_parts[e.path];
    if (row == kPlayPartSlotCount) {
        p.screen_scale = !p.screen_scale;
    } else {
        // 選べるもの：「-」（このスキンのまま）と、ほかの 7 鍵のプレースキン。スキンごとに、部品の絵を選ぶ #CUSTOMFILE の
        // 選択肢を順に並べる（そのスキン自身のカスタマイズで選んでいるものを先に）
        struct Choice {
            std::string path;
            std::map<int, std::string> custom;
        };
        const std::string key = kPlayPartSlots[row].key;
        std::vector<Choice> choices = {{}};
        int cur = 0;
        const auto f = p.from.find(key);
        const auto fc = p.custom.find(key);
        for (const SkinEntry& o : by_type_.at(0)) {
            if (key_of(o.path) == key_of(e.path)) continue;
            const bool now = f != p.from.end() && key_of(f->second) == key_of(o.path);
            std::vector<Choice> list = {{o.path, {}}};
            std::map<int, std::string> want;  // 今の選択
            for (int i : part_customs(o, cfg, key)) {
                const std::string mine = custom_label(o, cfg, i);
                want[i] = mine;
                if (now && fc != p.custom.end())
                    if (auto n = fc->second.find(i); n != fc->second.end()) want[i] = n->second;
                std::vector<std::string> labels = o.header.customs[i].labels;
                if (auto at = std::find(labels.begin(), labels.end(), mine); at != labels.end()) std::rotate(labels.begin(), at, labels.end());
                std::vector<Choice> next;
                for (const Choice& c : list)
                    for (const std::string& l : labels) {
                        if (next.size() >= 256) break;  // 項目が多くても送り切れる数まで
                        next.push_back(c), next.back().custom[i] = l;
                    }
                list = std::move(next);
            }
            if (now)
                for (size_t j = 0; j < list.size(); ++j)
                    if (list[j].custom == want) cur = static_cast<int>(choices.size() + j);
            if (now && cur == 0) cur = static_cast<int>(choices.size());
            choices.insert(choices.end(), list.begin(), list.end());
        }
        const int n = static_cast<int>(choices.size());
        cur = ((cur + dir) % n + n) % n;
        p.custom.erase(key);
        if (choices[cur].path.empty()) {
            p.from.erase(key);
        } else {
            p.from[key] = choices[cur].path;
            if (!choices[cur].custom.empty()) p.custom[key] = choices[cur].custom;
        }
    }
    if (p.empty()) cfg.skin_parts.erase(e.path);
    return true;
}

void SkinCatalog::set_scroll(int v, const AppConfig& cfg) { scroll_ = std::clamp(v, 0, scroll_max(cfg)); }

std::string SkinCatalog::text(int id, const AppConfig& cfg) const {
    const SkinEntry* e = current(cfg);
    if (!e) return {};
    if (id == 50) return e->header.title;
    if (id == 51) return e->header.maker;
    if (id >= 100 && id <= 114) {
        // 部品の行を先に、スキンのカスタマイズの項目をその後ろに並べる（項目の多いスキンでも部品がすぐ見えるように）
        size_t idx = static_cast<size_t>(scroll_ + (id % 10));
        if (idx == 0 && part_rows() > 0) {
            if (id < 110) return "SKIN PARTS";
            if (parts_open_) return "CLOSE";
            // たたんでいるときは、ほかのスキンから持ってきている部品の数
            auto it = cfg.skin_parts.find(e->path);
            const size_t n = it != cfg.skin_parts.end() ? it->second.from.size() : 0;
            return n > 0 ? "OPEN (" + std::to_string(n) + " CHANGED)" : "OPEN";
        }
        if (static_cast<int>(idx) < part_rows()) return part_text(static_cast<int>(idx) - 1, id >= 110, cfg, *e);
        idx -= static_cast<size_t>(part_rows());
        if (idx >= e->header.customs.size()) return {};
        const SkinCustomItem& c = e->header.customs[idx];
        if (id < 110) return c.title;
        std::vector<int> sel = selections(*e, cfg);
        if (c.labels.empty()) return {};
        // 既定が候補に無いときは既定の文字列（空なら読むたびにランダムなので RANDOM と出す）
        if (sel[idx] < 0) return c.default_value.empty() ? std::string("RANDOM") : c.default_value;
        return c.labels[sel[idx]];
    }
    return {};
}

int SkinCatalog::button(int t) const {
    if (t >= 170 && t <= 188) return type_ == t - 170 ? 1 : 0;
    return 0;
}

}  // namespace bmj
