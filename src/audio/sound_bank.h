// キー音の読み込み。すべて出力のサンプルレート・ステレオのfloat PCMに展開してメモリに載せる（仕様書12章）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

struct Sample {
    std::vector<float> pcm;  // ステレオのインターリーブ
    int64_t frames() const { return static_cast<int64_t>(pcm.size() / 2); }
};

// 1ファイルをデコードする。中身の先頭バイトで形式を判定する（拡張子は見ない）
bool decode_sound_file(const std::string& path, uint32_t out_rate, Sample& out, std::string& error);

class SoundBank {
public:
    // paths[id] が空なら、そのIDは無音。threads 本のワーカーで並列にデコードする
    void load(const std::vector<std::string>& paths, uint32_t out_rate, int threads,
              std::vector<std::string>* errors);
    const std::vector<Sample>& samples() const { return samples_; }

private:
    std::vector<Sample> samples_;
};

}  // namespace bmj
