// ゴースト（自己ベストやターゲットの、その時点でのEXスコア）。OpenLR2の PLAYSCORE（LR2/LR2_ghost.cpp）の移植。
// 今はスコアの保存が無いので、自己ベストは常に空で、ターゲットは既定のゴースト（割合指定）だけ
#pragma once

#include <array>

namespace bmj {

class Ghost {
public:
    // LR2: SetDefaultGhost。88/77/66 は AAA/AA/A、50〜100 はその割合、それ以外は「ターゲット無し」
    void set_default(int target, int notes) {
        *this = Ghost{};
        total_notes_ = notes;
        int pg;
        if (target == 88) pg = static_cast<int>(notes * 7.0 / 9.0);
        else if (target == 77) pg = static_cast<int>(notes * 5.0 / 9.0);
        else if (target == 66) pg = static_cast<int>(notes * 3.0 / 9.0);
        else if (target >= 50 && target <= 100) pg = static_cast<int>(((target + target) - 100.0) * notes) / 100;
        else return;
        expect_[5] = pg;
        expect_[4] = notes - pg;
    }
    // LR2: SetGhost。記録されたEXスコアから、PGREATとGREATだけでできたゴーストを作る
    void set_from_exscore(int exscore, int notes) {
        *this = Ghost{};
        total_notes_ = notes;
        if (notes <= 0) return;
        double v = (static_cast<double>(exscore) / notes - 1.0) * notes;
        int pg = static_cast<int>(v);
        if (pg < v) pg++;  // LR2: RoundUp
        expect_[5] = pg;
        expect_[4] = notes - pg;
    }
    // LR2: DealJudgeFromQueue（記録されたゴーストが無いとき）。自分のノーツが1つ判定されるたびに呼ぶ
    void deal() {
        if (total_notes_ < 1) return;
        if (now_note_ > 0)
            for (int i = 0; i < 6; ++i) judge_[i] = ((now_note_ + 1) * expect_[i]) / total_notes_;
        rate_ = judge_[4] + judge_[5] * 2;
        now_note_++;
    }
    int exscore() const { return rate_; }  // LR2の PLAYSCORE::rate（その時点のEXスコア）
    int judge(int j) const { return judge_[j]; }
    int now_note() const { return now_note_; }
    int total_notes() const { return total_notes_; }

private:
    int total_notes_ = 0, now_note_ = 0, rate_ = 0;
    std::array<int, 6> expect_{}, judge_{};
};

}  // namespace bmj
