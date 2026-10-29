// BMSの読み込み。挙動はOpenLR2の ParseBmsFile（LR2/LR2_bmsload.cpp）に合わせている。
// コメントの「LR2:」はそのOpenLR2側の該当箇所。
#include "chart/bms_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "util/lr2_random.h"
#include "util/path.h"
#include "util/text.h"

namespace rd {

namespace {

constexpr int kSlots = 3844;  // base62の最大+1（LR2: SINGLESLOTS）

int base36_char(char c, bool base62) {
    if (c >= '1' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return base62 ? c - 'a' + 36 : c - 'a' + 10;
    return 0;  // '0' と不正な文字は0（LR2: En_fileutil.cpp:537）
}

int base_id(char a, char b, bool base62) {
    int radix = base62 ? 62 : 36;
    return base36_char(a, base62) * radix + base36_char(b, base62);
}

int hex_char(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// 譜面の中の 1 つのもの（チャンネルの 1 つの値）の種類
enum class ObjType : uint8_t {
    Other,      // 使わないチャンネル（並びと時刻の計算にだけ入る）
    Bgm, Measure, Bpm, Bga, BgaPoor, BgaLayer, ExBpm, Stop,
    Note,       // 見えるノーツ（LN の始点もこれになる）
    Invisible,  // 見えないノーツ
    LnMark,     // 5x/6x の LN の端（始点か終点かは並べてから決まる）
    LnEnd,      // LN の終点になったもの（以後は使わない）
    Mine,       // 地雷
};

struct Obj {
    double pos = 0.0;  // 小節番号 + 小節内の位置
    ObjType type = ObjType::Other;
    int side = 1, lane = 0;  // 1P/2P と、レーン（0 = 皿、1〜7 = 鍵盤）
    int key = 0;       // 同じ位置で並べる順（チャンネルの番号の順。レーンは皿・1〜7 鍵の順）
    double val = 0.0;
    int order = 0;     // 同じ位置・同じ key の並びを安定させるため
    double real_ms = 0.0;
    double beat = 0.0;  // スクロール位置（拍）
    double real_ln = -1.0, beat_ln = 0.0;
    double bpm = 0.0;   // この位置での BPM（主 BPM を数えるため）
    int mine = 0;
};

// チャンネルの番号（16 進の上の桁 × 10 + 下の桁）から、種類・1P/2P・レーンを決める。捨てるチャンネルなら false。
// ノーツのチャンネル（1x/2x 見える、3x/4x 見えない、5x/6x LN、Dx/Ex 地雷）は、どれも「x1〜x5 が 1〜5 鍵、x6 が皿、x8・x9 が 6・7 鍵。
// x0・x7 は捨てる」
bool decode_channel(int ch, Obj& o) {
    struct Group {
        int base;
        ObjType type;
        int side;
    };
    static const Group kGroups[] = {{10, ObjType::Note, 1},      {20, ObjType::Note, 2},   {30, ObjType::Invisible, 1},
                                    {40, ObjType::Invisible, 2}, {50, ObjType::LnMark, 1}, {60, ObjType::LnMark, 2},
                                    {130, ObjType::Mine, 1},     {140, ObjType::Mine, 2}};
    for (const Group& g : kGroups) {
        const int digit = ch - g.base;
        if (digit < 0 || digit > 9) continue;
        static const int kLaneOfDigit[10] = {-1, 1, 2, 3, 4, 5, 0, -1, 6, 7};
        const int lane = kLaneOfDigit[digit];
        if (lane < 0) return false;
        o.type = g.type, o.side = g.side, o.lane = lane, o.key = g.base + lane;
        return true;
    }
    static const ObjType kLow[10] = {ObjType::Other, ObjType::Bgm,      ObjType::Measure,  ObjType::Bpm,   ObjType::Bga,
                                     ObjType::Other, ObjType::BgaPoor, ObjType::BgaLayer, ObjType::ExBpm, ObjType::Stop};
    if (ch == 0) return false;
    o.type = ch < 10 ? kLow[ch] : ObjType::Other;
    o.key = ch;
    return true;
}

// 選曲用のテキストのヘッダー「#NAME 値」。名前の直後の 1 文字（空白かタブ）を区切りとする（LR2: fileutil:271）
bool text_header(const std::string& up, const std::string& org, const char* name, std::string& out) {
    size_t n = std::char_traits<char>::length(name);
    if (up.size() <= n || up.compare(0, n, name) != 0 || (org[n] != ' ' && org[n] != '\t')) return false;
    out = std::string(trim(std::string_view(org).substr(n + 1)));
    return true;
}

// 選曲用の整数のヘッダー。区切りの次から整数として読めたときだけ値を替える（LR2: songmanage:166 GetRawBodyInt）
bool int_header(const std::string& up, const char* name, int& out) {
    size_t n = std::char_traits<char>::length(name);
    if (up.size() <= n + 1 || up.compare(0, n, name) != 0 || (up[n] != ' ' && up[n] != '\t')) return false;
    int v = 0;
    auto r = std::from_chars(up.data() + n + 1, up.data() + up.size(), v);
    if (r.ec == std::errc()) out = v;
    return true;
}

}  // namespace

void guess_difficulty_lr2(std::string_view text, int& difficulty, std::string* title, std::string* subtitle) {
    std::string up = to_upper_ascii(text);
    auto ends = [&](std::string_view t) { return up.size() >= t.size() && std::string_view(up).substr(up.size() - t.size()) == t; };
    // 末尾の言葉（後に書いたものが勝つ）
    if (ends("HARD")) difficulty = 2;
    if (ends("HYPER")) difficulty = 3;
    if (ends("ANOTHER")) difficulty = 4;
    if (ends("EASY")) difficulty = 1;
    if (ends("EX")) difficulty = 4;
    if (ends("MANIAC")) difficulty = 4;
    // 末尾の括弧。開きは最後の文字より前で一番右にあるもの。開きが先頭なら分けない
    static const std::pair<std::string_view, std::string_view> kTokens[] = {
        {"(", ")"}, {"[", "]"}, {"-", "-"}, {"\"", "\""}, {"<", ">"}, {"\uFF5E", "\uFF5E"}, {"\u3010", "\u3011"}};
    for (auto [l, r] : kTokens) {
        if (up.size() < l.size() + 1) continue;
        size_t pos = up.rfind(l, up.size() - 1 - l.size());
        if (pos == std::string::npos || pos == 0 || !ends(r)) continue;
        if (title) *title = std::string(trim(text.substr(0, pos)));
        if (subtitle) *subtitle = std::string(trim(text.substr(pos)));
        // 括弧の中の言葉。上から順に、最初に含まれていたもの
        static const std::pair<std::string_view, int> kWords[] = {
            {"BEG", 1}, {"HARD", 3}, {"HYPE", 3}, {"HD", 3}, {"5H", 3}, {"7H", 3}, {"10H", 3}, {"14H", 3}, {"9H", 3},
            {"DIF", 3}, {"VERYHARD", 4}, {"EX", 4}, {"AN", 4}, {"SHD", 4}, {"5A", 4}, {"7A", 4}, {"10A", 4}, {"14A", 4},
            {"9A", 4}, {"ULT", 4}, {"MANI", 4}, {"LUNA", 4}, {"AHO", 4}, {"AFO", 4}, {"ASDF", 4}, {"HELL", 4}};
        std::string_view in = std::string_view(up).substr(pos + 1);
        for (auto [w, v] : kWords)
            if (in.find(w) != std::string_view::npos) {
                difficulty = v;
                break;
            }
        return;
    }
    if (title) *title = std::string(text);
    if (subtitle) subtitle->clear();
}

BmsParseResult parse_bms_text(std::string_view text, const BmsParseOptions& opts) {
    BmsParseResult res;
    Chart& chart = res.chart;
    chart.wav.assign(kSlots, std::string());
    chart.bmp.assign(kSlots, std::string());

    std::vector<double> bpm_slot(kSlots, -1.0);
    std::vector<int> stop_slot(kSlots, -1);
    std::map<int, double> measure_len;
    std::vector<Obj> objs;
    int lnobj = -1;
    int lnmode = 0;  // #LNMODE（beatoraja の拡張。1 = LN 2 = CN 3 = HCN。0 は決めていない）
    int total = 0;
    int rank = 2;  // LR2: rank = 2（bmsload:2167）
    double bpm_fix = 0.0, now_bpm = 0.0;
    bool base62 = false;
    bool if_on = true;
    int random_val = -1;
    Lr2Random rng(opts.random_seed);
    int order = 0;

    // UTF-8 の BOM は落とす（LR2 は CP932 として読むので 1 行目ごと捨ててしまうが、UTF-8 を読む以上は落とす）
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::istringstream in{std::string(text)};
    std::string raw;
    while (std::getline(in, raw)) {
        // LR2: 前後の空白と改行を落とし、'#' で始まらない行は無視する
        std::string_view sv = trim(raw);
        while (sv.size() >= 3 && sv.front() == ',') sv.remove_prefix(1);
        if (sv.empty() || sv.front() != '#') continue;
        std::string org(sv);
        std::string up = to_upper_ascii(org);

        // #RANDOM / #IF / #ENDIF は、読み飛ばし中の行でも評価する（LR2: bmsload:2317）。
        // 選曲用の読み込みは #RANDOM を評価せず、「#IF 1」で始まらない #IF の中を読み飛ばす。
        // こちらは大文字小文字を区別する（LR2: songmanage:2952-2957）
        if (opts.select_meta) {
            if (starts_with(org, "#IF")) if_on = starts_with(org, "#IF 1");
            else if (starts_with(org, "#ENDIF")) if_on = true;
        } else if (starts_with(up, "#RANDOM")) {
            int n = std::atoi(up.size() > 8 ? up.c_str() + 8 : "");
            random_val = rng.get_rand(n - 1) + 1;
            res.random_values.push_back(random_val);
            if_on = false;
        } else if (starts_with(up, "#ENDIF")) {
            if_on = true;
        } else if (starts_with(up, "#IF")) {
            if_on = std::atoi(up.size() > 4 ? up.c_str() + 4 : "") == random_val;
        }
        if (!if_on) continue;

        // チャンネル行 #mmmcc:...（LR2: bmsload:2335）
        if (up.size() >= 6 && std::isdigit(static_cast<unsigned char>(up[1])) &&
            std::isdigit(static_cast<unsigned char>(up[2])) && std::isdigit(static_cast<unsigned char>(up[3])) &&
            std::isdigit(static_cast<unsigned char>(up[5]))) {
            int measure = (up[1] - '0') * 100 + (up[2] - '0') * 10 + (up[3] - '0');
            int ch = (up[5] - '0') + hex_char(up[4]) * 10;
            if (ch == 4 || ch == 7) res.chart.sound_only = false;
            std::string data = org.size() > 7 ? org.substr(7) : std::string();
            if (ch == 2) {
                double len = std::atof(data.c_str());
                if (len > 0 && measure <= 4999) measure_len[measure] = len;
                continue;
            }
            Obj kind;
            if (!decode_channel(ch, kind)) continue;
            int div = static_cast<int>(data.size() / 2);
            for (int i = 0; i < div; ++i) {
                double val;
                if (ch == 3) {
                    val = hex_char(up[7 + i * 2]) * 16 + hex_char(up[7 + i * 2 + 1]);
                } else {
                    val = base_id(data[i * 2], data[i * 2 + 1], base62);
                }
                if (val == 0) continue;
                Obj o = kind;
                o.pos = measure + static_cast<double>(i) / div;
                o.val = val;
                o.order = order++;
                objs.push_back(o);
            }
            continue;
        }

        if (starts_with(up, "#BPM ")) {
            double v = std::atof(up.c_str() + 5);
            if (bpm_fix <= 0.0) bpm_fix = v;
            now_bpm = v;  // 最後の #BPM が開始BPMになる（LR2: bmsload:2424）
        } else if (starts_with(up, "#LNMODE ")) {
            int v = std::atoi(up.c_str() + 8);
            if (v >= 0 && v <= 3) lnmode = v;
        } else if (starts_with(up, "#LNOBJ ") && org.size() >= 9) {
            lnobj = base_id(org[7], org[8], base62);
        } else if (starts_with(up, "#TOTAL ")) {
            total = std::atoi(up.c_str() + 7);  // 整数（小数は切り捨て）
        } else if (starts_with(up, "#BASE 62")) {
            base62 = true;
        } else if (starts_with(up, "#RANK ")) {
            rank = std::atoi(up.c_str() + 6);
        } else if (starts_with(up, "#BPM") && org.size() >= 7) {
            int id = base_id(org[4], org[5], base62);
            if (id >= 1 && id < kSlots) bpm_slot[id] = std::atof(org.c_str() + 7);
        } else if (starts_with(up, "#STOP") && org.size() >= 8) {
            int id = base_id(org[5], org[6], base62);
            int v = std::atoi(org.c_str() + 8);
            if (id >= 1 && id < kSlots && v > 0) stop_slot[id] = v;
        } else if (starts_with(up, "#WAV") && org.size() >= 8) {
            int id = base_id(org[4], org[5], base62);
            if (id >= 0 && id < kSlots) chart.wav[id] = std::string(trim(std::string_view(org).substr(7)));
        } else if (starts_with(up, "#BMP") && org.size() >= 8) {
            int id = base_id(org[4], org[5], base62);
            if (id >= 0 && id < kSlots) chart.bmp[id] = std::string(trim(std::string_view(org).substr(7)));
        } else {
            // 以下は選曲画面用のメタデータ（LR2: ParseBMSMETA の ParseTextCommand。songmanage:208）
            ChartMeta& m = chart.meta;
            std::string v;
            if (text_header(up, org, "#TITLE", v)) {
                // 末尾の括弧を副題に分ける。分けられなければ副題は空（前の #SUBTITLE も消える）
                guess_difficulty_lr2(v, m.difficulty, &m.title, &m.subtitle);
            } else if (text_header(up, org, "#GENRE", v)) {
                m.genre = v;
                guess_difficulty_lr2(v, m.difficulty, nullptr, nullptr);
            } else if (text_header(up, org, "#CATEGORY", v)) m.genre = v;
            else if (text_header(up, org, "#SUBTITLE", v)) m.subtitle = v;
            else if (text_header(up, org, "#ARTIST", v)) m.artist = v;
            else if (text_header(up, org, "#SUBARTIST", v)) m.subartist = v;
            else if (text_header(up, org, "#INFORMATION_A", v)) m.artist = v;
            else if (text_header(up, org, "#INFORMATION_B", v)) m.subartist = v;
            else if (text_header(up, org, "#STAGEFILE", v)) m.stagefile = v;
            else if (text_header(up, org, "#BANNER", v)) m.banner = v;
            else if (text_header(up, org, "#BACKBMP", v)) m.backbmp = v;
            else if (int_header(up, "#PLAYLEVEL", m.playlevel)) {}
            else if (int_header(up, "#MAXTRACKS", m.playlevel)) {}  // LR2 は #PLAYLEVEL を上書きする（songmanage:2969）
            else if (int_header(up, "#DIFFICULTY", m.difficulty)) {}  // 推定と同じく、後に書いたほうが勝つ
        }
    }

    // DP（2P側）とPMSはこのプレイヤーの対象外。2P 側の地雷（E1〜E9）は LR2 も鍵盤数の判定に数えない
    // （songmanage:2996）ので、それだけなら SP の譜面として読み、地雷は捨てる
    bool p2_mine = false;
    auto p2_note = [](const Obj& o) {
        return o.side == 2 && (o.type == ObjType::Note || o.type == ObjType::Invisible || o.type == ObjType::LnMark);
    };
    for (const Obj& o : objs) {
        if (p2_note(o)) {
            res.double_play = true;
            if (!opts.allow_double) {
                res.error = "2P側のノーツがある（DP譜面は非対応）";
                return res;
            }
        }
        if (o.side == 2 && o.type == ObjType::Mine) p2_mine = true;
    }
    if (res.double_play) objs.erase(std::remove_if(objs.begin(), objs.end(), p2_note), objs.end());
    if (p2_mine) res.warnings.push_back("2P 側の地雷を捨てた");

    // 小節0の頭にノーツがあれば、全体を1小節後ろにずらす（LR2: bmsload:2581）
    bool shift = false;
    for (const Obj& o : objs)
        if (o.pos == 0.0 && (o.type == ObjType::Note || o.type == ObjType::Invisible)) shift = true;
    if (shift) {
        for (Obj& o : objs) o.pos += 1.0;
        std::map<int, double> shifted;
        for (auto& [m, l] : measure_len) shifted[m + 1] = l;
        // 頭に足した空の小節の長さは、元の小節 0 の長さ（#00002）になる（LR2: bmsload:2589）
        if (auto it = measure_len.find(0); it != measure_len.end()) shifted[0] = it->second;
        measure_len = std::move(shifted);
    }

    // 小節線（op 2）を各小節の頭に入れる。小節の長さはここで切り替わる
    int max_measure = 0;
    for (const Obj& o : objs) max_measure = std::max(max_measure, static_cast<int>(o.pos));
    for (int m = 0; m <= max_measure + 1; ++m) {
        Obj o;
        o.pos = m;
        o.type = ObjType::Measure, o.key = 2;
        o.val = measure_len.count(m) ? measure_len[m] : 1.0;
        o.order = order++;
        objs.push_back(o);
    }

    // 位置、同じ位置ならチャンネルの順（LR2: CMP_NotesByBmsTiming）
    std::stable_sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        return a.key < b.key;
    });

