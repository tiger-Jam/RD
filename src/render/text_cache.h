// TTFの文字列を描く（stb_truetype）。文字列ごとに1枚のテクスチャにしてキャッシュする
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmj {

class TextCache {
public:
    explicit TextCache(SDL_Renderer* r) : r_(r) {}
    ~TextCache() { clear(); }
    struct Tex {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;  // 文字列の幅と高さ（ピクセル）
    };
    // size は文字の高さ（ピクセル）、thick は DxLib の太さ（0〜9）
    Tex get(int size, int thick, const std::string& s);
    // 前に呼んでからこれまでに新しく作った文字列の数と、作るのにかかった時間（ms）。遅いフレームの内訳に使う
    static void take_made(int& count, double& ms);
    // align: 0=左, 1=中央, 2=右。max_w を超えるときは横に縮める（LR2の文字と同じ）
    float draw(float x, float y, int size, SDL_Color c, const std::string& s, int align = 0, float max_w = 0, int thick = 4);
    void clear();
    // 使うフォントファイルを指定する（空ならOSの日本語フォントを探す）
    void set_font_path(const std::string& p, int face = 0) { font_path_ = p, font_face_ = face; }
    // false にすると、ぼかさない文字にする（DxLib の既定の文字と同じ。小さい文字を画素にそのまま描くとき）
    void set_antialias(bool on) { antialias_ = on; }
    // Windows だけ：OS の文字描画（GDI）でこの書体を使う（例 "MS UI Gothic"）。小さい文字はドット絵の字形になり、
    // Windows のダイアログや DxLib と同じ見た目になる。ぼかさない（set_antialias(false) と同じ）
    void set_system_font(const std::string& face) { system_font_ = face; }

private:
    const std::vector<uint8_t>* font_data(int thick);
    SDL_Renderer* r_;
    std::map<std::string, Tex> cache_;
    std::map<std::string, uint64_t> used_;  // 最後に使った順番（多すぎたら古いものから捨てる）
    uint64_t tick_ = 0;
    void evict();
    std::map<std::string, std::vector<uint8_t>> files_;
    std::string font_path_;
    int font_face_ = 0;  // .ttc の中の何番目の書体か（font_path_ を指定したときだけ）
    bool antialias_ = true;
    std::string system_font_;
    Tex get_system(int size, const std::string& s);
};

}  // namespace bmj
