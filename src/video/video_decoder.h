// 動画の BGA（仕様書3章）。FFmpeg は同梱せず、共有ライブラリが置いてあれば実行時に読み込んで使う。
// 読み込めなければ available() が偽になり、動画の BGA は出さない
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmj {

class VideoDecoder {
public:
    struct Frame {
        double t_ms = 0;  // 動画の頭からの時刻
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    // FFmpeg を読み込めたか（初めて呼んだときに読み込む）
    static bool available();

    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // 開いて、裏でデコードを始める（頭の数フレームを先に用意しておく）
    bool open(const std::string& path);
    // 動画の時刻 t_ms までに出すべき一番新しいフレームを取り出す（新しいフレームが無ければ false）
    bool take(double t_ms, Frame& out);
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void run();
    void close();

    struct Ctx;
    Ctx* ctx_ = nullptr;
    int w_ = 0, h_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    bool stop_ = false, eof_ = false;
};

}  // namespace bmj
