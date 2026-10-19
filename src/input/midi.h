// MIDI の入力（鍵盤のノート・ピッチベンド・ペダル）。つながっている MIDI の入力機器を全部開き、
// 押した・離したを SDL のイベント（push_midi_event）で送るので、InputMapper とキーコンフィグの画面はそのまま使える。
// macOS は CoreMIDI、Windows は winmm（midiIn）。ほかの OS では何もしない
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "input/keyconfig.h"

namespace bmj {

// MIDI のメッセージを入力元（MidiSource）の押した・離したに変える。読み方は LR2 と同じ：
//   ノートオン（ベロシティ 0 はオフ）・ノートオフ、ピッチベンドは上位 7 ビットが 64 より下なら −、上なら +、64 ならどちらも離す、
//   コントロールチェンジ 64（ダンパーペダル）は 64 より下で離す・上で押す（64 ちょうどは変えない）。
// チャンネルと機器は区別しない。同じ状態のままのメッセージは何も出さない
class MidiDecoder {
public:
    // status・data1・data2 は MIDI 1.0 の 3 バイト。変わった入力元を (source, down) で out に足す
    void feed(uint8_t status, uint8_t data1, uint8_t data2, std::vector<std::pair<int, bool>>& out);
    // 押している入力元を全部離す（機器を抜いたとき）
    void release_all(std::vector<std::pair<int, bool>>& out);

private:
    void set(int source, bool down, std::vector<std::pair<int, bool>>& out);
    std::array<bool, kMidiSourceCount> down_{};
};

class MidiInput {
public:
    // 機器を開いて読み始める（SDL を初期化した後に呼ぶ）。つないだ・抜いたも追う
    void start();
    void stop();
    // 開いている機器の名前（キーコンフィグの一覧に出す）
    std::vector<std::string> devices() const;
    ~MidiInput() { stop(); }

    struct Impl;

private:
    [[maybe_unused]] Impl* impl_ = nullptr;
};

MidiInput& midi_input();

}  // namespace bmj
