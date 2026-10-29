// LR2（DxLib）と同じ乱数。#RANDOM とレーンのランダムが同じ乱数列を使うので、配置を再現するにはこれが要る。
// 参照: OpenLR2 dep/DxLib/DxUseCLib.cpp:4219（srandMT）, dep/DxLib/DxSystem.cpp:1200（GetRand）
#pragma once

#include <array>
#include <cstdint>

namespace rd {

class Lr2Random {
public:
    explicit Lr2Random(uint32_t seed = 0) { seed_mt(seed); }

    // 1998年版MTの初期化（69069の線形合同法で状態を埋める）
    void seed_mt(uint32_t seed) {
        for (int i = 0; i < kN; ++i) {
            mt_[i] = seed & 0xffff0000u;
            seed = 69069u * seed + 1u;
            mt_[i] |= (seed & 0xffff0000u) >> 16;
            seed = 69069u * seed + 1u;
        }
        mti_ = kN;
    }

    uint32_t next_u32() {
        if (mti_ >= kN) generate();
        uint32_t y = mt_[mti_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }

    // DxLib の GetRand(rand_max)：0..rand_max を返す。rand_max が負でも乱数を 1 つ使う（#RANDOM 0 など）。
    // 計算は符号付き 64 ビットのまま（rand_max = -1 なら 0、それより小さければ負の値になる）
    int get_rand(int rand_max) {
        int64_t v = static_cast<int64_t>(next_u32()) * (static_cast<int64_t>(rand_max) + 1);
        return static_cast<int>(v >> 32);
    }

private:
    static constexpr int kN = 624, kM = 397;
    void generate() {
        static const uint32_t mag01[2] = {0u, 0x9908b0dfu};
        int k = 0;
        uint32_t y;
        for (; k < kN - kM; ++k) {
            y = (mt_[k] & 0x80000000u) | (mt_[k + 1] & 0x7fffffffu);
            mt_[k] = mt_[k + kM] ^ (y >> 1) ^ mag01[y & 1u];
        }
        for (; k < kN - 1; ++k) {
            y = (mt_[k] & 0x80000000u) | (mt_[k + 1] & 0x7fffffffu);
            mt_[k] = mt_[k + (kM - kN)] ^ (y >> 1) ^ mag01[y & 1u];
        }
        y = (mt_[kN - 1] & 0x80000000u) | (mt_[0] & 0x7fffffffu);
        mt_[kN - 1] = mt_[kM - 1] ^ (y >> 1) ^ mag01[y & 1u];
        mti_ = 0;
    }

    std::array<uint32_t, kN> mt_{};
    int mti_ = kN;
};

}  // namespace rd
