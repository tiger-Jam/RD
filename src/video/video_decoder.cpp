#include "video/video_decoder.h"

#include <algorithm>
#include <cstdio>

namespace rd {

namespace {

constexpr size_t kQueueMax = 6;  // 先に用意しておくフレーム数

// 見つかった FFmpeg の版（新しい版から探す）。無ければ nullptr
using Create = std::unique_ptr<VideoBackend> (*)();
Create backend() {
    static Create c = [] () -> Create {
        if (ff9::load()) return ff9::create;
        if (ff8::load()) return ff8::create;
        std::printf("FFmpeg（8 系か 9 系）が見つからないので、動画の BGA は出さない\n");
        return nullptr;
    }();
    return c;
}

}  // namespace

bool VideoDecoder::available() { return backend() != nullptr; }

VideoDecoder::~VideoDecoder() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool VideoDecoder::open(const std::string& path, int key) {
    Create c = backend();
    if (!c) return false;
    impl_ = c();
    if (!impl_->open(path)) {
        impl_.reset();
        return false;
    }
    key_ = key;
    w_ = impl_->width(), h_ = impl_->height();
    thread_ = std::thread([this] { run(); });
    return true;
}

void VideoDecoder::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            // 終わりまでデコードしたら、頭に戻すよう言われるまで待つ
            cv_.wait(lock, [&] { return stop_ || restart_ || !eof_; });
            if (stop_) return;
            if (restart_) {
                restart_ = false, eof_ = false;
                queue_.clear();
                loop_offset_ = last_t_ = 0;
                lock.unlock();
                impl_->rewind();
                continue;
            }
        }
        Frame f;
        if (!impl_->next(f)) {
            if (loop_ && last_t_ > 0) {
                // 繰り返す：1 フレーム分（約 1/30 秒）の間を空けて頭からつなぐ
                loop_offset_ += last_t_ + 1000.0 / 30.0;
                last_t_ = 0;
                impl_->rewind();
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            if (!restart_) eof_ = true;
            continue;
        }
        last_t_ = std::max(last_t_, f.t_ms);
        f.t_ms += loop_offset_;
        if (key_ >= 0) {
            // 透過色の画素を透明にする（LR2 の SetTransColor と同じく、色がぴったり同じものだけ）
            const uint8_t kr = key_ >> 16 & 255, kg = key_ >> 8 & 255, kb = key_ & 255;
            for (size_t i = 0; i < f.rgba.size(); i += 4)
                if (f.rgba[i] == kr && f.rgba[i + 1] == kg && f.rgba[i + 2] == kb) f.rgba[i + 3] = 0;
        }
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return stop_ || restart_ || queue_.size() < kQueueMax; });
        if (stop_) return;
        if (restart_) continue;  // 頭に戻すので、このフレームは捨てる
        queue_.push_back(std::move(f));
    }
}

void VideoDecoder::restart() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        restart_ = true;
        queue_.clear();
    }
    cv_.notify_all();
}

bool VideoDecoder::take(double t_ms, Frame& out) {
    std::lock_guard<std::mutex> lock(mu_);
    bool got = false;
    while (!queue_.empty() && queue_.front().t_ms <= t_ms) {
        out = std::move(queue_.front());
        queue_.pop_front();
        got = true;
    }
    if (got) cv_.notify_all();
    return got;
}

}  // namespace rd
