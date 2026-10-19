#include "video/video_decoder.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace bmj {

namespace {

// 実行時に読み込む FFmpeg の関数。ヘッダーと同じ版（メジャー番号）のライブラリだけを使う
struct Api {
    bool ok = false;
    // libavutil
    AVFrame* (*frame_alloc)();
    void (*frame_free)(AVFrame**);
    void (*log_set_level)(int);
    // libavcodec
    AVPacket* (*packet_alloc)();
    void (*packet_free)(AVPacket**);
    void (*packet_unref)(AVPacket*);
    AVCodecContext* (*codec_alloc)(const AVCodec*);
    int (*params_to_ctx)(AVCodecContext*, const AVCodecParameters*);
    int (*codec_open)(AVCodecContext*, const AVCodec*, AVDictionary**);
    int (*send_packet)(AVCodecContext*, const AVPacket*);
    int (*receive_frame)(AVCodecContext*, AVFrame*);
    void (*codec_free)(AVCodecContext**);
    // libavformat
    int (*open_input)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
    int (*find_stream_info)(AVFormatContext*, AVDictionary**);
    int (*find_best_stream)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**, int);
    int (*read_frame)(AVFormatContext*, AVPacket*);
    void (*close_input)(AVFormatContext**);
    // libswscale
    SwsContext* (*sws_get)(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat, int, SwsFilter*, SwsFilter*, const double*);
    int (*sws_scale_fn)(SwsContext*, const uint8_t* const[], const int[], int, int, uint8_t* const[], const int[]);
    void (*sws_free)(SwsContext*);
};

#define BMJ_STR2(x) #x
#define BMJ_STR(x) BMJ_STR2(x)

// ライブラリの名前の候補（実行ファイルの隣を先に探す）
std::vector<std::string> lib_names(const char* base, const char* major) {
    std::vector<std::string> names;
    const char* exe = SDL_GetBasePath();
    std::string dir = exe ? exe : "";
#if defined(_WIN32)
    std::string n = std::string(base) + "-" + major + ".dll";
    names = {dir + n, n};
#elif defined(__APPLE__)
    std::string n = std::string("lib") + base + "." + major + ".dylib";
    names = {dir + n, n, "/opt/homebrew/lib/" + n, "/usr/local/lib/" + n};
#else
    std::string n = std::string("lib") + base + ".so." + major;
    names = {dir + n, n};
#endif
    return names;
}

SDL_SharedObject* load_lib(const char* base, const char* major) {
    for (const std::string& n : lib_names(base, major))
        if (SDL_SharedObject* so = SDL_LoadObject(n.c_str())) return so;
    return nullptr;
}

const Api& api() {
    static Api a = [] {
        Api r{};
        SDL_SharedObject* util = load_lib("avutil", BMJ_STR(LIBAVUTIL_VERSION_MAJOR));
        SDL_SharedObject* codec = util ? load_lib("avcodec", BMJ_STR(LIBAVCODEC_VERSION_MAJOR)) : nullptr;
        SDL_SharedObject* format = codec ? load_lib("avformat", BMJ_STR(LIBAVFORMAT_VERSION_MAJOR)) : nullptr;
        SDL_SharedObject* sws = format ? load_lib("swscale", BMJ_STR(LIBSWSCALE_VERSION_MAJOR)) : nullptr;
        if (!sws) {
            std::printf("FFmpeg（%d 版）が見つからないので、動画の BGA は出さない\n", LIBAVCODEC_VERSION_MAJOR);
            return r;
        }
        bool all = true;
        auto get = [&](SDL_SharedObject* so, const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_LoadFunction(so, name));
            if (!fn) all = false, std::printf("FFmpeg の関数が無い: %s\n", name);
        };
        get(util, "av_frame_alloc", r.frame_alloc);
        get(util, "av_frame_free", r.frame_free);
        get(util, "av_log_set_level", r.log_set_level);
        get(codec, "av_packet_alloc", r.packet_alloc);
        get(codec, "av_packet_free", r.packet_free);
        get(codec, "av_packet_unref", r.packet_unref);
        get(codec, "avcodec_alloc_context3", r.codec_alloc);
        get(codec, "avcodec_parameters_to_context", r.params_to_ctx);
        get(codec, "avcodec_open2", r.codec_open);
        get(codec, "avcodec_send_packet", r.send_packet);
        get(codec, "avcodec_receive_frame", r.receive_frame);
        get(codec, "avcodec_free_context", r.codec_free);
        get(format, "avformat_open_input", r.open_input);
        get(format, "avformat_find_stream_info", r.find_stream_info);
        get(format, "av_find_best_stream", r.find_best_stream);
        get(format, "av_read_frame", r.read_frame);
        get(format, "avformat_close_input", r.close_input);
        get(sws, "sws_getContext", r.sws_get);
        get(sws, "sws_scale", r.sws_scale_fn);
        get(sws, "sws_freeContext", r.sws_free);
        r.ok = all;
        if (all) {
            r.log_set_level(AV_LOG_QUIET);
            std::printf("FFmpeg を読み込んだ（動画の BGA を出す）\n");
        }
        return r;
    }();
    return a;
}

