// 内部譜面モデル（仕様書5章）。どのフォーマットもここに変換し、ゲーム本体はフォーマットを知らない。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

constexpr int kLaneCount = 8;  // 0=皿, 1..7=鍵盤

enum class NoteKind : uint8_t { Normal, Long, Mine, Invisible };

struct Note {
    int64_t time_us = 0;    // 判定時刻（マイクロ秒）
    int64_t end_us = -1;    // LN終端（通常ノーツは-1）
    double pos = 0.0;       // スクロール位置（拍。BPM/STOP適用済み）
    double end_pos = 0.0;
    uint8_t lane = 0;
    NoteKind kind = NoteKind::Normal;
    uint16_t sound = 0;     // キー音ID（0=無し）
    uint16_t end_sound = 0; // LN終端のキー音ID（#LNOBJのとき）
    int damage = 0;         // 地雷のダメージ（譜面の値そのまま）
};

struct BgmEvent {
    int64_t time_us = 0;
    uint16_t sound = 0;
};

struct BgaEvent {
    enum class Layer : uint8_t { Base, Poor, Layer };
    int64_t time_us = 0;
    Layer layer = Layer::Base;
    uint16_t image = 0;
};

// 時刻→位置の区分線形テーブルの1区間。速さの変化は種類ごとに持つ（仕様書6章「ソフランの種類」）：
// BPM の変化は bpm、スクロール速度の変化（BMS の #SCROLL、osu の SV）は scroll、停止は stop。
// speed はそれらを合わせた結果（BPM × scroll。停止中は 0）
struct ScrollSeg {
    int64_t time_us = 0;
    double pos = 0.0;
    double speed = 0.0;   // 拍/マイクロ秒
    double bpm = 0.0;     // 表示用（STOP中も直前のBPM）
    double scroll = 1.0;  // スクロール速度の倍率
    bool stop = false;
};

// 音の切り出し（bmson の継続ノーツ）。ファイルの start_us から end_us まで（end_us が負ならファイルの終わりまで）
struct SoundSlice {
    int64_t start_us = 0, end_us = -1;
};

struct BarLine {
    int64_t time_us = 0;
    double pos = 0.0;
};

enum class LnHint : uint8_t { None, Ln, Cn, Hcn };
enum class KeyLayout : uint8_t { K7S, K7 };

struct ChartMeta {
    std::string title, subtitle, artist, subartist, genre;
    std::string stagefile, banner, backbmp;
    int playlevel = 0;
    int difficulty = 0;
};

struct Chart {
    ChartMeta meta;
    std::vector<Note> notes;  // time_us 昇順
    std::vector<BgmEvent> bgm;
    std::vector<BgaEvent> bga;
    std::vector<ScrollSeg> scroll;
    std::vector<BarLine> bars;
    std::vector<std::string> wav;  // ID→ファイル名（譜面フォルダからの相対）
    std::vector<SoundSlice> wav_slice;  // ID→切り出し（空ならどれもファイル全体）
    std::vector<std::string> bmp;
    std::string base_dir;
    LnHint ln_hint = LnHint::None;
    KeyLayout layout = KeyLayout::K7S;
    double total = 0.0;  // 0なら未定義（既定式で補う）
    double initial_bpm = 130.0;
    double main_bpm = 130.0;
    int judge_rank = 2;  // LR2の#RANK。0=VERY HARD, 1=HARD, 2=NORMAL, 3=EASY
    double judge_scale = 1.0;  // 判定幅の倍率（bmson の judge_rank。100% = 1.0）
    int64_t end_us = 0;  // 最後のイベントの時刻

    // 時刻からスクロール位置を求める（描画用）
    double pos_at(int64_t time_us) const;
    double bpm_at(int64_t time_us) const;
    // 判定対象のノーツ数（LNモードLN：LN1本＝1）
    int judgeable_notes() const;
    // BMS 以外のインポーター用の仕上げ。scroll に time_us・bpm・scroll・stop を入れて時刻順に並べてから呼ぶ。
    // 区間の速さと位置を決め、ノーツと小節線の位置を付け、主 BPM・TOTAL（未定義なら LR2 の既定式）・曲の終わりを決める
    void finish_import();
    // 一定速度にする（HS-FIX の CONSTANT）。BPM とスクロール速度の変化を無視し、BPM 150 の速さで流す。停止では止まる。
    // ノーツと小節線の位置も付け直す
    void make_constant_speed();
};

}  // namespace bmj
