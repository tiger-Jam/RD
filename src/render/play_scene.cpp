#include "render/play_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bmj {

namespace {

// 7_LL0.csv の #DST_NOTE（x, w）。0=皿, 1..7=鍵盤
constexpr float kLaneX[kLaneCount] = {33, 76, 100, 119, 143, 162, 186, 205};
constexpr float kLaneW[kLaneCount] = {41, 22, 17, 22, 17, 22, 17, 22};
constexpr float kJudgeY = 315;  // 判定ライン（#DST_JUDGELINE の y）
constexpr float kNoteH = 6;
constexpr float kLaneLeft = 33, kLaneRight = 227;

SDL_Color lane_color(int lane) {
    if (lane == 0) return {230, 60, 60, 255};
    if (lane % 2 == 0) return {80, 140, 255, 255};
    return {235, 235, 235, 255};
}

void fill(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

void text(SDL_Renderer* r, float x, float y, SDL_Color c, const char* s, float scale = 1.0f) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, s);
    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

const char* judge_name(int j) {
    switch (j) {
    case kPGreat: return "GREAT";  // LR2のPGREATは虹色の GREAT 表示
    case kGreat: return "GREAT";
    case kGood: return "GOOD";
    case kBad: return "BAD";
    case kPoor: return "POOR";
    case kEmptyPoor: return "POOR";
    default: return "";
    }
}

}  // namespace