constexpr size_t kQueueMax = 6;  // 先に用意しておくフレーム数

}  // namespace

struct VideoDecoder::Ctx {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1;
    double time_base = 0;  // 秒
    double start = 0;      // 頭の時刻（秒）
};

bool VideoDecoder::available() { return api().ok; }

VideoDecoder::~VideoDecoder() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    close();
}

void VideoDecoder::close() {
    if (!ctx_) return;
    const Api& a = api();
    if (ctx_->sws) a.sws_free(ctx_->sws);
    if (ctx_->frame) a.frame_free(&ctx_->frame);
    if (ctx_->pkt) a.packet_free(&ctx_->pkt);
    if (ctx_->codec) a.codec_free(&ctx_->codec);
    if (ctx_->fmt) a.close_input(&ctx_->fmt);
    delete ctx_;
    ctx_ = nullptr;
}

bool VideoDecoder::open(const std::string& path) {
    const Api& a = api();
    if (!a.ok) return false;
    ctx_ = new Ctx;
    if (a.open_input(&ctx_->fmt, path.c_str(), nullptr, nullptr) < 0 || a.find_stream_info(ctx_->fmt, nullptr) < 0) {
        close();
        return false;
    }
    const AVCodec* dec = nullptr;
    ctx_->stream = a.find_best_stream(ctx_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ctx_->stream < 0 || !dec) {
        close();
        return false;
    }
    AVStream* st = ctx_->fmt->streams[ctx_->stream];
    ctx_->codec = a.codec_alloc(dec);
    if (!ctx_->codec || a.params_to_ctx(ctx_->codec, st->codecpar) < 0 || a.codec_open(ctx_->codec, dec, nullptr) < 0) {
        close();
        return false;
    }
    ctx_->time_base = static_cast<double>(st->time_base.num) / st->time_base.den;
    ctx_->start = st->start_time != AV_NOPTS_VALUE ? st->start_time * ctx_->time_base : 0;
    w_ = ctx_->codec->width, h_ = ctx_->codec->height;
    if (w_ <= 0 || h_ <= 0) {
        close();
        return false;
    }
    ctx_->sws = a.sws_get(w_, h_, ctx_->codec->pix_fmt, w_, h_, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    ctx_->pkt = a.packet_alloc();
    ctx_->frame = a.frame_alloc();
    if (!ctx_->sws || !ctx_->pkt || !ctx_->frame) {
        close();
        return false;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void VideoDecoder::run() {
    const Api& a = api();
    bool flushing = false;
    auto push_frames = [&]() -> bool {
        // デコーダから出てくるフレームを全部 RGBA にして積む。止めるよう言われたら false
        while (a.receive_frame(ctx_->codec, ctx_->frame) >= 0) {
            Frame f;
            int64_t pts = ctx_->frame->best_effort_timestamp;
            f.t_ms = pts == AV_NOPTS_VALUE ? 0 : (pts * ctx_->time_base - ctx_->start) * 1000.0;
            f.w = w_, f.h = h_;
            f.rgba.resize(static_cast<size_t>(w_) * h_ * 4);
            uint8_t* dst[4] = {f.rgba.data(), nullptr, nullptr, nullptr};
            int lines[4] = {w_ * 4, 0, 0, 0};
            a.sws_scale_fn(ctx_->sws, ctx_->frame->data, ctx_->frame->linesize, 0, h_, dst, lines);
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || queue_.size() < kQueueMax; });
            if (stop_) return false;
            queue_.push_back(std::move(f));
        }
        return true;
    };
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
        }
        if (!flushing) {
            int r = a.read_frame(ctx_->fmt, ctx_->pkt);
            if (r < 0) {
                // 終わり：デコーダに残っているフレームを出し切る
                flushing = true;
                a.send_packet(ctx_->codec, nullptr);
            } else {
                if (ctx_->pkt->stream_index == ctx_->stream) a.send_packet(ctx_->codec, ctx_->pkt);
                a.packet_unref(ctx_->pkt);
            }
        }
        if (!push_frames()) return;
        if (flushing) {
            std::lock_guard<std::mutex> lock(mu_);
            eof_ = true;
            return;
        }
    }
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

}  // namespace bmj
