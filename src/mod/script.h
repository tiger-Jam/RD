// MOD のスクリプト（Lua。仕様書14章「MODの形」）。
// MOD ごとに別の Lua の環境を作り、本体が用意した窓口（関数の名前）だけを呼ぶ。ゲームの状態は写しを渡すだけで、書き換えられない。
//   on_play_start(info)   曲が始まる前（譜面の情報）
//   on_judge(e)           判定が出たとき・地雷を踏んだとき
//   on_play_end(result)   リザルトに入るとき（結果と全部のゲージ）
//   on_draw(state)        プレー中、毎フレーム（「見た目」を宣言した MOD だけ。bmj.draw_rect / bmj.draw_text で描く）
//   build_folders(charts) 選曲の一覧を作るとき（「操作」を宣言した MOD だけ。フォルダの表を返す。譜面は sha256 か { sha256, label }）
//   folder_bars(folder)   フォルダを開いたとき（「操作」を宣言した MOD だけ。フォルダの名前の下に出すバーを返す）
//   pick_chart(req)       folder_bars のバーを決定したとき（遊ぶ譜面の sha256 を返す）
// 使えるのは Lua の base（ファイルを読む関数を除く）・table・string・math・utf8・coroutine と bmj の表だけ。
// ファイルに書けるのは BMjoyfiles/Player/<ID>/ModData/<MOD の id>/ の下だけ
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/song_db.h"
#include "mod/mod.h"

namespace bmj {

// スクリプトが描いたもの（スキンの座標。プレー画面のスキンの上に重ねる）
struct ModDrawCmd {
    enum class Kind { Rect, Text } kind = Kind::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t rgb = 0xffffff;
    uint8_t alpha = 255;
    std::string text;
    int size = 16, align = 0;  // align：0 左、1 中央、2 右
};

struct ModPlayInfo {
    std::string sha256, md5, path, title, subtitle, artist, genre;
    int playlevel = 0, notes = 0, total = 0, rank = 2, keys = 7, length_ms = 0;
    double bpm_min = 0, bpm_max = 0;
    std::string gauge;  // 遊ぶゲージ（"GROOVE" など、またはカスタムゲージの鍵）
    int random = 0;     // 配置のオプション（LR2 の番号）
    bool autoplay = false, replay = false, course = false;
    int view_w = 640, view_h = 480;
    // 判定の幅（ms。早い側・遅い側。この譜面の #RANK とカスタム判定を反映したもの）
    int pgreat[2] = {}, great[2] = {}, good[2] = {}, bad[2] = {};
};

struct ModJudgeInfo {
    double time_ms = 0;   // 曲の時刻
    int lane = 0;         // 0 皿、1〜7 鍵盤
    int judge = 0;        // 0 空POOR、1 POOR、2 BAD、3 GOOD、4 GREAT、5 PGREAT
    bool has_offset = false;
    int offset_ms = 0;    // 押した時刻 − ノーツの時刻（正が遅い）
    bool fast = false, mine = false, ln_end = false;
    int combo = 0, exscore = 0;
    double hp = 0;        // 遊んでいるゲージ
};

struct ModPlayResult {
    int clear = 0, exscore = 0, bp = 0, max_combo = 0, total_notes = 0;
    std::array<int, 6> judge{};  // 空POOR〜PGREAT の数（POOR には空POOR を含めない）
    int fast = 0, slow = 0;
    bool quit_early = false;
    std::vector<std::pair<std::string, double>> gauges;  // 裏で計算した全部のゲージ
};

struct ModDrawState {
    double now_ms = 0, song_ms = 0;
    int combo = 0, exscore = 0;
    double hp = 0;
    int view_w = 640, view_h = 480;
};

// フォルダ（build_folders の戻り値）。中身は譜面の sha256 か、下のフォルダ
struct ModFolder {
    std::string title;
    std::vector<std::string> charts;
    std::vector<std::string> labels;  // charts と同じ並び。曲名の前に出す文字（無ければ空）
    bool keep_order = false;          // 選曲の並べ替えをかけず、charts の順のまま出す
    std::vector<ModFolder> children;
};

class ModScript;

// 使っている MOD のスクリプトの全部（プレー 1 回ぶん。プレーの前に作り、終わったら捨てる）
class ModScripts {
public:
    ModScripts();
    ~ModScripts();
    // スクリプトを持つ MOD を読む。読めなかったものは errors に入り、使わない。
    // data_root は MOD が書いてよい場所の親（BMjoyfiles/Player/<ID>/ModData。MOD ごとに <id>/ を切る）
    void load(const std::vector<const ModManifest*>& mods, const std::string& data_root);
    bool empty() const { return scripts_.empty(); }
    void play_start(const ModPlayInfo& info);
    void judge(const ModJudgeInfo& e);
    void play_end(const ModPlayResult& r);
    // 毎フレーム描くもの（前のフレームの分は消える）
    const std::vector<ModDrawCmd>& draw(const ModDrawState& s);
    // 途中で起きた失敗（失敗した MOD はそのプレーのあいだ止める）
    std::vector<std::string> take_errors();

private:
    std::vector<std::unique_ptr<ModScript>> scripts_;
    std::vector<ModDrawCmd> draw_;
    std::vector<std::string> errors_;
};

// 譜面が入っている難易度表のレベル（build_folders の譜面の tables）
struct ModTableLevel {
    std::string table, symbol, level;
    int table_order = 0, level_order = 0;  // 表の並び・表の中のレベルの並び
};
// build_folders に渡す、譜面の情報のほかのもの（無ければ渡さない）
struct ModChartExtra {
    const std::unordered_map<std::string, ChartAttr>* attrs = nullptr;                   // sha256 → 傾向
    const std::unordered_map<std::string, std::vector<ModTableLevel>>* tables = nullptr;  // md5 → 難易度表のレベル
};

// 選曲の一覧を作るときに、「操作」を宣言した MOD の build_folders を呼んでフォルダを作る
std::vector<ModFolder> build_mod_folders(const std::vector<const ModManifest*>& mods, const std::vector<ChartRecord>& charts,
                                         const std::unordered_map<std::string, ScoreRecord>& scores,
                                         const std::string& data_root, std::vector<std::string>* errors = nullptr,
                                         const ModChartExtra& extra = {});

// フォルダの中の、フォルダの名前の下に出すバー（folder_bars の戻り値。ランダムセレクトなど）
struct ModBar {
    std::string mod_id, key, title;
    bool roulette = false;  // カーソルを置いている間、曲名の枠に一覧の曲名を次々に出す
};

// フォルダを開いたときに、「操作」を宣言した MOD の folder_bars を呼んでバーを集める
std::vector<ModBar> mod_folder_bars(const std::vector<const ModManifest*>& mods, const std::string& folder, int charts,
                                    const std::string& data_root, std::vector<std::string>* errors = nullptr);

// バーを決定したときに、その MOD の pick_chart を呼んで遊ぶ譜面（sha256）を決める。決めなければ空。
// shown はルーレットのバーで、決定したときに曲名の枠に出ていた譜面（sha256）
std::string mod_pick_chart(const ModManifest& m, const ModBar& bar, const std::string& folder,
                           const std::vector<const ChartRecord*>& charts,
                           const std::unordered_map<std::string, ScoreRecord>& scores, const std::string& data_root,
                           std::string* error = nullptr, const std::string& shown = {});

// MOD がファイルに書いてよい名前か（相対で、.. や絶対パスを含まない）
bool mod_file_name_ok(const std::string& name);

}  // namespace bmj