void draw_play(SDL_Renderer* r, const Chart& chart, const JudgeEngine& judge, const PlayViewState& v) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    // レーンの背景と、押しているレーンの光
    fill(r, kLaneLeft - 3, 0, kLaneRight - kLaneLeft + 6, kJudgeY + 8, {20, 20, 28, 255});
    for (int lane = 0; lane < kLaneCount; ++lane) {
        fill(r, kLaneX[lane], 0, kLaneW[lane], kJudgeY, lane % 2 == 0 && lane != 0 ? SDL_Color{14, 14, 22, 255}
                                                                                       : SDL_Color{24, 24, 32, 255});
        if (judge.lane_held(lane)) {
            SDL_Color c = lane_color(lane);
            c.a = 60;
            fill(r, kLaneX[lane], kJudgeY - 120, kLaneW[lane], 120, c);
        }
    }

    // ノーツの位置：LR2では 1小節(4拍)=1920 で、y = (今 − ノーツ) × ハイスピ / 600（Play.cpp:463）
    double px_per_beat = 480.0 * v.speed_multiplier * v.hi_speed / 600.0;
    auto beat_to_y = [&](double beat) {
        double y = (v.render_beat - beat) * px_per_beat;
        return static_cast<float>(kJudgeY + std::min(0.0, y));
    };

    // 小節線
    SDL_SetRenderDrawColor(r, 120, 120, 120, 255);
    for (const BarLine& b : chart.bars) {
        float y = beat_to_y(b.pos);
        if (y < -10 || b.pos < v.render_beat) continue;
        SDL_RenderLine(r, kLaneLeft, y + kNoteH / 2, kLaneRight, y + kNoteH / 2);
    }

    for (size_t i = 0; i < chart.notes.size(); ++i) {
        const Note& n = chart.notes[i];
        if (n.kind == NoteKind::Invisible) continue;
        if (judge.note_done(i)) continue;
        float y = beat_to_y(n.pos);
        if (n.kind == NoteKind::Long) {
            float ye = beat_to_y(n.end_pos);
            if (y < -kNoteH && ye < -kNoteH) break;
            SDL_Color c = lane_color(n.lane);
            SDL_Color body = c;
            body.a = judge.ln_holding(i) ? 220 : 130;
            float top = std::max(ye, -10.0f);
            fill(r, kLaneX[n.lane] + 2, top + kNoteH, kLaneW[n.lane] - 4, y - top, body);
            fill(r, kLaneX[n.lane], y, kLaneW[n.lane], kNoteH, c);
            fill(r, kLaneX[n.lane], ye, kLaneW[n.lane], kNoteH, c);
            continue;
        }
        if (y < -kNoteH) {
            // これより先は画面外（時刻順に並んでいるが、LNの終点は先にあり得るので続ける）
            continue;
        }
        if (n.kind == NoteKind::Mine) {
            fill(r, kLaneX[n.lane], y, kLaneW[n.lane], kNoteH, {160, 0, 0, 255});
            fill(r, kLaneX[n.lane] + 3, y + 1, kLaneW[n.lane] - 6, kNoteH - 2, {255, 90, 0, 255});
            continue;
        }
        fill(r, kLaneX[n.lane], y, kLaneW[n.lane], kNoteH, lane_color(n.lane));
    }

    // 判定ライン
    fill(r, kLaneLeft, kJudgeY, kLaneRight - kLaneLeft, kNoteH, {200, 40, 40, 255});

    // 判定とコンボ（判定は1.3秒で消える。7_LL0 の #DST_NOWJUDGE_1P に合わせた位置）
    if (v.last_judge >= 0 && v.song_ms - v.last_judge_ms < 1300) {
        bool flash = v.last_judge == kPGreat && (static_cast<int>(v.song_ms / 50) % 2 == 0);
        SDL_Color c = v.last_judge == kPGreat ? (flash ? SDL_Color{255, 255, 120, 255} : SDL_Color{120, 255, 255, 255})
                      : v.last_judge >= kGood ? SDL_Color{255, 200, 60, 255}
                                              : SDL_Color{255, 80, 80, 255};
        char buf[64];
        if (v.last_judge >= kGood && v.combo > 0) std::snprintf(buf, sizeof(buf), "%s %d", judge_name(v.last_judge), v.combo);
        else std::snprintf(buf, sizeof(buf), "%s", judge_name(v.last_judge));
        float w = static_cast<float>(std::strlen(buf)) * 8 * 2;
        text(r, (kLaneLeft + kLaneRight) / 2 - w / 2, 230, c, buf, 2.0f);
        if (v.last_has_offset && v.last_judge != kPGreat) {
            text(r, (kLaneLeft + kLaneRight) / 2 - 16, 252, v.last_fast ? SDL_Color{80, 160, 255, 255} : SDL_Color{255, 110, 80, 255},
                 v.last_fast ? "FAST" : "SLOW");
        }
    }

    // ゲージ：50個のセグメント（1つ2%）。80%以上は赤（7_LL0 の #DST_GROOVEGAUGE は 44,387 から4pxずつ）
    int type = judge.gauge_type();
    double hp = judge.gauge().hp(type);
    int segs = static_cast<int>(hp) / 2;
    bool survival = !(type == kGroove || type == kEasy);
    for (int s = 0; s < 50; ++s) {
        bool on = s < segs;
        bool red = survival || s >= 39;
        SDL_Color c = red ? SDL_Color{230, 40, 40, 255} : SDL_Color{40, 200, 80, 255};
        if (!on) c = red ? SDL_Color{70, 15, 15, 255} : SDL_Color{15, 60, 25, 255};
        fill(r, 44 + s * 4.0f, 387, 3, 14, c);
    }
    char buf[256];
    static const char* gauge_names[] = {"GROOVE", "HARD", "DEATH", "EASY", "P-ATTACK", "G-ATTACK"};
    std::snprintf(buf, sizeof(buf), "%s %3d%%", gauge_names[type], static_cast<int>(hp / 2) * 2);
    text(r, 44, 405, {255, 255, 255, 255}, buf);

    // 右側の情報
    const PlayStats& st = judge.stats();
    SDL_Color white{230, 230, 230, 255}, dim{150, 150, 150, 255};
    float x = 260;
    text(r, x, 20, white, chart.meta.title.c_str());
    text(r, x, 32, dim, chart.meta.artist.c_str());
    std::snprintf(buf, sizeof(buf), "BPM %3.0f   HS %d.%02d", chart.bpm_at(static_cast<int64_t>(v.song_ms * 1000)),
                  v.hi_speed / 100, v.hi_speed % 100);
    text(r, x, 52, white, buf);
    int y = 330;
    std::snprintf(buf, sizeof(buf), "EX SCORE %5d   MAX COMBO %4d", st.exscore(), st.max_combo);
    text(r, x, y, white, buf);
    y += 14;
    std::snprintf(buf, sizeof(buf), "PG %4d  GR %4d  GD %4d", st.judge_count[kPGreat], st.judge_count[kGreat],
                  st.judge_count[kGood]);
    text(r, x, y, white, buf);
    y += 12;
    std::snprintf(buf, sizeof(buf), "BD %4d  PR %4d  BP %4d", st.judge_count[kBad], st.judge_count[kPoor], st.bp());
    text(r, x, y, white, buf);
    y += 12;
    std::snprintf(buf, sizeof(buf), "FAST %4d  SLOW %4d   %d/%d", st.fast, st.slow, st.note_current, st.total_notes);
    text(r, x, y, white, buf);
    if (v.autoplay) text(r, x, 80, {255, 220, 80, 255}, "AUTO PLAY");
    if (v.failed) text(r, 80, 200, {255, 60, 60, 255}, "FAILED", 3.0f);
    if (!v.status.empty()) text(r, 4, 468, dim, v.status.c_str());
}

