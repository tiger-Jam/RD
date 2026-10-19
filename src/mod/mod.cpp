#include "mod/mod.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "util/hash.h"
#include "yyjson.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}

std::string str(yyjson_val* o, const char* key, const std::string& def = {}) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v)) : def;
}
double num(yyjson_val* o, const char* key, double def) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_num(v) ? yyjson_get_num(v) : def;
}
bool boolean(yyjson_val* o, const char* key, bool def) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}

uint32_t parse_color(const std::string& s, uint32_t def) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return def;
    char* end = nullptr;
    unsigned long v = std::strtoul(h.c_str(), &end, 16);
    return end && *end == 0 ? static_cast<uint32_t>(v) : def;
}

// 増減の値：数か {"fixed": 数, "total": 数}
GaugeDelta parse_delta(yyjson_val* v) {
    GaugeDelta d;
    if (yyjson_is_num(v)) d.fixed = yyjson_get_num(v);
    else if (yyjson_is_obj(v)) d.fixed = num(v, "fixed", 0), d.total = num(v, "total", 0);
    return d;
}

// [早い側, 遅い側] か、1 つの数（両側とも同じ）
void parse_window(yyjson_val* o, const char* key, int out[2]) {
    yyjson_val* v = yyjson_obj_get(o, key);
    if (yyjson_is_num(v)) out[0] = out[1] = static_cast<int>(yyjson_get_num(v));
    else if (yyjson_is_arr(v) && yyjson_arr_size(v) == 2)
        out[0] = static_cast<int>(yyjson_get_num(yyjson_arr_get(v, 0))), out[1] = static_cast<int>(yyjson_get_num(yyjson_arr_get(v, 1)));
}

uint32_t parse_touches(yyjson_val* arr) {
    static const std::map<std::string, ModTouch> names = {
        {"visual", ModTouch::Visual},         {"sound", ModTouch::Sound}, {"control", ModTouch::Control},
        {"evaluation", ModTouch::Evaluation}, {"judge", ModTouch::Judge}, {"chart", ModTouch::Chart},
        {"input", ModTouch::Input},
    };
    uint32_t t = 0;
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(arr, i, n, v) {
        if (!yyjson_is_str(v)) continue;
        auto it = names.find(yyjson_get_str(v));
        if (it != names.end()) t |= static_cast<uint32_t>(it->second);
    }
    return t;
}

// フォルダの中身（ファイルの相対パスと中身）から作るハッシュ。MOD を書き換えたら変わる
std::string folder_hash(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::string all;
    for (const fs::path& f : files) all += fs::relative(f, dir, ec).generic_string() + '\n' + sha256_hex(read_file(f)) + '\n';
    return sha256_hex(all);
}

}  // namespace

