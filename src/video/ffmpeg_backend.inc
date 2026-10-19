// FFmpeg を使う動画のデコード。BMJ_FF_NS（ff9 など）を決めてから読み込み、その版のヘッダーでビルドする。
// ライブラリはヘッダーと同じ版（メジャー番号）のものを実行時に読み込む
#include <SDL3/SDL.h>

#include <cstdio>
#include <type_traits>

#include "video/video_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace bmj {
namespace BMJ_FF_NS {

namespace {

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
    void (*flush_buffers)(AVCodecContext*);
    // libavformat
    int (*open_input)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
    int (*find_stream_info)(AVFormatContext*, AVDictionary**);
    int (*find_best_stream)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**, int);
    int (*read_frame)(AVFormatContext*, AVPacket*);
    void (*close_input)(AVFormatContext**);
    int (*seek_frame)(AVFormatContext*, int, int64_t, int);
    // libswscale
    SwsContext* (*sws_get)(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat, int, SwsFilter*, SwsFilter*, const double*);
    int (*sws_scale_fn)(SwsContext*, const uint8_t* const[], const int[], int, int, uint8_t* const[], const int[]);
    void (*sws_free)(SwsContext*);
};

#define BMJ_STR2(x) #x
#define BMJ_STR(x) BMJ_STR2(x)

// ライブラリの名前の候補（実行ファイルの隣を先に探す）
SDL_SharedObject* load_lib(const char* base, const char* major) {
    const char* exe = SDL_GetBasePath();
    std::string dir = exe ? exe : "";
#if defined(_WIN32)
    std::string n = std::string(base) + "-" + major + ".dll";
    const std::string names[] = {dir + n, n};
#elif defined(__APPLE__)
    std::string n = std::string("lib") + base + "." + major + ".dylib";
    const std::string names[] = {dir + n, n, "/opt/homebrew/lib/" + n, "/usr/local/lib/" + n};
#else
    std::string n = std::string("lib") + base + ".so." + major;
    const std::string names[] = {dir + n, n};
#endif
    for (const std::string& name : names)
        if (SDL_SharedObject* so = SDL_LoadObject(name.c_str())) return so;
    return nullptr;
}

const Api& api() {
    static Api a = [] {
        Api r{};
        SDL_SharedObject* util = load_lib("avutil", BMJ_STR(LIBAVUTIL_VERSION_MAJOR));
        SDL_SharedObject* codec = util ? load_lib("avcodec", BMJ_STR(LIBAVCODEC_VERSION_MAJOR)) : nullptr;
        SDL_SharedObject* format = codec ? load_lib("avformat", BMJ_STR(LIBAVFORMAT_VERSION_MAJOR)) : nullptr;
        SDL_SharedObject* sws = format ? load_lib("swscale", BMJ_STR(LIBSWSCALE_VERSION_MAJOR)) : nullptr;
        if (!sws) return r;
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
        get(codec, "avcodec_flush_buffers", r.flush_buffers);
        get(format, "avformat_open_input", r.open_input);
        get(format, "avformat_find_stream_info", r.find_stream_info);
        get(format, "av_find_best_stream", r.find_best_stream);
        get(format, "av_read_frame", r.read_frame);
        get(format, "avformat_close_input", r.close_input);
        get(format, "av_seek_frame", r.seek_frame);
        get(sws, "sws_getContext", r.sws_get);
        get(sws, "sws_scale", r.sws_scale_fn);
        get(sws, "sws_freeContext", r.sws_free);
        r.ok = all;
        if (all) {
            r.log_set_level(AV_LOG_QUIET);
            std::printf("FFmpeg（avcodec %d）を読み込んだ（動画の BGA を出す）\n", LIBAVCODEC_VERSION_MAJOR);
        }
        return r;
    }();
    return a;
}

class Backend : public VideoBackend {
public:
    ~Backend() override {
        const Api& a = api();
        if (sws_) a.sws_free(sws_);
        if (frame_) a.frame_free(&frame_);
        if (pkt_) a.packet_free(&pkt_);
        if (codec_) a.codec_free(&codec_);
        if (fmt_) a.close_input(&fmt_);
    }

    bool open(const std::string& path) override {
        const Api& a = api();
        if (a.open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0 || a.find_stream_info(fmt_, nullptr) < 0) return false;
        const AVCodec* dec = nullptr;
        stream_ = a.find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (stream_ < 0 || !dec) return false;
        AVStream* st = fmt_->streams[stream_];
        codec_ = a.codec_alloc(dec);
        if (!codec_ || a.params_to_ctx(codec_, st->codecpar) < 0 || a.codec_open(codec_, dec, nullptr) < 0) return false;
        time_base_ = static_cast<double>(st->time_base.num) / st->time_base.den;
        start_ = st->start_time != AV_NOPTS_VALUE ? st->start_time * time_base_ : 0;
        w_ = codec_->width, h_ = codec_->height;
        if (w_ <= 0 || h_ <= 0) return false;
        sws_ = a.sws_get(w_, h_, codec_->pix_fmt, w_, h_, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        pkt_ = a.packet_alloc();
        frame_ = a.frame_alloc();
        return sws_ && pkt_ && frame_;
    }
    int width() const override { return w_; }
    int height() const override { return h_; }

    bool next(VideoFrame& f) override {
        const Api& a = api();
        while (true) {
            if (a.receive_frame(codec_, frame_) >= 0) {
                int64_t pts = frame_->best_effort_timestamp;
                f.t_ms = pts == AV_NOPTS_VALUE ? 0 : (pts * time_base_ - start_) * 1000.0;
                f.w = w_, f.h = h_;
                f.rgba.resize(static_cast<size_t>(w_) * h_ * 4);
                uint8_t* dst[4] = {f.rgba.data(), nullptr, nullptr, nullptr};
                int lines[4] = {w_ * 4, 0, 0, 0};
                a.sws_scale_fn(sws_, frame_->data, frame_->linesize, 0, h_, dst, lines);
                return true;
            }
            if (flushing_) return false;
            if (a.read_frame(fmt_, pkt_) < 0) {
                // 終わり：デコーダに残っているフレームを出し切る
                flushing_ = true;
                a.send_packet(codec_, nullptr);
                continue;
            }
            if (pkt_->stream_index == stream_) a.send_packet(codec_, pkt_);
            a.packet_unref(pkt_);
        }
    }

    void rewind() override {
        const Api& a = api();
        a.seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD);
        a.flush_buffers(codec_);
        flushing_ = false;
    }

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1, w_ = 0, h_ = 0;
    double time_base_ = 0, start_ = 0;  // 秒
    bool flushing_ = false;
};

}  // namespace

bool load() { return api().ok; }

std::unique_ptr<VideoBackend> create() { return std::make_unique<Backend>(); }

}  // namespace BMJ_FF_NS
}  // namespace bmj
