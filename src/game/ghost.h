// ゴースト（自己ベストやターゲットの、その時点での判定の数とEXスコア）。LR2 の PLAYSCORE（LR2/LR2_ghost.cpp）と同じ動き。
// 記録した判定の列があれば 1 ノーツずつ再生し、無ければ最後の判定の数から割り振る
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

// 判定の列を LR2 のゴーストの文字列にする（LR2: score.ghost）。値は 0=空POOR 1=POOR … 5=PGREAT
std::string encode_ghost(const std::vector<int8_t>& judges);
// LR2 のゴーストの文字列を判定の列に戻す。読めない文字は飛ばす
std::vector<int8_t> decode_ghost(const std::string& data);

class Ghost {
public:
    // LR2: SetDefaultGhost。88/77/66 は AAA/AA/A、50〜100 はその割合、それ以外は「ターゲット無し」
    void set_default(int target, int notes);
    // LR2: SetGhost。EXスコアから、PGREATとGREATだけでできたゴーストを作る
    void set_from_exscore(int exscore, int notes, std::string name = {});
    // LR2: DecodeGhostData。記録した判定の列から作る（総ノーツは列の中のノーツの数）
    void set_from_judges(std::vector<int8_t> judges, std::string name = {});
    // LR2: DealJudgeFromQueue。自分のノーツが1つ判定されるたびに呼ぶ。
    // 列があれば、空POOR を読み飛ばしながら次のノーツの判定を 1 つ進める
    void deal();
    bool has_queue() const { return !queue_.empty(); }
    int exscore() const { return ex_; }  // その時点の EX スコア（LR2 の PLAYSCORE::rate）
    int final_exscore() const { return expect_[4] + expect_[5] * 2; }  // LR2の PLAYSCORE::exscore
    int judge(int j) const { return judge_[j]; }
    int expect(int j) const { return expect_[j]; }
    int now_note() const { return now_note_; }
    int total_notes() const { return total_notes_; }
    const std::string& name() const { return name_; }

private:
    int total_notes_ = 0, now_note_ = 0, ex_ = 0;
    std::array<int, 6> expect_{}, judge_{};
    std::vector<int8_t> queue_;
    size_t read_ = 0;
    std::string name_;
};

}  // namespace bmj