void draw_loading(SDL_Renderer* r, const Chart& chart, int done, int total) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    text(r, 40, 200, {230, 230, 230, 255}, chart.meta.title.c_str(), 2.0f);
    text(r, 40, 224, {150, 150, 150, 255}, chart.meta.artist.c_str());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "LOADING %d / %d", done, total);
    text(r, 40, 260, {230, 230, 230, 255}, buf);
}

void draw_result(SDL_Renderer* r, const Chart& chart, const JudgeEngine& judge) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    const PlayStats& st = judge.stats();
    static const char* lamps[] = {"NO PLAY", "FAILED", "EASY CLEAR", "CLEAR", "HARD CLEAR", "FULL COMBO"};
    int lamp = judge.failed() ? 1 : judge.clear();
    SDL_Color white{230, 230, 230, 255};
    text(r, 40, 40, white, chart.meta.title.c_str(), 2.0f);
    text(r, 40, 80, lamp >= 2 ? SDL_Color{255, 220, 80, 255} : SDL_Color{255, 80, 80, 255}, lamps[lamp], 3.0f);
    // DJ LEVEL（LR2: statplay.cpp:636）
    static const char* ranks[] = {"F", "F", "E", "D", "C", "B", "A", "AA", "AAA"};
    int rank = st.total_notes > 0 ? st.exscore() * 9 / (st.total_notes * 2) : 0;
    rank = std::clamp(rank, st.exscore() > 0 ? 1 : 0, 8);
    char buf[128];
    int y = 140;
    auto line = [&](const char* s) {
        text(r, 40, static_cast<float>(y), white, s, 2.0f);
        y += 24;
    };
    std::snprintf(buf, sizeof(buf), "DJ LEVEL  %s", ranks[rank]);
    line(buf);
    std::snprintf(buf, sizeof(buf), "EX SCORE  %d / %d", st.exscore(), st.total_notes * 2);
    line(buf);
    std::snprintf(buf, sizeof(buf), "MAX COMBO %d", st.max_combo);
    line(buf);
    std::snprintf(buf, sizeof(buf), "PGREAT %d  GREAT %d", st.judge_count[kPGreat], st.judge_count[kGreat]);
    line(buf);
    std::snprintf(buf, sizeof(buf), "GOOD %d  BAD %d  POOR %d", st.judge_count[kGood], st.judge_count[kBad],
                  st.judge_count[kPoor]);
    line(buf);
    std::snprintf(buf, sizeof(buf), "BP %d   FAST %d  SLOW %d", st.bp(), st.fast, st.slow);
    line(buf);
    text(r, 40, 440, {150, 150, 150, 255}, "ENTER / ESC: QUIT   F5: RETRY");
}

}  // namespace bmj
