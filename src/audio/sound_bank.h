// キー音の読み込み。すべて出力のサンプルレート・ステレオの PCM に展開してメモリに載せる（仕様書12章）。
// 持ち方は float（既定）か 16 ビットの整数（メモリが半分。BMS の音はほぼ 16 ビットなので音は変わらない）
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

struct Sample {
    std::vector<float> pcm;      // ステレオのインターリーブ（float で持つとき）
    std::vector<int16_t> pcm16;  // 同じく 16 ビットで持つとき（どちらか片方だけを使う）
    int64_t frames() const { return static_cast<int64_t>((pcm16.empty() ? pcm.size() : pcm16.size()) / 2); }
    bool empty() const { return pcm.empty() && pcm16.empty(); }
    // float の中身を 16 ビットにして、float の側は捨てる
    void to_16bit();
};

// 1ファイルをデコードする。中身の先頭バイトで形式を判定する（拡張子は見ない）
bool decode_sound_file(const std::string& path, uint32_t out_rate, Sample& out, std::string& error);

class SoundBank {
public:
    // paths[id] が空なら、そのIDは無音。threads 本のワーカーで並列にデコードする（同じファイルは 1 回だけ）。
    // ranges[id] があれば、その範囲（マイクロ秒。終わりが負ならファイルの終わりまで）だけを切り出す（bmson の継続ノーツ）。
    // done には読み終えた数を数える（読み込み画面の進み具合に使う）
    // pcm16 なら 16 ビットで持つ（デコードしたそばから直すので、読み込み中のメモリも増えない）
    void load(const std::vector<std::string>& paths, uint32_t out_rate, int threads,
              std::vector<std::string>* errors, std::atomic<int>* done = nullptr,
              const std::vector<std::pair<int64_t, int64_t>>* ranges = nullptr, bool pcm16 = false);
    const std::vector<Sample>& samples() const { return samples_; }
    // id の音の大きさを gain 倍にする（読み終えたあと、鳴らす前に）
    void scale(size_t id, float gain);

private:
    std::vector<Sample> samples_;
};

}  // namespace bmj