    // 時刻とスクロール位置を求める（LR2: bmsload:2670-2935）。
    // #BPM が無くても読む（LR2 は開始 BPM 0 のまま進める）。小節 0 の頭の 03/08 で BPM が決まれば普通に遊べる
    const double header_bpm = now_bpm;
    double real_ms = 0.0, beat = 0.0, prev_pos = 0.0, mea_len = 1.0;
    int pending_stop = 0;
    auto push_seg = [&](double t_ms, double b, double bpm, bool stopped) {
        ScrollSeg s;
        s.time_us = static_cast<int64_t>(std::llround(t_ms * 1000.0));
        s.pos = b;
        s.speed = stopped ? 0.0 : bpm / 60.0 / 1e6;
        s.bpm = bpm;
        s.stop = stopped;
        if (!chart.scroll.empty() && chart.scroll.back().time_us == s.time_us) chart.scroll.back() = s;
        else chart.scroll.push_back(s);
    };
    push_seg(0.0, 0.0, now_bpm, false);
    for (Obj& o : objs) {
        if (o.pos != prev_pos) {
            if (pending_stop != 0) {
                double stop_ms = (pending_stop / 192.0) * (240000.0 / now_bpm);
                if (stop_ms < 0) {
                    res.warnings.push_back("未定義の #STOP を参照している（無視した）");
                    stop_ms = 0;
                }
                push_seg(real_ms, beat, now_bpm, true);
                real_ms += stop_ms;
                push_seg(real_ms, beat, now_bpm, false);
                pending_stop = 0;
            }
            if (now_bpm <= 0.0) {
                // LR2 は時刻が無限大に飛んで遊べなくなる
                res.error = "#BPM が無い（曲の頭で BPM も決めていない）";
                return res;
            }
            double d = o.pos - prev_pos;
            real_ms += 240.0 / now_bpm * mea_len * d * 1000.0;
            beat += 4.0 * mea_len * d;
            prev_pos = o.pos;
        }
        o.real_ms = real_ms;
        o.beat = beat;
        o.bpm = now_bpm;  // 同じ位置の BPM の変更は先に処理してある
        if (o.type == ObjType::Measure) {
            mea_len = o.val;
            BarLine bl;
            bl.time_us = static_cast<int64_t>(std::llround(real_ms * 1000.0));
            bl.pos = beat;
            chart.bars.push_back(bl);
        } else if (o.type == ObjType::Bpm || o.type == ObjType::ExBpm) {
            double bpm = o.val;
            if (o.type == ObjType::ExBpm) {
                int id = static_cast<int>(o.val);
                bpm = (id >= 0 && id < kSlots) ? bpm_slot[id] : -1.0;
                if (bpm == -1.0) bpm = 1.0;  // 未定義の #BPMxx は1.0（LR2: bmsload:2844）
                if (bpm < 0) bpm = -bpm;
            }
            if (bpm <= 0.0) {
                res.warnings.push_back("BPM 0 への変更（無視した）");
            } else {
                now_bpm = bpm;
                push_seg(real_ms, beat, now_bpm, false);
            }
        } else if (o.type == ObjType::Stop) {
            int id = static_cast<int>(o.val);
            pending_stop = (id >= 0 && id < kSlots) ? stop_slot[id] : -1;  // 同じ位置のSTOPは最後のものだけ
        }
    }
    chart.initial_bpm = header_bpm > 0.0 ? header_bpm : chart.scroll.front().bpm;

