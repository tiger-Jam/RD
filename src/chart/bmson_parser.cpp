#include "chart/bmson_parser.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "yyjson.h"

namespace bmj {

namespace {

yyjson_val* get(yyjson_val* o, const char* a, const char* b = nullptr) {
    yyjson_val* v = yyjson_obj_get(o, a);
    if (!v && b) v = yyjson_obj_get(o, b);
    return v;
}
double num(yyjson_val* o, const char* a, double def = 0, const char* b = nullptr) {
    yyjson_val* v = get(o, a, b);
    return yyjson_is_num(v) ? yyjson_get_num(v) : def;
}
std::string str(yyjson_val* o, const char* a) {
    yyjson_val* v = yyjson_obj_get(o, a);
    return yyjson_is_str(v) ? yyjson_get_str(v) : "";
}
bool boolean(yyjson_val* o, const char* a) {
    yyjson_val* v = yyjson_obj_get(o, a);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : yyjson_is_num(v) && yyjson_get_num(v) != 0;
}

// 1 つの拍の位置（pulse）での時刻。STOP の始まりの時刻（at）と、明けの時刻（after）を持つ
struct TimePoint {
    double y = 0, at_ms = 0, after_ms = 0, bpm = 120;
};

}  // namespace

BmsParseResult parse_bmson_text(std::string_view text) {
    BmsParseResult res;
    Chart& chart = res.chart;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return res.error = "bmson を読めない", res;
    }
    struct Free {
        yyjson_doc* d;
        ~Free() { yyjson_doc_free(d); }
    } free_doc{doc};
    // version が無ければ 0.21（名前が camelCase、1 拍 = 240 pulse）
    const bool v1 = yyjson_obj_get(root, "version") != nullptr;
    yyjson_val* info = yyjson_obj_get(root, "info");
    if (!yyjson_is_obj(info)) return res.error = "bmson に info が無い", res;
    std::string mode = str(info, "mode_hint");
    if (!mode.empty() && mode != "beat-7k") return res.error = mode + " は対応していない", res;
    const double init_bpm = num(info, "init_bpm", 0, "initBPM");
    if (init_bpm <= 0) return res.error = "bmson に init_bpm が無い", res;
    double resolution = v1 ? std::fabs(num(info, "resolution", 240)) : 240;
    if (resolution <= 0) resolution = 240;