bool load_mod_manifest(const std::string& dir, ModManifest& out, std::string* error) {
    std::string text = read_file(fs::path(dir) / "mod.json");
    if (text.empty()) {
        if (error) *error = "mod.json が無い: " + dir;
        return false;
    }
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!yyjson_is_obj(root)) {
        if (error) *error = "mod.json が読めない: " + dir;
        yyjson_doc_free(doc);
        return false;
    }
    out = ModManifest{};
    out.dir = dir;
    out.id = str(root, "id");
    out.name = str(root, "name", out.id);
    out.version = str(root, "version", "1");
    out.author = str(root, "author");
    out.description = str(root, "description");
    out.script = str(root, "script");
    out.touches = parse_touches(yyjson_obj_get(root, "touches"));
    out.gas = boolean(root, "gas", false);
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(yyjson_obj_get(root, "gauges"), i, n, v) {
        if (!yyjson_is_obj(v)) continue;
        CustomGaugeDef g;
        g.id = str(v, "id");
        g.name = str(v, "name", g.id);
        g.initial = num(v, "initial", g.initial);
        g.border = num(v, "border", g.border);
        g.survival = boolean(v, "survival", g.survival);
        g.floor = num(v, "floor", g.floor);
        yyjson_val* d = yyjson_obj_get(v, "delta");
        static const char* keys[6] = {"empty_poor", "poor", "bad", "good", "great", "pgreat"};
        for (int k = 0; k < 6; ++k) g.delta[k] = parse_delta(yyjson_obj_get(d, keys[k]));
        g.mine_mul = num(v, "mine", g.mine_mul);
        yyjson_val* low = yyjson_obj_get(v, "low_hp");
        if (yyjson_is_obj(low)) g.low_hp = num(low, "below", 0), g.low_hp_mul = num(low, "damage", 1.0);
        yyjson_val* lamp = yyjson_obj_get(v, "lamp");
        g.lamp_name = str(lamp, "name", g.name);
        g.lamp_color = parse_color(str(lamp, "color"), g.lamp_color);
        g.lamp_rank = num(lamp, "rank", g.lamp_rank);
        g.lamp_as = static_cast<int>(num(lamp, "as", g.lamp_as));
        if (!g.id.empty()) out.gauges.push_back(g);
    }
    yyjson_arr_foreach(yyjson_obj_get(root, "judges"), i, n, v) {
        if (!yyjson_is_obj(v)) continue;
        CustomJudgeDef j;
        j.id = str(v, "id");
        j.name = str(v, "name", j.id);
        parse_window(v, "pgreat", j.pgreat);
        parse_window(v, "great", j.great);
        parse_window(v, "good", j.good);
        parse_window(v, "bad", j.bad);
        j.empty_poor = static_cast<int>(num(v, "empty_poor", j.empty_poor));
        j.rank_scaled = boolean(v, "rank_scaled", false);
        if (!j.id.empty()) out.judges.push_back(j);
    }
    yyjson_doc_free(doc);
    // 定義から分かる分類は宣言が無くても足す（カスタムゲージ・GAS は評価、カスタム判定は判定）
    if (!out.gauges.empty() || out.gas) out.touches |= static_cast<uint32_t>(ModTouch::Evaluation);
    if (!out.judges.empty()) out.touches |= static_cast<uint32_t>(ModTouch::Judge);
    if (out.id.empty()) {
        if (error) *error = "mod.json に id が無い: " + dir;
        return false;
    }
    out.hash = folder_hash(dir);
    return true;
}

std::vector<ModManifest> scan_mods(const std::string& root, bool official, std::vector<std::string>* errors) {
    std::vector<ModManifest> out;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && fs::exists(e.path() / "mod.json", ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& d : dirs) {
        ModManifest m;
        std::string err;
        if (load_mod_manifest(d.string(), m, &err)) {
            m.official = official;
            out.push_back(std::move(m));
        } else if (errors) {
            errors->push_back(err);
        }
    }
    return out;
}

std::vector<std::string> mod_conflicts(const std::vector<const ModManifest*>& mods) {
    std::vector<std::string> out;
    std::set<std::string> ids;
    int gas = 0, judge = 0, chart = 0, input = 0;
    for (const ModManifest* m : mods) {
        if (!ids.insert(m->id).second) out.push_back("同じ MOD が 2 つある: " + m->id);
        gas += m->gas;
        judge += !m->judges.empty();
        chart += (m->touches & static_cast<uint32_t>(ModTouch::Chart)) != 0;
        input += (m->touches & static_cast<uint32_t>(ModTouch::Input)) != 0;
    }
    if (gas > 1) out.push_back("GAS を使う MOD が 2 つある");
    if (judge > 1) out.push_back("判定を変える MOD が 2 つある（使えるのは 1 つ）");
    if (chart > 1) out.push_back("譜面を変える MOD が 2 つある");
    if (input > 1) out.push_back("入力を変える MOD が 2 つある");
    return out;
}

std::string play_mod_key(const std::vector<const ModManifest*>& mods) {
    std::vector<std::string> parts;
    for (const ModManifest* m : mods)
        if (m->affects_play()) parts.push_back(m->id + "@" + m->version + "#" + m->hash.substr(0, 12));
    std::sort(parts.begin(), parts.end());
    std::string key;
    for (const std::string& p : parts) key += (key.empty() ? "" : ",") + p;
    return key;
}

}  // namespace bmj
