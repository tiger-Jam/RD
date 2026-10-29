#include "game/ghost.h"

#include <cctype>
#include <cmath>
#include <utility>

namespace bmj {

namespace {

// 判定の文字：'@'=空POOR 'A'=POOR … 'E'=PGREAT。後ろに続く数字は繰り返しの数（1 は書かない）
char judge_char(int8_t j) { return j >= 0 && j <= 5 ? static_cast<char>('@' + j) : '-'; }

// よく出る「判定＋繰り返し」の 2 文字を 1 文字に縮める表（LR2 のゴーストの文字列と同じ置き換え）
struct Pair {
    char from[3];
    char to;
};
constexpr Pair kFirstPass[] = {
    {"E1", 'F'}, {"E2", 'G'}, {"E3", 'H'}, {"E4", 'I'}, {"E5", 'J'}, {"E6", 'K'}, {"E7", 'L'}, {"E8", 'M'}, {"E9", 'N'},
    {"EC", 'P'}, {"EB", 'Q'}, {"EA", 'R'}, {"D2", 'S'}, {"D3", 'T'}, {"D4", 'U'}, {"D5", 'V'}, {"D6", 'W'}, {"DE", 'X'},
    {"DC", 'Y'}, {"DB", 'a'}, {"DA", 'b'}, {"C2", 'c'}, {"C3", 'd'}, {"C4", 'e'}, {"C5", 'f'}, {"CE", 'g'}, {"CD", 'h'},
    {"CB", 'i'}, {"CA", 'j'}, {"AB", 'k'}, {"AC", 'l'}, {"AD", 'm'}, {"AE", 'n'}, {"A2", 'o'}, {"A3", 'p'},
};
// 1 回目で縮めた 'X'（DE）の後ろを、さらに縮める
constexpr Pair kSecondPass[] = {
    {"XX", 'q'}, {"X1", 'r'}, {"X2", 's'}, {"X3", 't'}, {"X4", 'u'}, {"X5", 'v'}, {"X6", 'w'}, {"X7", 'x'}, {"X8", 'y'}, {"X9", 'z'},
};

template <size_t N>
std::string compress(const std::string& s, const Pair (&table)[N]) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        bool hit = false;
        if (i + 1 < s.size())
            for (const Pair& p : table)
                if (s[i] == p.from[0] && s[i + 1] == p.from[1]) {
                    out += p.to, ++i, hit = true;
                    break;
                }
        if (!hit) out += s[i];
    }
    return out;
}

template <size_t N>
std::string expand(const std::string& s, const Pair (&table)[N]) {
    std::string out;
    for (char c : s) {
        bool hit = false;
        for (const Pair& p : table)
            if (c == p.to) {
                out += p.from, hit = true;
                break;
            }
        if (!hit) out += c;
    }
    return out;
}

}  // namespace

std::string encode_ghost(const std::vector<int8_t>& judges) {
    std::string rle;
    for (size_t i = 0; i < judges.size();) {
        size_t j = i;
        while (j < judges.size() && judges[j] == judges[i]) ++j;
        rle += judge_char(judges[i]);
        if (j - i >= 2) rle += std::to_string(j - i);
        i = j;
    }
    if (rle.empty()) return {};
    // 終わりの印（LR2 と同じく "Z" を付ける。読むときは無視される）
    return compress(compress(rle, kFirstPass), kSecondPass) + "Z";
}

std::vector<int8_t> decode_ghost(const std::string& data) {
    std::string s = expand(expand(data, kSecondPass), kFirstPass);
    std::vector<int8_t> out;
    int8_t cur = -1;
    int rep = -1;  // -1 = まだ判定が無い 0 = 数字なし（1 回）
    auto flush = [&] {
        if (cur < 0) return;
        for (int k = 0; k < std::max(1, rep); ++k) out.push_back(cur);
    };
    for (char c : s) {
        if (c >= '@' && c <= 'E') {
            flush();
            cur = static_cast<int8_t>(c - '@');
            rep = 0;
        } else if (c >= '0' && c <= '9') {
            if (cur >= 0) rep = rep * 10 + (c - '0');
        }
    }
    flush();
    return out;
}

void Ghost::set_default(int target, int notes) {
    *this = Ghost{};
    total_notes_ = notes;
    int pg;
    if (target == 88) pg = static_cast<int>(notes * 7.0 / 9.0), name_ = "RANK AAA";
    else if (target == 77) pg = static_cast<int>(notes * 5.0 / 9.0), name_ = "RANK AA";
    else if (target == 66) pg = static_cast<int>(notes * 3.0 / 9.0), name_ = "RANK A";
    else if (target >= 50 && target <= 100) {
        // 割合 target% の EX スコアになる PGREAT の数（残りは GREAT）。PGREAT の割合は 2 × target − 100 %
        pg = static_cast<int>((2.0 * target - 100.0) * notes) / 100;
        name_ = std::to_string(target) + "%";
    } else {
        name_ = "NO TARGET";
        return;
    }
    expect_[5] = pg;
    expect_[4] = notes - pg;
}

void Ghost::set_from_exscore(int exscore, int notes, std::string name) {
    *this = Ghost{};
    total_notes_ = notes;
    name_ = std::move(name);
    if (notes <= 0) return;
    expect_[5] = static_cast<int>(std::ceil((static_cast<double>(exscore) / notes - 1.0) * notes));
    expect_[4] = notes - expect_[5];
}

void Ghost::set_from_judges(std::vector<int8_t> judges, std::string name) {
    *this = Ghost{};
    name_ = std::move(name);
    for (int8_t j : judges) {
        if (j == 0) expect_[1]++;  // 空POOR は POOR に数え、ノーツには数えない
        else if (j > 0 && j < 6) expect_[j]++, total_notes_++;
    }
    queue_ = std::move(judges);
}

void Ghost::deal() {
    if (total_notes_ < 1) return;
    auto update_ex = [&] { ex_ = judge_[5] * 2 + judge_[4]; };
    if (queue_.empty()) {
        // 判定の列が無い：何ノーツ目かに比例して、最後の判定の数を割り振る（1 ノーツ目では、まだ何も数えない）
        if (now_note_ > 0) {
            const int reached = now_note_ + 1;
            for (int i = 0; i < 6; ++i) judge_[i] = reached * expect_[i] / total_notes_;
        }
        update_ex();
        ++now_note_;
        return;
    }
    // 判定の列がある：空POOR（0）はノーツを進めずに POOR に足して読み進め、ノーツ 1 つ分の判定を読んだら止まる。列が尽きたら何もしない
    for (; read_ < queue_.size(); ++read_) {
        const int8_t j = queue_[read_];
        if (j == 0) {
            judge_[1]++;
            update_ex();
            continue;
        }
        ++read_;
        if (j > 0 && j < 6) {
            judge_[j]++;
            ++now_note_;
            update_ex();
        }
        return;
    }
}

}  // namespace bmj
