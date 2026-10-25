#include "mod/mod.h"

#include <algorithm>
#include <cstdio>
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

CustomLampDef parse_lamp(yyjson_val* v, const std::string& id, const std::string& name) {
    CustomLampDef l;
    l.id = str(v, "id", id);
    l.name = str(v, "name", name.empty() ? l.id : name);
    l.color = parse_color(str(v, "color"), l.color);
    l.colors = std::clamp(static_cast<int>(num(v, "colors", l.colors)), 1, 2);
    l.color2 = parse_color(str(v, "color2"), l.color2);
    l.blink_ms = std::max(50, static_cast<int>(num(v, "blink", l.blink_ms)));
    l.rank = num(v, "rank", l.rank);
    // 絵と記録は、すぐ下の本体のランプのもの（前の形の "as" は読まない）
    l.as = std::clamp(static_cast<int>(l.rank), 1, 4);
    return l;
}

// lamps：前の形（ゲージの中にランプを書く "lamp": {…}）なら、そのランプをゲージと同じ id で lamps に足す
CustomGaugeDef parse_gauge(yyjson_val* v, std::vector<CustomLampDef>* lamps) {
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
    g.colors = std::clamp(static_cast<int>(num(v, "colors", g.colors)), 1, 2);
    g.color = parse_color(str(v, "color"), g.color);
    g.color_low = parse_color(str(v, "color_low"), g.color_low);
    g.frame = static_cast<int>(num(v, "frame", g.frame));
    yyjson_val* lamp = yyjson_obj_get(v, "lamp");
    if (yyjson_is_str(lamp)) {
        g.lamp = yyjson_get_str(lamp);
    } else if (yyjson_is_obj(lamp) && lamps) {
        CustomLampDef l = parse_lamp(lamp, g.id, g.name);
        l.id = g.id;
        if (std::none_of(lamps->begin(), lamps->end(), [&](const CustomLampDef& x) { return x.id == l.id; })) lamps->push_back(l);
        g.lamp = g.id;
    }
    if (g.lamp.empty()) g.lamp = g.survival ? "HARD" : "CLEAR";
    return g;
}

std::vector<CustomGaugeDef> parse_gauges(yyjson_val* arr, std::vector<CustomLampDef>* lamps) {
    std::vector<CustomGaugeDef> out;
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(arr, i, n, v) {
        if (!yyjson_is_obj(v)) continue;
        CustomGaugeDef g = parse_gauge(v, lamps);
        if (!g.id.empty()) out.push_back(g);
    }
    return out;
}

std::vector<CustomLampDef> parse_lamps(yyjson_val* arr) {
    std::vector<CustomLampDef> out;
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(arr, i, n, v) {
        if (!yyjson_is_obj(v)) continue;
        CustomLampDef l = parse_lamp(v, "", "");
        if (!l.id.empty()) out.push_back(l);
    }
    return out;
}

std::vector<CustomJudgeDef> parse_judges(yyjson_val* arr) {
    std::vector<CustomJudgeDef> out;
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(arr, i, n, v) {
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
        if (!j.id.empty()) out.push_back(j);
    }
    return out;
}

// JSON の文字列にする（" と \ と制御文字を逃がす）
std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o += c;
        }
    }
    return o + "\"";
}

std::string json_num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string color_str(uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06x", c & 0xffffff);
    return buf;
}

std::string gauge_json(const CustomGaugeDef& g) {
    static const char* keys[6] = {"empty_poor", "poor", "bad", "good", "great", "pgreat"};
    std::string d;
    for (int k = 0; k < 6; ++k) {
        d += std::string(k ? ", " : "") + "\"" + keys[k] + "\": ";
        d += g.delta[k].total != 0 ? "{\"fixed\": " + json_num(g.delta[k].fixed) + ", \"total\": " + json_num(g.delta[k].total) + "}"
                                   : json_num(g.delta[k].fixed);
    }
    std::string out = "    {\n      \"id\": " + json_str(g.id) + ", \"name\": " + json_str(g.name) + ",\n      \"initial\": " +
                      json_num(g.initial) + ", \"border\": " + json_num(g.border) + ", \"survival\": " + (g.survival ? "true" : "false") +
                      ", \"floor\": " + json_num(g.floor) + ",\n      \"delta\": {" + d + "},\n      \"mine\": " + json_num(g.mine_mul) +
                      ", \"low_hp\": {\"below\": " + json_num(g.low_hp) + ", \"damage\": " + json_num(g.low_hp_mul) + "},\n      \"colors\": " +
                      std::to_string(g.colors) + ", \"color\": \"" + color_str(g.color) + "\", \"color_low\": \"" + color_str(g.color_low) + "\"";
    if (g.frame >= 0) out += ", \"frame\": " + std::to_string(g.frame);
    return out + ",\n      \"lamp\": " + json_str(g.lamp) + "\n    }";
}

