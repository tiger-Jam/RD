#include "chart/osu_parser.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace bmj {

namespace {

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) out.push_back(trim(cur)), cur.clear();
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

bool is_video_name(const std::string& p) {
    std::string l = p;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : {".mpg", ".mpeg", ".avi", ".wmv", ".mp4", ".m4v", ".webm", ".mkv", ".ogv", ".flv"})
        if (l.size() >= std::strlen(e) && l.compare(l.size() - std::strlen(e), std::strlen(e), e) == 0) return true;
    return false;
}

struct TimingPoint {
    double time = 0, beat_len = 500;
    int meter = 4;
    bool uninherited = true;
    int effects = 0;
};

}  // namespace

BmsParseResult parse_osu_text(std::string_view text, const OsuParseOptions& opts) {
    BmsParseResult res;
    Chart& chart = res.chart;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.remove_prefix(3);  // BOM

    int version = 14;  // 版の行が無ければ最新として読む
    std::map<std::string, std::string> general, meta, diff;
    std::vector<TimingPoint> points;
    std::vector<std::vector<std::string>> objects;
    std::string background, video;
    double video_time = 0;
    std::vector<std::pair<double, std::string>> samples;  // ストーリーボードの効果音（時刻 ms, ファイル）

    std::string section;
    std::istringstream in{std::string(text)};
    std::string raw;
    bool first = true;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (first) {
            first = false;
            if (line.rfind("osu file format v", 0) == 0) {
                version = std::atoi(line.c_str() + 17);
                continue;
            }
        }
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        // 行の途中の // はコメント（[Metadata] だけは曲名に入りうるので落とさない）
        if (section != "Metadata")
            if (size_t c = line.find("//"); c != std::string::npos) line = trim(line.substr(0, c));
        if (section == "General" || section == "Metadata" || section == "Difficulty") {
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 1));
            (section == "General" ? general : section == "Metadata" ? meta : diff)[k] = v;
        } else if (section == "Events") {
            std::vector<std::string> f = split(line, ',');
            if (f.size() < 3) continue;
            if (f[0] == "0" || f[0] == "Background") {
                if (background.empty()) background = unquote(f[2]);
            } else if (f[0] == "1" || f[0] == "Video") {
                std::string name = unquote(f[2]);
                // 拡張子が動画でなければ背景画像として扱う（古い譜面の書き間違い）
                if (is_video_name(name)) video = name, video_time = std::atof(f[1].c_str());
                else if (background.empty()) background = name;
            } else if ((f[0] == "5" || f[0] == "Sample") && f.size() >= 4) {
                // レイヤ 1（失敗中）・2（合格中）の音は鳴らさない
                int layer = std::atoi(f[2].c_str());
                if (layer == 0 || layer == 3) samples.push_back({std::atof(f[1].c_str()), unquote(f[3])});
            }
        } else if (section == "TimingPoints") {
            std::vector<std::string> f = split(line, ',');
            if (f.size() < 2) continue;
            TimingPoint p;
            p.time = std::atof(f[0].c_str());
            p.beat_len = std::atof(f[1].c_str());
            if (f.size() > 2 && !f[2].empty() && f[2][0] != '0') p.meter = std::atoi(f[2].c_str());
            if (f.size() > 6) p.uninherited = std::atoi(f[6].c_str()) != 0;
            if (f.size() > 7) p.effects = std::atoi(f[7].c_str());
            if (p.uninherited && !(p.beat_len > 0)) continue;  // 拍の長さが正しくない行は捨てる
            points.push_back(p);
        } else if (section == "HitObjects") {
            std::vector<std::string> f = split(line, ',');
            if (f.size() >= 4) objects.push_back(std::move(f));
        }
    }

    if (std::atoi(general["Mode"].c_str()) != 3) return res.error = "osu!mania の譜面ではない", res;
    int keys = std::max(1, static_cast<int>(std::lround(std::atof(diff["CircleSize"].c_str()))));
    if (keys != 7 && keys != 8) return res.error = std::to_string(keys) + "K は対応していない", res;
    // v5 未満は、全部の時刻に 24ms を足す（osu! 本体と同じ）
    const double offset = version < 5 ? 24.0 : 0.0;
    auto us = [&](double ms) { return static_cast<int64_t>(std::llround((ms + offset) * 1000.0)); };

    // 情報
    ChartMeta& m = chart.meta;
    m.title = !meta["TitleUnicode"].empty() ? meta["TitleUnicode"] : meta["Title"];
    m.artist = !meta["ArtistUnicode"].empty() ? meta["ArtistUnicode"] : meta["Artist"];
    if (!meta["Version"].empty()) m.subtitle = "[" + meta["Version"] + "]";
    m.subartist = meta["Creator"];
    m.backbmp = m.stagefile = background;
    chart.judge_rank = 2;  // 判定幅は BMS の NORMAL（OD からの変換は未決。仕様書13章）

    // 音：ファイル名ごとに 1 つの番号。0 は「無し」
    std::map<std::string, uint16_t> sound_id;
    chart.wav.assign(1, "");
    auto sound_of = [&](const std::string& name) -> uint16_t {
        if (name.empty()) return 0;
        auto [it, fresh] = sound_id.try_emplace(name, static_cast<uint16_t>(chart.wav.size()));
        if (fresh) chart.wav.push_back(name);
        return it->second;
    };
    // 音源は時刻 0 から鳴らす（AudioLeadIn は開始前の待ち時間で、譜面の時刻はずれないので使わない）
    if (uint16_t a = sound_of(general["AudioFilename"])) chart.bgm.push_back({0, a});
    for (const auto& [t, name] : samples)
        if (uint16_t s = sound_of(name)) chart.bgm.push_back({static_cast<int64_t>(std::llround(t * 1000.0)), s});
    // 動画の BGA（背景画像は BACKBMP）
    if (!video.empty()) {
        chart.bmp = {"", video};
        BgaEvent e;
        e.time_us = std::max<int64_t>(0, us(video_time));
        e.layer = BgaEvent::Layer::Base;
        e.image = 1;
        chart.bga.push_back(e);
    }

    // ノーツ。列 = floor(x × キー数 / 512)。8K は片方の端を皿にする
    for (const auto& f : objects) {
        int x = std::clamp(static_cast<int>(std::atof(f[0].c_str())), 0, 512);
        int col = std::clamp(x * keys / 512, 0, keys - 1);
        int type = std::atoi(f[3].c_str());
        Note n;
        if (keys == 7) n.lane = static_cast<uint8_t>(col + 1);
        else if (opts.scratch_side == 0) n.lane = static_cast<uint8_t>(col);
        else n.lane = static_cast<uint8_t>(col == 7 ? 0 : col + 1);
        n.time_us = us(std::atof(f[2].c_str()));
        // ビット0（丸）が先、次にビット7（ホールド）。どちらでもなければ捨てる
        std::string sample;
        if (type & 1) {
            n.kind = NoteKind::Normal;
            if (f.size() > 5) {
                std::vector<std::string> hs = split(f[5], ':');
                if (hs.size() >= 5) sample = hs[4];
            }
        } else if (type & 128) {
            if (f.size() < 6) continue;
            std::vector<std::string> p = split(f[5], ':');
            n.kind = NoteKind::Long;
            n.end_us = std::max(n.time_us, us(std::atof(p[0].c_str())));
            if (p.size() >= 6) sample = p[5];
            // 長さ 0 のホールドは通常ノーツにする
            if (n.end_us == n.time_us) n.kind = NoteKind::Normal, n.end_us = -1;
        } else {
            continue;
        }
        // ファイル名の無いヒット音は鳴らさない（汎用のヒット音は未対応）
        n.sound = sound_of(sample);
        chart.notes.push_back(n);
    }
    if (chart.notes.empty()) return res.error = "ノーツが無い", res;
    chart.layout = keys == 8 ? KeyLayout::K7S : KeyLayout::K7;

    // 速さ：BPM（非継承点）と SV（継承点。100 / −beatLength を 0.01〜10 に丸める）を掛けたもの。
    // 非継承点で SV は 1 に戻る（同じ時刻の継承点があればそちらが勝つ）
    std::stable_sort(points.begin(), points.end(), [](const TimingPoint& a, const TimingPoint& b) { return a.time < b.time; });
    double bpm = 120, sv = 1;
    for (const TimingPoint& p : points)
        if (p.uninherited) {
            bpm = 60000.0 / std::clamp(p.beat_len, 6.0, 60000.0);
            break;
        }
    chart.initial_bpm = bpm;
    chart.scroll.push_back({0, 0, 0, bpm, 1.0, false});
    for (size_t i = 0; i < points.size();) {
        double t = points[i].time;
        bool has_inherited = false;
        double new_sv = sv;
        for (; i < points.size() && points[i].time == t; ++i) {
            const TimingPoint& p = points[i];
            if (p.uninherited) {
                bpm = 60000.0 / std::clamp(p.beat_len, 6.0, 60000.0);
                if (!has_inherited) new_sv = 1.0;
            } else if (p.beat_len < 0) {
                new_sv = std::clamp(100.0 / -p.beat_len, 0.01, 10.0);
                has_inherited = true;
            }
        }
        sv = new_sv;
        int64_t tu = std::max<int64_t>(0, us(t));
        ScrollSeg s{tu, 0, 0, bpm, sv, false};
        if (chart.scroll.back().time_us == tu) chart.scroll.back() = s;
        else chart.scroll.push_back(s);
    }

    // 小節線：非継承点ごとに「拍の長さ × 拍数」おき。次の非継承点で打ち切る
    int64_t last = 0;
    for (const Note& n : chart.notes) last = std::max(last, std::max(n.time_us, n.end_us));
    std::vector<const TimingPoint*> red;
    for (const TimingPoint& p : points)
        if (p.uninherited) red.push_back(&p);
    for (size_t i = 0; i < red.size(); ++i) {
        double step = red[i]->beat_len * std::max(1, red[i]->meter);
        double end = i + 1 < red.size() ? red[i + 1]->time : last / 1000.0 + step;
        bool skip_first = red[i]->effects & 8;
        for (double t = red[i]->time; t < end - 1; t += step) {
            if (skip_first) {
                skip_first = false;
                continue;
            }
            if (t + offset >= 0) chart.bars.push_back({us(t), 0});
        }
    }
    chart.finish_import();
    return res.ok = true, res;
}

}  // namespace bmj
