// 動画の BGA（仕様書3章）。FFmpeg は同梱せず、共有ライブラリが置いてあれば実行時に読み込んで使う。
// 読み込めなければ available() が偽になり、動画の BGA は出さない
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video/video_backend.h"

namespace bmj {

class VideoDecoder {
public:
    using Frame = VideoFrame;

    // FFmpeg を読み込めたか（初めて呼んだときに読み込む）
    static bool available();

    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // 開いて、裏でデコードを始める（頭の数フレームを先に用意しておく）。
    // key は透過色（RGB。この色の画素を透明にする）。負なら透過しない
    bool open(const std::string& path, int key = -1);
    // 頭から再生し直す（LR2 は BGA に出るたびに頭から再生する）
    void restart();
    // 動画の時刻 t_ms までに出すべき一番新しいフレームを取り出す（新しいフレームが無ければ false）
    bool take(double t_ms, Frame& out);
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void run();

    std::unique_ptr<VideoBackend> impl_;
    int w_ = 0, h_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    bool stop_ = false, eof_ = false, restart_ = false;
    int key_ = -1;
};

}  // namespace bmj
