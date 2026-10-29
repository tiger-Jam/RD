// 動画のデコードの中身。FFmpeg の版ごとに別々にビルドし（ヘッダーの構造体が版で変わるため）、
// 実行時に見つかった版のものを使う
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rd {

struct VideoFrame {
    double t_ms = 0;  // 動画の頭からの時刻
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

class VideoBackend {
public:
    virtual ~VideoBackend() = default;
    virtual bool open(const std::string& path) = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // 次のフレームを RGBA にする。終わりまで来たら false
    virtual bool next(VideoFrame& f) = 0;
    // 頭に戻す
    virtual void rewind() = 0;
};

// FFmpeg の各版。ライブラリを読み込めたら true（一度だけ読み込む）
namespace ff9 {
bool load();
std::unique_ptr<VideoBackend> create();
}  // namespace ff9
namespace ff8 {
bool load();
std::unique_ptr<VideoBackend> create();
}  // namespace ff8

}  // namespace rd