std::string lamp_json(const CustomLampDef& l) {
    return "    {\"id\": " + json_str(l.id) + ", \"name\": " + json_str(l.name) + ", \"color\": \"" + color_str(l.color) +
           "\", \"colors\": " + std::to_string(l.colors) + ", \"color2\": \"" + color_str(l.color2) + "\", \"blink\": " +
           std::to_string(l.blink_ms) + ", \"rank\": " + json_num(l.rank) + ", \"as\": " + std::to_string(l.as) + "}";
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
    out.lamps = parse_lamps(yyjson_obj_get(root, "lamps"));
    out.gauges = parse_gauges(yyjson_obj_get(root, "gauges"), &out.lamps);
    out.gauge_label = str(root, "gauge_label");
    if (yyjson_val* mt = yyjson_obj_get(root, "metronome"); yyjson_is_obj(mt)) {
        out.metronome = true;
        out.metronome_accent = (fs::path(dir) / fs::u8path(str(mt, "accent", "accent.wav"))).string();
        out.metronome_beat = (fs::path(dir) / fs::u8path(str(mt, "beat", "beat.wav"))).string();
        out.metronome_volume = std::clamp(static_cast<int>(num(mt, "volume", 400)), 0, kMaxMetronomeVolume);
    }
    out.gauge_frame = static_cast<int>(num(root, "gauge_frame", -1));
    yyjson_arr_foreach(yyjson_obj_get(root, "gas_order"), i, n, v)
        if (yyjson_is_str(v)) out.gas_order.push_back(yyjson_get_str(v));
    if (out.gas && out.gas_order.empty()) out.gas_order = {"HARD", "GROOVE", "EASY"};
    yyjson_arr_foreach(yyjson_obj_get(root, "settings"), i, n, v) {
        if (!yyjson_is_obj(v)) continue;
        ModSetting st;
        st.type = str(v, "type");
        st.name = str(v, "name", st.type);
        if (st.type == "gauge_order" || st.type == "gauges" || st.type == "lamps" || st.type == "judges" || st.type == "metronome")
            out.settings.push_back(st);
    }
    out.judges = parse_judges(yyjson_obj_get(root, "judges"));
    yyjson_doc_free(doc);
    // 定義から分かる分類は宣言が無くても足す（カスタムゲージ・GAS は評価、カスタム判定は判定）
    if (out.metronome) out.touches |= static_cast<uint32_t>(ModTouch::Sound);
    if (!out.gauges.empty() || !out.lamps.empty() || out.gas) out.touches |= static_cast<uint32_t>(ModTouch::Evaluation);
    if (!out.judges.empty() || out.has_setting("judges")) out.touches |= static_cast<uint32_t>(ModTouch::Judge);
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

void apply_mod_settings(ModManifest& m, const std::string& settings_path) {
    if (m.settings.empty()) return;
    std::string text = read_file(fs::u8path(settings_path));
    if (text.empty()) return;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (yyjson_is_obj(root)) {
        for (const ModSetting& st : m.settings) {
            if (st.type == "lamps" && yyjson_is_arr(yyjson_obj_get(root, "lamps"))) m.lamps = parse_lamps(yyjson_obj_get(root, "lamps"));
            if (st.type == "lamps") {
                size_t i, n;
                yyjson_val* v;
                yyjson_arr_foreach(yyjson_obj_get(root, "standard_lamps"), i, n, v) {
                    if (i >= m.standard_lamps.size() || !yyjson_is_obj(v)) continue;
                    CustomLampDef l = parse_lamp(v, "", "");
                    l.colors = std::clamp(static_cast<int>(num(v, "colors", 0)), 0, 2);  // 0 はスキンの絵のまま
                    m.standard_lamps[i] = l;
                }
                yyjson_val* lc = yyjson_obj_get(root, "lamp_colors");
                yyjson_val *k, *v2;
                yyjson_obj_iter it = yyjson_obj_iter_with(lc);
                m.lamp_colors.clear();
                while (yyjson_is_obj(lc) && (k = yyjson_obj_iter_next(&it))) {
                    v2 = yyjson_obj_iter_get_val(k);
                    if (!yyjson_is_obj(v2)) continue;
                    CustomLampDef l = parse_lamp(v2, "", "");
                    l.colors = std::clamp(static_cast<int>(num(v2, "colors", 0)), 0, 2);  // 0 はその MOD の色のまま
                    if (l.colors > 0) m.lamp_colors[yyjson_get_str(k)] = l;
                }
            }
            if (st.type == "gauges" && yyjson_is_arr(yyjson_obj_get(root, "gauges")))
                m.gauges = parse_gauges(yyjson_obj_get(root, "gauges"), &m.lamps);
            if (st.type == "gauges") {
                size_t i, n;
                yyjson_val* v;
                yyjson_arr_foreach(yyjson_obj_get(root, "standard_gauges"), i, n, v) {
                    if (i >= m.standard_gauges.size() || !yyjson_is_obj(v)) continue;
                    GaugeColors& c = m.standard_gauges[i];
                    c.colors = std::clamp(static_cast<int>(num(v, "colors", 0)), 0, 2);
                    c.color = parse_color(str(v, "color"), c.color);
                    c.color_low = parse_color(str(v, "color_low"), c.color_low);
                }
            }
            if (st.type == "metronome") m.metronome_volume = std::clamp(static_cast<int>(num(root, "metronome_volume", m.metronome_volume)), 0, kMaxMetronomeVolume);
            if (st.type == "judges" && yyjson_is_arr(yyjson_obj_get(root, "judges"))) {
                m.judges = parse_judges(yyjson_obj_get(root, "judges"));
                m.judge_use = str(root, "judge_use");
            }
            if (st.type == "gauge_order" && yyjson_is_arr(yyjson_obj_get(root, "gas_order"))) {
                m.gas_order.clear();
                size_t i, n;
                yyjson_val* v;
                yyjson_arr_foreach(yyjson_obj_get(root, "gas_order"), i, n, v)
                    if (yyjson_is_str(v)) m.gas_order.push_back(yyjson_get_str(v));
            }
        }
    }
    yyjson_doc_free(doc);
}

std::string mod_settings_json(const ModManifest& m) {
    std::string out = "{\n";
    bool first = true;
    for (const ModSetting& st : m.settings) {
        if (!first) out += ",\n";
        first = false;
        if (st.type == "gauges") {
            out += "  \"gauges\": [\n";
            for (size_t i = 0; i < m.gauges.size(); ++i) out += gauge_json(m.gauges[i]) + (i + 1 < m.gauges.size() ? ",\n" : "\n");
            out += "  ],\n  \"standard_gauges\": [\n";
            for (size_t i = 0; i < m.standard_gauges.size(); ++i) {
                const GaugeColors& c = m.standard_gauges[i];
                out += "    {\"colors\": " + std::to_string(c.colors) + ", \"color\": \"" + color_str(c.color) + "\", \"color_low\": \"" +
                       color_str(c.color_low) + "\"}" + (i + 1 < m.standard_gauges.size() ? ",\n" : "\n");
            }
            out += "  ]";
        } else if (st.type == "lamps") {
            out += "  \"lamps\": [\n";
            for (size_t i = 0; i < m.lamps.size(); ++i) out += lamp_json(m.lamps[i]) + (i + 1 < m.lamps.size() ? ",\n" : "\n");
            out += "  ],\n  \"standard_lamps\": [\n";
            for (size_t i = 0; i < m.standard_lamps.size(); ++i) {
                const CustomLampDef& l = m.standard_lamps[i];
                out += "    {\"colors\": " + std::to_string(l.colors) + ", \"color\": \"" + color_str(l.color) + "\", \"color2\": \"" +
                       color_str(l.color2) + "\", \"blink\": " + std::to_string(l.blink_ms) + "}" + (i + 1 < m.standard_lamps.size() ? ",\n" : "\n");
            }
            out += "  ],\n  \"lamp_colors\": {";
            bool first_lc = true;
            for (const auto& [key, l] : m.lamp_colors) {
                if (l.colors <= 0) continue;
                out += std::string(first_lc ? "\n" : ",\n") + "    " + json_str(key) + ": {\"colors\": " + std::to_string(l.colors) + ", \"color\": \"" +
                       color_str(l.color) + "\", \"color2\": \"" + color_str(l.color2) + "\", \"blink\": " + std::to_string(l.blink_ms) + "}";
                first_lc = false;
            }
            out += first_lc ? "}" : "\n  }";
        } else if (st.type == "metronome") {
            out += "  \"metronome_volume\": " + std::to_string(m.metronome_volume);
        } else if (st.type == "judges") {
            auto pair = [](const int v[2]) { return "[" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + "]"; };
            out += "  \"judge_use\": " + json_str(m.judge_use) + ",\n  \"judges\": [\n";
            for (size_t i = 0; i < m.judges.size(); ++i) {
                const CustomJudgeDef& j = m.judges[i];
                out += "    {\"id\": " + json_str(j.id) + ", \"name\": " + json_str(j.name) + ", \"pgreat\": " + pair(j.pgreat) +
                       ", \"great\": " + pair(j.great) + ", \"good\": " + pair(j.good) + ", \"bad\": " + pair(j.bad) +
                       ", \"empty_poor\": " + std::to_string(j.empty_poor) + ", \"rank_scaled\": " + (j.rank_scaled ? "true" : "false") + "}" +
                       (i + 1 < m.judges.size() ? ",\n" : "\n");
            }
            out += "  ]";
        } else if (st.type == "gauge_order") {
            out += "  \"gas_order\": [";
            for (size_t i = 0; i < m.gas_order.size(); ++i) out += (i ? ", " : "") + json_str(m.gas_order[i]);
            out += "]";
        }
    }
    return out + "\n}\n";
}

bool save_mod_settings(const ModManifest& m, const std::string& settings_path) {
    std::error_code ec;
    fs::create_directories(fs::u8path(settings_path).parent_path(), ec);
    std::ofstream f(fs::u8path(settings_path), std::ios::binary);
    f << mod_settings_json(m);
    return static_cast<bool>(f);
}

std::vector<CustomGaugeDef> standard_gauge_samples() {
    // 増減の並びは 空POOR, POOR, BAD, GOOD, GREAT, PGREAT（game/gauge.cpp の表）。total は TOTAL ÷ 総ノーツ数 に掛ける値
    auto make = [](const char* name, bool survival, double initial, std::array<double, 6> fixed, std::array<double, 6> total,
                   double low_hp, const char* lamp) {
        CustomGaugeDef g;
        g.id = g.name = name;
        g.survival = survival, g.initial = initial, g.border = 80, g.floor = survival ? 0 : 2;
        for (int k = 0; k < 6; ++k) g.delta[k] = {fixed[k], total[k]};
        if (low_hp > 0) g.low_hp = low_hp, g.low_hp_mul = 0.6;
        g.colors = survival ? 1 : 2;
        g.lamp = lamp;
        return g;
    };
    return {
        make("GROOVE", false, 20, {-2, -6, -4, 0, 0, 0}, {0, 0, 0, 0.5, 1, 1}, 0, "CLEAR"),
        make("HARD", true, 100, {-2, -10, -6, 0.05, 0.1, 0.1}, {}, 30, "HARD"),
        make("HAZARD", true, 100, {0, -100, -100, 0, 0, 0}, {}, 0, "HARD"),
        make("EASY", false, 20, {-1.6, -4.8, -3.2, 0, 0, 0}, {0, 0, 0, 0.6, 1.2, 1.2}, 0, "EASY"),
        make("P-ATTACK", true, 100, {-100, -100, -100, -100, -1, 0.1}, {}, 30, "HARD"),
        make("G-ATTACK", true, 100, {-2, -10, -6, 0.1, -1, -10}, {}, 30, "HARD"),
    };
}

std::vector<CustomJudgeDef> standard_judge_samples() {
    // game/judge.cpp の JudgeWindows::for_rank と同じ幅（BAD 200・空POOR 1000 はどの #RANK でも同じ）
    auto make = [](const char* name, int pg, int gr, int gd, bool scaled) {
        CustomJudgeDef j;
        j.id = j.name = name;
        j.pgreat[0] = j.pgreat[1] = pg, j.great[0] = j.great[1] = gr, j.good[0] = j.good[1] = gd;
        j.bad[0] = j.bad[1] = 200, j.empty_poor = 1000;
        j.rank_scaled = scaled;
        return j;
    };
    return {make("#RANK どおり", 18, 40, 100, true), make("VERY HARD", 8, 24, 40, false), make("HARD", 15, 30, 60, false),
            make("NORMAL", 18, 40, 100, false), make("EASY", 21, 60, 120, false)};
}

const CustomJudgeDef* standard_judge(const std::string& id) {
    static const std::vector<CustomJudgeDef> all = standard_judge_samples();
    for (const char* k : kStandardJudgeIds)
        if (id == k)
            for (const CustomJudgeDef& j : all)
                if (j.id == id) return &j;
    return nullptr;
}

const CustomJudgeDef* ModManifest::active_judge() const {
    if (!has_setting("judges")) return judges.empty() ? nullptr : &judges.front();
    if (const CustomJudgeDef* s = standard_judge(judge_use)) return s;
    for (const CustomJudgeDef& j : judges)
        if (j.id == judge_use) return &j;
    return nullptr;
}

std::string judge_values_key(const CustomJudgeDef& j) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "pg%d,%d gr%d,%d gd%d,%d bd%d,%d ep%d rs%d", j.pgreat[0], j.pgreat[1], j.great[0], j.great[1], j.good[0],
                  j.good[1], j.bad[0], j.bad[1], j.empty_poor, j.rank_scaled ? 1 : 0);
    return buf;
}

