#include "audio/sound_bank.h"

#include <algorithm>
#include <map>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "dr_mp3.h"
#include "dr_wav.h"
#include "miniaudio.h"

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
}

namespace bmj {

namespace {

enum class Format { Unknown, Wav, Ogg, Mp3 };

Format sniff(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Format::Unknown;
    unsigned char h[12] = {};
    size_t n = std::fread(h, 1, sizeof(h), f);
    std::fclose(f);
    if (n >= 4 && std::memcmp(h, "RIFF", 4) == 0) return Format::Wav;
    if (n >= 4 && std::memcmp(h, "OggS", 4) == 0) return Format::Ogg;
    if (n >= 3 && std::memcmp(h, "ID3", 3) == 0) return Format::Mp3;
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return Format::Mp3;
    return Format::Unknown;
}

// 任意のチャンネル数・サンプルレートのfloat PCMを、ステレオ・out_rate に変換する
bool convert(const float* in, uint64_t in_frames, uint32_t in_ch, uint32_t in_rate, uint32_t out_rate,
             Sample& out) {
    if (in_ch == 0 || in_rate == 0) return false;
    ma_data_converter_config cfg = ma_data_converter_config_init(ma_format_f32, ma_format_f32, in_ch, 2,
                                                                 in_rate, out_rate);
    cfg.resampling.algorithm = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = 4;
    if (in_ch == 1) cfg.channelMixMode = ma_channel_mix_mode_simple;
    ma_data_converter conv;
    if (ma_data_converter_init(&cfg, nullptr, &conv) != MA_SUCCESS) return false;
    ma_uint64 expect = 0;
    ma_data_converter_get_expected_output_frame_count(&conv, in_frames, &expect);
    out.pcm.assign(static_cast<size_t>(expect + 64) * 2, 0.0f);
    ma_uint64 frames_in = in_frames;
    ma_uint64 frames_out = expect + 64;
    ma_result r = ma_data_converter_process_pcm_frames(&conv, in, &frames_in, out.pcm.data(), &frames_out);
    ma_data_converter_uninit(&conv, nullptr);
    if (r != MA_SUCCESS) return false;
    out.pcm.resize(static_cast<size_t>(frames_out) * 2);
    return true;
}

}  // namespace

bool decode_sound_file(const std::string& path, uint32_t out_rate, Sample& out, std::string& error) {
    Format fmt = sniff(path);
    unsigned int ch = 0, rate = 0;
    drwav_uint64 frames = 0;
    float* data = nullptr;
    bool ok = false;
    switch (fmt) {
    case Format::Wav:
        data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
        if (data) {
            ok = convert(data, frames, ch, rate, out_rate, out);
            drwav_free(data, nullptr);
        }
        break;
    case Format::Mp3: {
        drmp3_config c{};
        drmp3_uint64 f = 0;
        data = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &c, &f, nullptr);
        if (data) {
            ok = convert(data, f, c.channels, c.sampleRate, out_rate, out);
            drmp3_free(data, nullptr);
        }
        break;
    }
    case Format::Ogg: {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(path.c_str(), &err, nullptr);
        if (v) {
            stb_vorbis_info info = stb_vorbis_get_info(v);
            unsigned int total = stb_vorbis_stream_length_in_samples(v);
            std::vector<float> buf(static_cast<size_t>(total) * info.channels + info.channels * 4096);
            size_t got = 0;
            for (;;) {
                if (buf.size() - got * info.channels < static_cast<size_t>(info.channels) * 4096)
                    buf.resize(buf.size() + static_cast<size_t>(info.channels) * 65536);
                int n = stb_vorbis_get_samples_float_interleaved(
                    v, info.channels, buf.data() + got * info.channels,
                    static_cast<int>(buf.size() - got * info.channels));
                if (n <= 0) break;
                got += static_cast<size_t>(n);
            }
            stb_vorbis_close(v);
            ok = convert(buf.data(), got, info.channels, info.sample_rate, out_rate, out);
        }
        break;
    }
    case Format::Unknown:
        error = "形式を判定できない: " + path;
        return false;
    }
    if (!ok) error = "デコードに失敗: " + path;
    return ok;
}

void SoundBank::load(const std::vector<std::string>& paths, uint32_t out_rate, int threads,
                     std::vector<std::string>* errors, std::atomic<int>* done,
                     const std::vector<std::pair<int64_t, int64_t>>* ranges) {
    samples_.assign(paths.size(), Sample{});
    // 同じファイルは 1 回だけデコードする
    std::map<std::string, size_t> first_of;
    std::vector<size_t> unique;
    for (size_t i = 0; i < paths.size(); ++i)
        if (!paths[i].empty() && first_of.try_emplace(paths[i], unique.size()).second) unique.push_back(i);
    std::vector<Sample> decoded(unique.size());
    std::atomic<size_t> next{0};
    std::mutex err_mu;
    auto worker = [&] {
        for (;;) {
            size_t k = next.fetch_add(1);
            if (k >= unique.size()) return;
            std::string err;
            if (!decode_sound_file(paths[unique[k]], out_rate, decoded[k], err) && errors) {
                std::lock_guard<std::mutex> lock(err_mu);
                errors->push_back(err);
            }
        }
    };
    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    // 番号ごとに、ファイル全体か切り出した範囲を割り当てる
    std::vector<int> uses(unique.size(), 0);
    for (size_t i = 0; i < paths.size(); ++i)
        if (!paths[i].empty()) uses[first_of[paths[i]]]++;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].empty()) continue;
        size_t k = first_of[paths[i]];
        Sample& src = decoded[k];
        int64_t start = 0, end = -1;
        if (ranges && i < ranges->size()) start = (*ranges)[i].first, end = (*ranges)[i].second;
        if (start <= 0 && end < 0) {
            // ファイル全体：最後の 1 つは移し、ほかは写す
            if (--uses[k] == 0) samples_[i] = std::move(src);
            else samples_[i] = src;
        } else {
            --uses[k];
            int64_t f0 = std::clamp<int64_t>(start * out_rate / 1000000, 0, src.frames());
            int64_t f1 = end < 0 ? src.frames() : std::clamp<int64_t>(end * out_rate / 1000000, f0, src.frames());
            samples_[i].pcm.assign(src.pcm.begin() + f0 * 2, src.pcm.begin() + f1 * 2);
        }
        if (done) done->fetch_add(1);
    }
}

}  // namespace bmj