    // LNの組み立て（LR2: bmsload:2755）。時刻順に見て、5x/6x の端は始点と終点を交互に、#LNOBJ の値のノーツは直前のノーツの終点にする。
    // 「始点が開いている」はレーンごとに 1 つで、5x/6x と #LNOBJ で同じものを使う（混ざった譜面でもそのまま組になる）
    std::stable_sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) {
        if (a.real_ms != b.real_ms) return a.real_ms < b.real_ms;
        if (a.pos != b.pos) return a.pos < b.pos;
        return a.key < b.key;
    });
    int open_head[3][kLaneCount];  // [1P/2P][レーン] → 始点の objs の番号（-1 = 開いていない）
    for (auto& row : open_head) std::fill(std::begin(row), std::end(row), -1);
    auto close_ln = [&](Obj& end, int& head) {
        Obj& h = objs[static_cast<size_t>(head)];
        h.real_ln = end.real_ms;
        h.beat_ln = end.beat;
        head = -1;
        end.type = ObjType::LnEnd;  // 終点は以後使わない（終点のキー音は鳴らない）
    };
    for (size_t i = 0; i < objs.size(); ++i) {
        Obj& o = objs[i];
        if (o.type == ObjType::LnMark) {
            int& head = open_head[o.side][o.lane];
            if (head == -1) {
                o.type = ObjType::Note;  // 始点はふつうのノーツとしてレーンに入る
                head = static_cast<int>(i);
            } else {
                close_ln(o, head);
            }
        } else if (o.type == ObjType::Note && lnobj != -1) {
            int& head = open_head[o.side][o.lane];
            if (static_cast<int>(o.val) == lnobj && head != -1) close_ln(o, head);
            else head = static_cast<int>(i);
        } else if (o.type == ObjType::Mine) {
            // 地雷：値はダメージ。レーンにはノーツとして入れる（2P 側の地雷は使わない）
            o.mine = static_cast<int>(o.val);
            o.val = 0;
            o.type = o.side == 1 ? ObjType::Note : ObjType::Other;
        }
    }

    // レーンに入れる。同じ時刻は先に来たものが勝ち、LNの始点だけは通常ノーツを上書きする。
    // LNの終点以前に始まるノーツは捨てる（LR2: bmsload:3664）
    int last_in_lane[kLaneCount];
    std::fill(std::begin(last_in_lane), std::end(last_in_lane), -1);
    std::vector<Note> lane_notes;
    auto to_us = [](double ms) { return static_cast<int64_t>(std::llround(ms * 1000.0)); };
    bool has_scratch = false;
    std::map<double, int> per_bpm;  // 主 BPM 用。重なりで捨てるノーツも数える（LR2: bmsload:2899）
    for (const Obj& o : objs) {
        if (o.type == ObjType::Bgm) {
            chart.bgm.push_back({to_us(o.real_ms), static_cast<uint16_t>(o.val)});
            continue;
        }
        if (o.type == ObjType::Bga || o.type == ObjType::BgaPoor || o.type == ObjType::BgaLayer) {
            BgaEvent e;
            e.time_us = to_us(o.real_ms);
            e.layer = o.type == ObjType::Bga ? BgaEvent::Layer::Base : o.type == ObjType::BgaPoor ? BgaEvent::Layer::Poor : BgaEvent::Layer::Layer;
            e.image = static_cast<uint16_t>(o.val);
            chart.bga.push_back(e);
            continue;
        }
        if (o.type == ObjType::Invisible && o.side == 1) {
            Note n;
            n.time_us = to_us(o.real_ms);
            n.pos = o.beat;
            n.lane = static_cast<uint8_t>(o.lane);
            n.kind = NoteKind::Invisible;
            n.sound = static_cast<uint16_t>(o.val);
            lane_notes.push_back(n);
            continue;
        }
        if (o.type != ObjType::Note || o.side != 1) continue;
        per_bpm[o.bpm]++;
        const int lane = o.lane;
        Note n;
        n.time_us = to_us(o.real_ms);
        n.pos = o.beat;
        n.lane = static_cast<uint8_t>(lane);
        n.sound = static_cast<uint16_t>(o.val);
        if (o.mine > 0) {
            n.kind = NoteKind::Mine;
            n.damage = o.mine;
        } else if (o.real_ln >= 0) {
            n.kind = NoteKind::Long;
            n.end_us = to_us(o.real_ln);
            n.end_pos = o.beat_ln;
        }
        int prev = last_in_lane[lane];
        if (prev == -1) {
            last_in_lane[lane] = static_cast<int>(lane_notes.size());
            lane_notes.push_back(n);
        } else {
            Note& p = lane_notes[prev];
            int64_t p_end = p.kind == NoteKind::Long ? p.end_us : -1;
            if (n.time_us > p_end) {
                if (n.time_us != p.time_us) {
                    last_in_lane[lane] = static_cast<int>(lane_notes.size());
                    lane_notes.push_back(n);
                } else if (n.kind == NoteKind::Long && n.end_us > n.time_us) {
                    // 長さ 0 の LN は上書きしない（LR2: bmsload:3678）
                    p = n;
                } else {
                    res.warnings.push_back("同じ時刻に重なったノーツを捨てた");
                }
            } else {
                res.warnings.push_back("LNの途中に始まるノーツを捨てた");
            }
        }
        if (lane == 0 && n.kind != NoteKind::Mine) has_scratch = true;
    }
    std::stable_sort(lane_notes.begin(), lane_notes.end(),
                     [](const Note& a, const Note& b) { return a.time_us < b.time_us; });
    for (Note& n : lane_notes)
        if (n.kind == NoteKind::Long) n.ln_type = static_cast<uint8_t>(lnmode);
    chart.notes = std::move(lane_notes);
    chart.layout = has_scratch ? KeyLayout::K7S : KeyLayout::K7;

    // 主BPM：ノーツが一番多いBPM（LR2: bmsload:3315）
    chart.main_bpm = now_bpm;
    int best = 0;
    for (auto& [bpm, cnt] : per_bpm)
        if (cnt > best) best = cnt, chart.main_bpm = bpm;

    // TOTAL：未定義（1未満）ならLR2の既定式で補う（LR2: bmsload:3790）。配置のオプションでノーツが減ったら、遊ぶ前に数え直す
    chart.total_defined = total >= 1;
    chart.total = total >= 1 ? total : lr2_default_total(chart.judgeable_notes());
    chart.judge_rank = rank;

    // 曲の終わり：最後のノーツ（LNなら終点）+2秒と、最後のBGMの遅いほう。不可視ノーツは数えない（LR2: bmsload:3499, 3649）
    int64_t end = 0;
    for (const Note& n : chart.notes)
        if (n.kind != NoteKind::Invisible) end = std::max(end, std::max(n.time_us, n.end_us) + 2000000);
    for (const BgmEvent& b : chart.bgm) end = std::max(end, b.time_us);
    chart.end_us = end;
    res.rng = rng;
    return res.ok = true, res;
}

BmsParseResult parse_bms_file(const std::string& path, const BmsParseOptions& opts) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        BmsParseResult r;
        r.error = "ファイルを開けない: " + path;
        return r;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BmsParseResult r = parse_bms_text(to_utf8_auto(bytes), opts);
    r.chart.base_dir = parent_dir(path);
    return r;
}

}  // namespace rd
