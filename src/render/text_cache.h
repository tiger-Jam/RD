// TTFの文字列を描く（stb_truetype）。文字列ごとに1枚のテクスチャにしてキャッシュする
#pragma once

#include <SDL3/SDL.h>

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
    // align: 0=左, 1=中央, 2=右。max_w を超えるときは横に縮める（LR2の文字と同じ）
    float draw(float x, float y, int size, SDL_Color c, const std::string& s, int align = 0, float max_w = 0, int thick = 4);
    void clear();
    void set_font_dir(const std::string& d) { font_dir_ = d; }

private:
    const std::vector<uint8_t>* font_data(int thick);
    SDL_Renderer* r_;
    std::map<std::string, Tex> cache_;
    std::map<std::string, std::vector<uint8_t>> files_;
    std::string font_dir_ = "/System/Library/Fonts";
};

}  // namespace bmj