std::vector<CustomLampDef> standard_lamp_samples() {
    static const char* names[6] = {"FAILED", "EASY", "CLEAR", "HARD", "FULL COMBO", "PERFECT"};
    std::vector<CustomLampDef> out;
    for (int i = 0; i < 6; ++i) {
        CustomLampDef l;
        l.id = l.name = names[i];
        l.rank = i + 1, l.as = i + 1;
        out.push_back(l);
    }
    return out;
}

void apply_lamp_colors(std::vector<ModManifest>& mods, const std::vector<std::string>& enabled) {
    for (const ModManifest& m : mods) {
        if (std::find(enabled.begin(), enabled.end(), m.id) == enabled.end()) continue;
        for (const auto& [key, c] : m.lamp_colors) {
            if (c.colors <= 0) continue;
            const size_t slash = key.find('/');
            if (slash == std::string::npos) continue;
            for (ModManifest& o : mods)
                if (o.id == key.substr(0, slash))
                    for (CustomLampDef& l : o.lamps)
                        if (l.id == key.substr(slash + 1)) l.colors = c.colors, l.color = c.color, l.color2 = c.color2, l.blink_ms = c.blink_ms;
        }
    }
}

void resolve_gauge_lamps(std::vector<ModManifest>& mods) {
    for (ModManifest& m : mods) {
        for (size_t i = 0; i < m.gauges.size(); ++i) {
            CustomGaugeDef& g = m.gauges[i];
            if (!m.gauge_label.empty()) {
                const int no = static_cast<int>(i) + 1;
                g.name = m.gauge_label + std::to_string(no);
                g.frame = m.gauge_frame >= 0 && no <= kMaxNumberedGauges ? m.gauge_frame + no - 1 : -1;
            }
            g.lamp_key.clear();
            g.lamp_name = g.survival ? "HARD" : "CLEAR";
            g.lamp_as = g.survival ? 4 : 3;
            g.lamp_color = 0xffffff;
            bool found = false;
            for (int k = 0; k < 4; ++k)
                if (g.lamp == kStandardLampKeys[k]) g.lamp_name = g.lamp, g.lamp_as = k + 1, found = true;
            if (!found) {
                // 同じ MOD の id か「MOD/id」
                const size_t slash = g.lamp.find('/');
                const std::string mod_id = slash == std::string::npos ? m.id : g.lamp.substr(0, slash);
                const std::string lamp_id = slash == std::string::npos ? g.lamp : g.lamp.substr(slash + 1);
                for (const ModManifest& o : mods)
                    if (o.id == mod_id)
                        for (const CustomLampDef& l : o.lamps)
                            if (l.id == lamp_id) {
                                g.lamp_key = o.id + "/" + l.id;
                                g.lamp_name = l.name, g.lamp_color = l.color, g.lamp_as = std::clamp(static_cast<int>(l.rank), 1, 4);
                                g.lamp_rank = l.rank;
                                found = true;
                            }
            }
            if (g.lamp_key.empty()) g.lamp_rank = g.lamp_as;
        }
    }
}

std::vector<std::string> mod_conflicts(const std::vector<const ModManifest*>& mods) {
    std::vector<std::string> out;
    std::set<std::string> ids;
    int gas = 0, judge = 0, chart = 0, input = 0;
    for (const ModManifest* m : mods) {
        if (!ids.insert(m->id).second) out.push_back("同じ MOD が 2 つある: " + m->id);
        gas += m->gas;
        judge += m->active_judge() != nullptr;
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
        if (!m->affects_play()) continue;
        // 詳細で作る判定は、MOD のフォルダの中身ではなく、選んだ判定の幅の値で分ける（幅を変えれば別の記録、名前を変えても同じ記録）
        else if (m->has_setting("judges")) parts.push_back(m->id + "@judge#" + sha256_hex(judge_values_key(*m->active_judge())).substr(0, 12));
        else parts.push_back(m->id + "@" + m->version + "#" + m->hash.substr(0, 12));
    std::sort(parts.begin(), parts.end());
    std::string key;
    for (const std::string& p : parts) key += (key.empty() ? "" : ",") + p;
    return key;
}

std::string seven_key_record_key(const std::string& mod_key) { return mod_key.empty() ? "7k" : "7k," + mod_key; }

}  // namespace bmj