    // 情報
    ChartMeta& m = chart.meta;
    m.title = str(info, "title");
    m.subtitle = str(info, "subtitle");
    std::string chart_name = str(info, "chart_name");
    if (!chart_name.empty()) m.subtitle += (m.subtitle.empty() ? "" : " ") + std::string("[") + chart_name + "]";
    m.artist = str(info, "artist");
    if (yyjson_val* sa = yyjson_obj_get(info, "subartists"); yyjson_is_arr(sa)) {
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(sa, i, n, v) if (yyjson_is_str(v)) m.subartist += (m.subartist.empty() ? "" : " ") + std::string(yyjson_get_str(v));
    }
    m.genre = str(info, "genre");
    m.playlevel = static_cast<int>(num(info, "level"));
    static const char* const kDiff[] = {"BEGINNER", "NORMAL", "HYPER", "ANOTHER", "INSANE"};
    std::string up = chart_name;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int d = 0; d < 5; ++d)
        if (up.find(kDiff[d]) != std::string::npos) m.difficulty = d + 1;
    m.stagefile = str(info, "eyecatch_image");
    m.backbmp = str(info, "back_image");
    m.banner = str(info, "banner_image");
    // 判定幅：NORMAL（#RANK 2）を judge_rank% で伸縮する（仕様の参考どおり）
    double jr = num(info, "judge_rank", 100, "judgeRank");
    chart.judge_rank = 2;
    chart.judge_scale = jr > 0 ? jr / 100.0 : 1.0;
    double total_rel = num(info, "total", 100);
    if (total_rel <= 0) total_rel = 100;

    // 速さの変化を pulse の順に並べる。同じ位置では SCROLL → BPM → STOP（STOP は足し合わせる）
    struct Change {
        double bpm = 0, stop = 0, scroll = -1;
    };
    std::map<double, Change> changes;
    auto each = [](yyjson_val* arr, auto fn) {
        if (!yyjson_is_arr(arr)) return;
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(arr, i, n, v) if (yyjson_is_obj(v)) fn(v);
    };
    each(get(root, "bpm_events", "bpmNotes"), [&](yyjson_val* e) {
        double b = num(e, "bpm", 0, "v");
        if (b > 0) changes[num(e, "y")].bpm = b;
    });
    each(get(root, "stop_events", "stopNotes"), [&](yyjson_val* e) {
        double d = num(e, "duration", 0, "v");
        if (d > 0) changes[num(e, "y")].stop += d;
    });
    each(yyjson_obj_get(root, "scroll_events"), [&](yyjson_val* e) { changes[num(e, "y")].scroll = num(e, "rate", 1); });

    std::vector<TimePoint> tl{{0, 0, 0, init_bpm}};
    double scroll = 1;
    chart.initial_bpm = init_bpm;
    chart.scroll.push_back({0, 0, 0, init_bpm, 1.0, false});
    auto push_seg = [&](double t_ms, double bpm, double sc, bool stop) {
        ScrollSeg s{static_cast<int64_t>(std::llround(t_ms * 1000.0)), 0, 0, bpm, sc, stop};
        if (chart.scroll.back().time_us == s.time_us) chart.scroll.back() = s;
        else chart.scroll.push_back(s);
    };
    for (const auto& [y, c] : changes) {
        TimePoint p = tl.back();
        if (y > p.y) p.at_ms = p.after_ms + (y - p.y) * 60000.0 / (resolution * p.bpm), p.y = y;
        if (c.scroll >= 0) scroll = c.scroll;
        if (c.bpm > 0) p.bpm = c.bpm;
        push_seg(p.at_ms, p.bpm, scroll, false);
        p.after_ms = p.at_ms;
        if (c.stop > 0) {
            push_seg(p.at_ms, p.bpm, scroll, true);
            p.after_ms = p.at_ms + c.stop * 60000.0 / (resolution * p.bpm);
            push_seg(p.after_ms, p.bpm, scroll, false);
        }
        if (tl.back().y == y) tl.back() = p;
        else tl.push_back(p);
    }
    // pulse → 時刻（ms）。STOP と同じ位置のものは、停止の始まりの時刻（BMS と同じ）
    auto time_of = [&](double y) {
        auto it = std::upper_bound(tl.begin(), tl.end(), y, [](double v, const TimePoint& p) { return v < p.y; });
        const TimePoint& p = it == tl.begin() ? tl.front() : *(it - 1);
        if (p.y == y) return p.at_ms;
        return p.after_ms + (y - p.y) * 60000.0 / (resolution * p.bpm);
    };
    auto to_us = [&](double ms) { return static_cast<int64_t>(std::llround(ms * 1000.0)); };

    // 音とノーツ。sound channel ごとに、ノーツの位置で音を切り分ける（2.5.2）。
    // 継続（c）でないノーツで音を頭に戻し、切れ端は次の継続ノーツまで（次が頭に戻すノーツなら最後まで鳴らす）
    chart.wav.assign(1, "");
    chart.wav_slice.assign(1, {});
    std::map<std::tuple<std::string, int64_t, int64_t>, uint16_t> slice_id;
    auto sound_of = [&](const std::string& name, int64_t start_us, int64_t end_us) -> uint16_t {
        auto [it, fresh] = slice_id.try_emplace({name, start_us, end_us}, static_cast<uint16_t>(chart.wav.size()));
        if (fresh) {
            chart.wav.push_back(name);
            chart.wav_slice.push_back({start_us, end_us});
        }
        return it->second;
    };
    std::map<std::pair<int, double>, size_t> note_at;  // (レーン, y) → ノーツ（重なったノーツをまとめる）
    bool has_scratch = false;
    double last_y = 0;
    each(get(root, "sound_channels", "soundChannel"), [&](yyjson_val* ch) {
        std::string name = str(ch, "name");
        struct N {
            int x;
            double y, l;
            bool c;
        };
        std::vector<N> ns;
        each(yyjson_obj_get(ch, "notes"), [&](yyjson_val* n) {
            yyjson_val* xv = yyjson_obj_get(n, "x");
            int x = yyjson_is_num(xv) ? static_cast<int>(yyjson_get_num(xv)) : 0;
            ns.push_back({x, num(n, "y"), num(n, "l"), boolean(n, "c")});
        });
        std::stable_sort(ns.begin(), ns.end(), [](const N& a, const N& b) { return a.y < b.y; });
        // 同じ位置のノーツは同じ切れ端。c が混ざっていたら頭に戻す
        std::vector<std::pair<double, bool>> groups;  // (y, 継続か)
        for (const N& n : ns) {
            if (groups.empty() || groups.back().first != n.y) groups.push_back({n.y, n.c});
            else groups.back().second = groups.back().second && n.c;
        }
        std::map<double, uint16_t> id_at;
        double restart_ms = 0;
        for (size_t g = 0; g < groups.size(); ++g) {
            double t = time_of(groups[g].first);
            if (!groups[g].second || g == 0) restart_ms = t;
            int64_t start = to_us(t - restart_ms), end = -1;
            if (g + 1 < groups.size() && groups[g + 1].second) end = to_us(time_of(groups[g + 1].first) - restart_ms);
            id_at[groups[g].first] = sound_of(name, start, end);
        }
        for (const N& n : ns) {
            uint16_t id = id_at[n.y];
            int64_t t = to_us(time_of(n.y));
            last_y = std::max(last_y, n.y + n.l);
            int lane = n.x >= 1 && n.x <= 7 ? n.x : n.x == 8 ? 0 : -1;
            if (lane < 0) {
                chart.bgm.push_back({t, id});
                continue;
            }
            // 重なったノーツ（同じレーン・同じ位置）は 1 つにまとめ、2 つ目からの音は BGM として鳴らす
            auto key = std::make_pair(lane, n.y);
            if (note_at.count(key)) {
                chart.bgm.push_back({t, id});
                continue;
            }
            Note note;
            note.lane = static_cast<uint8_t>(lane);
            note.time_us = t;
            note.sound = id;
            if (n.l > 0) note.kind = NoteKind::Long, note.end_us = to_us(time_of(n.y + n.l));
            if (lane == 0) has_scratch = true;
            note_at[key] = chart.notes.size();
            chart.notes.push_back(note);
        }
    });
    // beatoraja の拡張：地雷と、見えないノーツ（押したときに鳴る音だけ）
    each(yyjson_obj_get(root, "mine_channels"), [&](yyjson_val* ch) {
        each(yyjson_obj_get(ch, "notes"), [&](yyjson_val* n) {
            int x = static_cast<int>(num(n, "x"));
            int lane = x >= 1 && x <= 7 ? x : x == 8 ? 0 : -1;
            if (lane < 0 || note_at.count({lane, num(n, "y")})) return;
            Note note;
            note.lane = static_cast<uint8_t>(lane);
            note.time_us = to_us(time_of(num(n, "y")));
            note.kind = NoteKind::Mine;
            note.damage = static_cast<int>(num(n, "damage"));
            chart.notes.push_back(note);
        });
    });
    each(yyjson_obj_get(root, "key_channels"), [&](yyjson_val* ch) {
        std::string name = str(ch, "name");
        each(yyjson_obj_get(ch, "notes"), [&](yyjson_val* n) {
            int x = static_cast<int>(num(n, "x"));
            int lane = x >= 1 && x <= 7 ? x : x == 8 ? 0 : -1;
            if (lane < 0) return;
            Note note;
            note.lane = static_cast<uint8_t>(lane);
            note.time_us = to_us(time_of(num(n, "y")));
            note.kind = NoteKind::Invisible;
            note.sound = sound_of(name, 0, -1);
            chart.notes.push_back(note);
        });
    });
    if (chart.judgeable_notes() == 0) return res.error = "ノーツが無い", res;
    chart.layout = has_scratch ? KeyLayout::K7S : KeyLayout::K7;

    // BGA
    if (yyjson_val* bga = yyjson_obj_get(root, "bga"); yyjson_is_obj(bga)) {
        std::map<int, uint16_t> img;
        chart.bmp.assign(1, "");
        each(get(bga, "bga_header", "bgaHeader"), [&](yyjson_val* h) {
            int id = static_cast<int>(num(h, "id", 0, "ID"));
            img[id] = static_cast<uint16_t>(chart.bmp.size());
            chart.bmp.push_back(str(h, "name"));
        });
        auto events = [&](yyjson_val* arr, BgaEvent::Layer layer) {
            each(arr, [&](yyjson_val* e) {
                auto it = img.find(static_cast<int>(num(e, "id", 0, "ID")));
                if (it == img.end()) return;
                BgaEvent be;
                be.time_us = to_us(time_of(num(e, "y")));
                be.layer = layer;
                be.image = it->second;
                chart.bga.push_back(be);
            });
        };
        events(get(bga, "bga_events", "bgaNotes"), BgaEvent::Layer::Base);
        events(get(bga, "layer_events", "layerNotes"), BgaEvent::Layer::Layer);
        events(get(bga, "poor_events", "poorNotes"), BgaEvent::Layer::Poor);
    }

    // 小節線：lines の位置。無ければ 4 拍ごと、空の配列なら引かない
    yyjson_val* lines = yyjson_obj_get(root, "lines");
    if (yyjson_is_arr(lines)) {
        each(lines, [&](yyjson_val* l) { chart.bars.push_back({to_us(time_of(num(l, "y"))), 0}); });
    } else {
        for (double y = 0; y <= last_y + resolution * 4; y += resolution * 4) chart.bars.push_back({to_us(time_of(y)), 0});
    }

    chart.total = 0;
    chart.finish_import();
    // total は既定の増え方に対する % （100 = 既定）
    chart.total = std::floor(chart.total * total_rel / 100.0);
    return res.ok = true, res;
}

}  // namespace bmj
