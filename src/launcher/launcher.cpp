// 起動前の設定アプリ（LR2 の LR2.exe「LR2 SETUP」に相当）。見た目とタブの並びは LR2 SETUP と同じにしてある
// （LR2beta3 付属マニュアル help/8.html の画像）。BMjoy で使えない項目は同じ場所に灰色で置き、BMjoy だけの項目は
// 最後の「BMJOY」タブにまとめる。「BMjoyの起動」で本体（BMjoybody）を立ち上げて、自分は閉じる
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/lr2_import.h"
#include "db/song_db.h"
#include "mod/mod.h"
#include "render/text_cache.h"
#include "util/dev.h"

namespace fs = std::filesystem;
using namespace bmj;

namespace {

constexpr int kW = 612, kH = 462;

// Windows の「クラシック」の部品の色（LR2 SETUP は見た目の設定を使わない昔ながらの部品）
const SDL_Color kFace{240, 240, 240, 255};     // ダイアログの地
const SDL_Color kText{0, 0, 0, 255};
const SDL_Color kGray{160, 160, 160, 255};     // 使えない項目の文字
const SDL_Color kHi{255, 255, 255, 255};       // 立体の明るい側
const SDL_Color kLight{227, 227, 227, 255};
const SDL_Color kShadow{160, 160, 160, 255};   // 立体の暗い側
const SDL_Color kDark{105, 105, 105, 255};
const SDL_Color kSel{0, 120, 215, 255};        // 選んでいる行

// ファイルやフォルダを選ぶ画面の結果（別のスレッドから呼ばれることがある）
struct DialogResult {
    std::mutex mu;
    std::function<void(const std::string&)> apply;
    std::string path;
    bool ready = false;
};
DialogResult g_dialog;

void dialog_cb(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_dialog.mu);
    if (files && files[0]) {
        g_dialog.path = files[0];
        g_dialog.ready = true;
    }
}

void pick_folder(SDL_Window* w, std::function<void(const std::string&)> apply) {
    {
        std::lock_guard<std::mutex> lock(g_dialog.mu);
        g_dialog.apply = std::move(apply);
        g_dialog.ready = false;
    }
    SDL_ShowOpenFolderDialog(dialog_cb, nullptr, w, nullptr, false);
}

void pick_skin(SDL_Window* w, std::function<void(const std::string&)> apply) {
    {
        std::lock_guard<std::mutex> lock(g_dialog.mu);
        g_dialog.apply = std::move(apply);
        g_dialog.ready = false;
    }
    static const SDL_DialogFileFilter filters[] = {{"スキン", "lr2skin"}};
    SDL_ShowOpenFileDialog(dialog_cb, nullptr, w, filters, 1, nullptr, false);
}

// 本体を立ち上げる
bool launch_body(std::string& err) {
    const char* base = SDL_GetBasePath();
    std::string dir = base ? base : "";
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        w.resize(n > 0 ? n - 1 : 0);
        return w;
    };
    std::wstring exe = wide(dir + "BMjoybody.exe");
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, wide(dir).c_str(), &si, &pi)) {
        err = "BMjoybody.exe を起動できない（" + std::to_string(GetLastError()) + "）";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::string exe = dir + "BMjoybody";
    if (!fs::exists(exe)) {
        err = "BMjoybody が見つからない: " + exe;
        return false;
    }
    std::string cmd = "'" + exe + "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        err = "本体を起動できない: " + cmd;
        return false;
    }
    return true;
#endif
}


// 1フレームごとに部品を並べて描く（押されたかどうかはその場で返す）
struct Gui {
    SDL_Renderer* r = nullptr;
    SDL_Window* win = nullptr;
    TextCache* text = nullptr;
    int font = 12;
    float mx = -1, my = -1;
    bool pressed = false;  // このフレームで左ボタンを押した
    float wheel = 0;
    // 入力欄
    const void* focus = nullptr;
    std::string buf;
    std::function<void(const std::string&)> commit;
    std::string typed;     // このフレームに打った文字
    bool key_enter = false, key_back = false, key_esc = false;
    // 開いているコンボボックス
    const void* combo_open = nullptr;
    SDL_FRect combo_rect{};
    std::vector<std::string> combo_items;
    std::function<void(int)> combo_pick;

    bool in(SDL_FRect rc) const { return mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h; }
    void fill(SDL_FRect rc, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(r, &rc);
    }
    void frame(SDL_FRect rc, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_RenderRect(r, &rc);
    }
    void line(float x1, float y1, float x2, float y2, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        SDL_RenderLine(r, x1, y1, x2, y2);
    }
    float label(float x, float y, const std::string& s, bool enabled = true, int align = 0) {
        return text->draw(x, y, font, enabled ? kText : kGray, s, align, 0, 0);
    }
    // 押したら入力欄の編集を確定する
    void unfocus() {
        if (focus && commit) commit(buf);
        focus = nullptr;
        commit = nullptr;
        SDL_StopTextInput(win);
    }
    bool clicked(SDL_FRect rc) {
        if (!pressed || combo_open || !in(rc)) return false;
        unfocus();
        return true;
    }
    // 浮き出た枠（ボタン・タブの中）と凹んだ枠（入力欄・チェックボックス）
    void raised(SDL_FRect rc) {
        line(rc.x, rc.y, rc.x + rc.w - 1, rc.y, kHi), line(rc.x, rc.y, rc.x, rc.y + rc.h - 1, kHi);
        line(rc.x, rc.y + rc.h - 1, rc.x + rc.w - 1, rc.y + rc.h - 1, kDark), line(rc.x + rc.w - 1, rc.y, rc.x + rc.w - 1, rc.y + rc.h - 1, kDark);
        line(rc.x + 1, rc.y + rc.h - 2, rc.x + rc.w - 2, rc.y + rc.h - 2, kShadow), line(rc.x + rc.w - 2, rc.y + 1, rc.x + rc.w - 2, rc.y + rc.h - 2, kShadow);
    }
    void sunken(SDL_FRect rc, SDL_Color inside) {
        fill(rc, inside);
        line(rc.x, rc.y, rc.x + rc.w - 1, rc.y, kShadow), line(rc.x, rc.y, rc.x, rc.y + rc.h - 1, kShadow);
        line(rc.x + 1, rc.y + 1, rc.x + rc.w - 2, rc.y + 1, kDark), line(rc.x + 1, rc.y + 1, rc.x + 1, rc.y + rc.h - 2, kDark);
        line(rc.x, rc.y + rc.h - 1, rc.x + rc.w - 1, rc.y + rc.h - 1, kHi), line(rc.x + rc.w - 1, rc.y, rc.x + rc.w - 1, rc.y + rc.h - 1, kHi);
        line(rc.x + 1, rc.y + rc.h - 2, rc.x + rc.w - 2, rc.y + rc.h - 2, kLight), line(rc.x + rc.w - 2, rc.y + 1, rc.x + rc.w - 2, rc.y + rc.h - 2, kLight);
    }
    // 使えない項目の文字は、白い影をつけた灰色（クラシックの見た目）
    float label2(float x, float y, const std::string& s, bool enabled, int align = 0) {
        if (!enabled) text->draw(x + 1, y + 1, font, kHi, s, align, 0, 0);
        return text->draw(x, y, font, enabled ? kText : kGray, s, align, 0, 0);
    }
    bool button(float x, float y, float w, float h, const std::string& s, bool enabled = true) {
        SDL_FRect rc{x, y, w, h};
        bool down = enabled && in(rc) && !combo_open && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
        fill(rc, kFace);
        if (down) frame(rc, kDark), frame({x + 1, y + 1, w - 2, h - 2}, kShadow);
        else raised(rc);
        label2(x + w / 2 + (down ? 1 : 0), y + (h - font) / 2 + (down ? 1 : 0), s, enabled, 1);
        return enabled && clicked(rc);
    }
    bool check(float x, float y, const std::string& s, bool& v, bool enabled = true) {
        SDL_FRect box{x, y, 13, 13};
        sunken(box, enabled ? kHi : kFace);
        if (v) {
            SDL_Color c = enabled ? kText : kGray;
            for (int i = 0; i < 3; ++i) line(x + 3, y + 5 + i, x + 5, y + 7 + i, c), line(x + 5, y + 7 + i, x + 9, y + 3 + i, c);
        }
        float tw = label2(x + 18, y, s, enabled);
        if (enabled && clicked({x, y - 2, 18 + tw, 17})) {
            v = !v;
            return true;
        }
        return false;
    }
    bool radio(float x, float y, const std::string& s, bool on, bool enabled = true) {
        // クラシックの丸：左上が暗く、右下が明るい凹んだ円
        for (int dy = -6; dy <= 5; ++dy)
            for (int dx = -6; dx <= 5; ++dx) {
                float d = std::sqrt((dx + 0.5f) * (dx + 0.5f) + (dy + 0.5f) * (dy + 0.5f));
                if (d > 6.0f) continue;
                SDL_Color c = enabled ? kHi : kFace;
                if (d > 4.6f) c = (dx + dy < 0) ? kShadow : kHi;
                else if (d > 3.6f) c = (dx + dy < 0) ? kDark : kLight;
                fill({x + 6 + dx, y + 6 + dy, 1, 1}, c);
            }
        if (on)
            for (int dy = -2; dy <= 1; ++dy)
                for (int dx = -2; dx <= 1; ++dx)
                    if ((dx + 0.5f) * (dx + 0.5f) + (dy + 0.5f) * (dy + 0.5f) <= 4.2f) fill({x + 6 + dx, y + 6 + dy, 1, 1}, enabled ? kText : kGray);
        float tw = label2(x + 18, y, s, enabled);
        return enabled && clicked({x, y - 2, 18 + tw, 17});
    }
    void group(float x, float y, float w, float h, const std::string& title) {
        float tw = title.empty() ? 0 : text->get(font, 0, title).w + 4;
        auto etched = [&](float x1, float y1, float x2, float y2) { line(x1, y1, x2, y2, kShadow), line(x1 + 1, y1 + 1, x2 + 1, y2 + 1, kHi); };
        etched(x, y + 6, x + 6, y + 6), etched(x + 8 + tw, y + 6, x + w, y + 6);
        etched(x, y + 6, x, y + h), etched(x + w, y + 6, x + w, y + h), etched(x, y + h, x + w, y + h);
        if (!title.empty()) label(x + 8, y, title);
    }
    // 入力欄。文字列はそのまま、数字だけの欄は int に戻す
    void edit_box(const void* id, float x, float y, float w, float h, const std::string& shown, bool enabled,
                  std::function<void(const std::string&)> on_commit) {
        SDL_FRect rc{x, y, w, h};
        sunken(rc, enabled ? kHi : kFace);
        bool focused = focus == id;
        if (focused) {
            if (!typed.empty()) buf += typed;
            if (key_back && !buf.empty()) {
                size_t i = buf.size() - 1;
                while (i > 0 && (static_cast<unsigned char>(buf[i]) & 0xC0) == 0x80) --i;
                buf.erase(i);
            }
            if (key_enter) unfocus();
            else if (key_esc) focus = nullptr, commit = nullptr, SDL_StopTextInput(win);
        }
        SDL_Rect clip{static_cast<int>(x) + 2, static_cast<int>(y), static_cast<int>(w) - 4, static_cast<int>(h)};
        SDL_SetRenderClipRect(r, &clip);
        float tw = label2(x + 4, y + (h - font) / 2, focus == id ? buf : shown, enabled);
        if (focus == id && (SDL_GetTicks() / 500) % 2 == 0) line(x + 5 + tw, y + 3, x + 5 + tw, y + h - 4, kText);
        SDL_SetRenderClipRect(r, nullptr);
        if (enabled && clicked(rc)) {
            focus = id;
            buf = shown;
            commit = std::move(on_commit);
            SDL_StartTextInput(win);
        }
    }
    void edit_int(int& v, float x, float y, float w, float h = 20, bool enabled = true, int lo = INT32_MIN, int hi = INT32_MAX) {
        edit_box(&v, x, y, w, h, std::to_string(v), enabled, [&v, lo, hi](const std::string& s) {
            if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || c == '-'; })) return;
            v = std::clamp(std::atoi(s.c_str()), lo, hi);
        });
    }
    void edit_str(std::string& v, float x, float y, float w, float h = 20, bool enabled = true) {
        edit_box(&v, x, y, w, h, v, enabled, [&v](const std::string& s) { v = s; });
    }
    // コンボボックス（押すと下に一覧が開く）
    void combo(const void* id, float x, float y, float w, float h, const std::vector<std::string>& items, int sel,
               std::function<void(int)> pick, bool enabled = true) {
        SDL_FRect rc{x, y, w, h};
        sunken(rc, enabled ? kHi : kFace);
        SDL_FRect arrow{x + w - h + 2, y + 2, h - 4, h - 4};
        fill(arrow, kFace);
        raised(arrow);
        SDL_Color ac = enabled ? kText : kGray;
        float cx = arrow.x + arrow.w / 2, cy = arrow.y + arrow.h / 2;
        for (int i = 0; i < 4; ++i) line(cx - 3 + i, cy - 1 + i, cx + 3 - i, cy - 1 + i, ac);
        SDL_Rect clip{static_cast<int>(x) + 2, static_cast<int>(y), static_cast<int>(w - h), static_cast<int>(h)};
        SDL_SetRenderClipRect(r, &clip);
        if (sel >= 0 && sel < static_cast<int>(items.size())) label2(x + 4, y + (h - font) / 2, items[sel], enabled);
        SDL_SetRenderClipRect(r, nullptr);
        if (enabled && clicked(rc)) {
            combo_open = id;
            combo_rect = {x, y + h, w, static_cast<float>(items.size()) * 18 + 2};
            combo_items = items;
            combo_pick = std::move(pick);
            pressed = false;
        }
    }
    // 開いているコンボボックスの一覧（ほかの部品より手前に描く）
    void combo_popup() {
        if (!combo_open) return;
        fill(combo_rect, SDL_Color{255, 255, 255, 255});
        frame(combo_rect, kText);
        for (size_t i = 0; i < combo_items.size(); ++i) {
            SDL_FRect row{combo_rect.x + 1, combo_rect.y + 1 + i * 18.0f, combo_rect.w - 2, 18};
            bool hot = in(row);
            if (hot) fill(row, kSel);
            text->draw(row.x + 3, row.y + 3, font, hot ? SDL_Color{255, 255, 255, 255} : kText, combo_items[i], 0, 0, 0);
            if (hot && pressed) {
                combo_pick(static_cast<int>(i));
                combo_open = nullptr;
                pressed = false;
                return;
            }
        }
        if (pressed) combo_open = nullptr, pressed = false;
    }
    // 一覧（JUKEBOX1 の曲検索フォルダなど）。sel は選んでいる行
    void listview(float x, float y, float w, float h, const std::string& header, const std::vector<std::string>& rows,
                  int& sel, int& scroll) {
        SDL_FRect rc{x, y, w, h};
        sunken(rc, kHi);
        fill({x + 2, y + 2, w - 4, 16}, kFace);
        raised({x + 2, y + 2, w - 24, 16});
        raised({x + w - 22, y + 2, 20, 16});
        label(x + 6, y + 3, header);
        int visible = static_cast<int>((h - 22) / 16);
        if (in(rc) && wheel != 0) scroll -= static_cast<int>(wheel) * 3;
        scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));
        for (int i = 0; i < visible && scroll + i < static_cast<int>(rows.size()); ++i) {
            int k = scroll + i;
            SDL_FRect row{x + 2, y + 20 + i * 16.0f, w - 4, 16};
            if (k == sel) fill(row, kSel);
            SDL_Rect clip{static_cast<int>(row.x), static_cast<int>(row.y), static_cast<int>(row.w), static_cast<int>(row.h)};
            SDL_SetRenderClipRect(r, &clip);
            text->draw(row.x + 4, row.y + 2, font, k == sel ? SDL_Color{255, 255, 255, 255} : kText, rows[k], 0, 0, 0);
            SDL_SetRenderClipRect(r, nullptr);
            if (clicked(row)) sel = k;
        }
    }
};

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDLの初期化に失敗: %s\n", SDL_GetError());
        return 1;
    }
    AppConfig cfg;
    const std::string cfg_path = config_path();
    cfg.load(cfg_path);

    SDL_Window* window = SDL_CreateWindow("BMjoy SETUP", kW, kH, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* r = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!r) {
        std::fprintf(stderr, "ウィンドウを作れない: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(r, kW, kH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(r, 1);
    TextCache text(r);
#ifdef _WIN32
    // Windows の標準のダイアログと同じ MS UI ゴシック（msgothic.ttc の 2 番目）を、ぼかさずに描く
    text.set_system_font("MS UI Gothic");
    text.set_antialias(false);
#endif
    Gui g;
    g.r = r, g.win = window, g.text = &text;
#ifndef _WIN32
    g.font = 11;  // MS UI ゴシックより横に広い書体なので、少し小さくして LR2 SETUP の配置に収める
#endif

    // BMjoy に無い項目の見た目だけの値（LR2 SETUP の既定に合わせる）
    bool d_false = false, d_true = true;
    int d_flash = 0, d_buffer = 128;
    std::string d_font = "HG丸ｺﾞｼｯｸM-PRO";

    int tab = 0;
    // MOD（仕様書14章）。公式のあとに利用者のもの
    std::vector<std::string> mod_errors;
    std::vector<ModManifest> mods = scan_mods(official_mods_dir(), true, &mod_errors);
    for (ModManifest& m : scan_mods(user_mods_dir(), false, &mod_errors)) mods.push_back(std::move(m));
    int mod_sel = mods.empty() ? -1 : 0, mod_scroll = 0;
    int song_sel = -1, song_scroll = 0, table_sel = -1, table_scroll = 0;
    std::string table_input;
    std::string message;
    // LR2 からの引き継ぎ（裏で動かす。曲が多いと更新日時を読むのに時間がかかる）
    struct ImportJob {
        std::thread th;
        std::mutex mu;
        std::string status;
        bool running = false, done = false;
        Lr2ImportResult res;
        std::vector<std::string> song_dirs, tables, custom_folder_dirs;
        decltype(AppConfig::skin_custom) skin_custom;
        AppConfig cfg;          // 引き継いだあとの設定（素材の場所を写した先に直したもの）
        AssetCopyResult assets;
        ~ImportJob() {
            if (th.joinable()) th.join();
        }
    } job;
    bool quit = false, start = false;
    bool autostart = dev_env("BMJ_LAUNCHER_AUTOSTART") != nullptr;  // 開発用：すぐ起動する

    while (!quit) {
        g.pressed = false, g.wheel = 0, g.typed.clear();
        g.key_enter = g.key_back = g.key_esc = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_WHEEL) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                if (e.type == SDL_EVENT_MOUSE_MOTION) g.mx = e.motion.x, g.my = e.motion.y;
                else if (e.type == SDL_EVENT_MOUSE_WHEEL) g.wheel += e.wheel.y;
                else if (e.button.button == SDL_BUTTON_LEFT) g.mx = e.button.x, g.my = e.button.y, g.pressed = true;
            }
            if (e.type == SDL_EVENT_TEXT_INPUT) g.typed += e.text.text;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) g.key_enter = true;
                if (e.key.key == SDLK_BACKSPACE) g.key_back = true;
                if (e.key.key == SDLK_ESCAPE) g.key_esc = true;
                if (e.key.key == SDLK_V && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && g.focus)
                    if (char* c = SDL_GetClipboardText()) g.typed += c, SDL_free(c);
            }
            // フォルダをドロップしたら曲検索フォルダに足す（LR2 SETUP の JUKEBOX1 と同じ）
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                std::error_code ec;
                std::string p = e.drop.data;
                if (fs::is_directory(p, ec) && std::find(cfg.song_dirs.begin(), cfg.song_dirs.end(), p) == cfg.song_dirs.end())
                    cfg.song_dirs.push_back(p), tab = 1;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_dialog.mu);
            if (g_dialog.ready && g_dialog.apply) g_dialog.apply(g_dialog.path), g_dialog.ready = false;
        }

        SDL_SetRenderDrawColor(r, kFace.r, kFace.g, kFace.b, 255);
        SDL_RenderClear(r);
        // メニューの帯
        g.fill({0, 0, static_cast<float>(kW), 18}, kHi);
        g.label(6, 3, "Language");

        // タブ（クラシック：選んでいるタブは少し大きく、ページとつながる）
        static const char* tabs[] = {"MAIN", "JUKEBOX1", "JUKEBOX2", "OPTION", "SELECT", "SYSTEM", "BMJOY", "PLAY", "MOD"};
        const SDL_FRect page{10, 56, 590, 368};
        g.fill(page, kFace);
        g.raised(page);
        float tx = 12;
        for (int i = 0; i < 9; ++i) {
            float w = text.get(g.font, 0, tabs[i]).w + 10.0f;
            bool on = i == tab;
            SDL_FRect rc{on ? tx - 2 : tx, on ? 38.0f : 40.0f, on ? w + 4 : w, on ? 19.0f : 16.0f};
            g.fill(rc, kFace);
            g.line(rc.x, rc.y + 2, rc.x, rc.y + rc.h, kHi);
            g.line(rc.x + 2, rc.y, rc.x + rc.w - 3, rc.y, kHi);
            g.fill({rc.x + 1, rc.y + 1, 1, 1}, kHi);
            g.line(rc.x + rc.w - 1, rc.y + 2, rc.x + rc.w - 1, rc.y + rc.h, kDark);
            g.line(rc.x + rc.w - 2, rc.y + 1, rc.x + rc.w - 2, rc.y + rc.h, kShadow);
            if (on) g.line(rc.x + 1, page.y, rc.x + rc.w - 2, page.y, kFace);
            g.label(tx + 5, rc.y + (on ? 3 : 2), tabs[i]);
            if (g.clicked(rc)) tab = i;
            tx += w;
        }

        const float X = page.x, Y = page.y;  // ページの左上（以下の座標は LR2 SETUP の画面に合わせてある）
        if (tab == 0) {
            // LR2 SETUP（100201）の MAIN と同じ並び。プレイヤーと IR は BMjoy に無いので灰色
            g.group(X + 9, Y + 9, 556, 99, "PLAYER");
            g.label(X + 26, Y + 40, "PLAYER ID");
            g.combo(&d_flash, X + 90, Y + 33, 142, 19, {""}, 0, [](int) {}, false);
            g.label(X + 251, Y + 40, "PASSWORD");
            std::string pw;
            g.edit_str(pw, X + 324, Y + 33, 107, 21, false);
            g.button(X + 27, Y + 72, 143, 23, "新規プレイヤーデータ作成", false);
            g.button(X + 189, Y + 72, 143, 23, "プレイヤーデータ削除", false);
            bool windowed = !cfg.fullscreen;
            if (g.check(X + 19, Y + 132, "ウインドウモード", windowed)) cfg.fullscreen = !windowed;
            g.label(X + 152, Y + 135, "ウインドウサイズ");
            g.label(X + 243, Y + 137, "X");
            g.edit_int(cfg.window_w, X + 261, Y + 121, 59, 22, true, 0, 7680);
            g.label(X + 333, Y + 137, "Y");
            g.edit_int(cfg.window_h, X + 351, Y + 121, 59, 22, true, 0, 4320);
            g.label(X + 17, Y + 164, "データベース自動更新");
            if (g.radio(X + 136, Y + 164, "手動のみ(F8で現在選択中のフォルダの更新を行えます)", cfg.db_update == 1)) cfg.db_update = 1;
            g.radio(X + 136, Y + 184, "開いたフォルダに対して自動で行う (フォルダ読み込み時間が少し長くなります)", false, false);
            if (g.radio(X + 136, Y + 203, "起動時に全フォルダに対して行う (起動時間が長くなります)", cfg.db_update == 0)) cfg.db_update = 0;
            g.check(X + 19, Y + 242, "インターネットランキングに参加する", d_false, false);
            g.check(X + 54, Y + 271, "ライバルデータを取得する", d_false, false);
            g.check(X + 54, Y + 301, "ランキングを自動更新する", d_false, false);
            g.button(X + 18, Y + 336, 224, 22, "マイページに接続", false);
            g.button(X + 261, Y + 336, 224, 22, "トップページに接続", false);
        } else if (tab == 1) {
            g.label(X + 30, Y + 16, "曲検索フォルダ");
            g.label(X + 30, Y + 36, "フォルダをドラッグ・ドロップで登録できます。");
            g.listview(X + 28, Y + 72, 522, 240, "path", cfg.song_dirs, song_sel, song_scroll);
            if (g.button(X + 30, Y + 322, 74, 22, "追加"))
                pick_folder(window, [&](const std::string& p) {
                    if (std::find(cfg.song_dirs.begin(), cfg.song_dirs.end(), p) == cfg.song_dirs.end()) cfg.song_dirs.push_back(p);
                });
            bool has = song_sel >= 0 && song_sel < static_cast<int>(cfg.song_dirs.size());
            if (g.button(X + 128, Y + 322, 74, 22, "登録解除", has)) cfg.song_dirs.erase(cfg.song_dirs.begin() + song_sel), song_sel = -1;
            if (g.button(X + 228, Y + 322, 84, 22, "フォルダを開く", has))
                SDL_OpenURL(("file://" + fs::path(cfg.song_dirs[song_sel]).generic_string()).c_str());
        } else if (tab == 2) {
            g.group(X + 10, Y + 8, 558, 266, "特殊フォルダ");
            static const char* folders[] = {"RANDOM SELECT", "FAVORITE FOLDER", "IGNORE FOLDER", "PLAYCOUNT TOP 10 FOLDER",
                                            "LEVEL FOLDER", "CLEAR FOLDER", "PLAYRANK FOLDER", "INSANE BMS FOLDER"};
            for (int i = 0; i < 8; ++i) g.check(X + 30, Y + 42 + i * 29.5f, folders[i], d_false, false);
            g.label(X + 28, Y + 292, "検索最大ヒット数");
            g.edit_int(cfg.search_max, X + 128, Y + 286, 80, 22, true, 0, 100000);
            g.label(X + 220, Y + 292, "(0:無制限)");
            g.label(X + 28, Y + 332, "タイトルのフラッシュ");
            g.edit_int(cfg.title_flash, X + 128, Y + 326, 80, 22, true, 0, 100000);
            g.label(X + 220, Y + 330, "時間以内に追加した曲");
        } else if (tab == 3) {
            g.label(X + 28, Y + 28, "HI SPEED");
            g.label(X + 138, Y + 28, "MIN");
            g.edit_int(cfg.hs_min, X + 164, Y + 22, 58, 20, true, 10, 990);
            g.label(X + 253, Y + 28, "MAX");
            g.edit_int(cfg.hs_max, X + 278, Y + 22, 58, 20, true, 10, 990);
            g.label(X + 418, Y + 28, "MARGIN");
            g.edit_int(cfg.hs_margin, X + 444, Y + 22, 58, 20, true, 1, 100);
            g.label(X + 28, Y + 56, "基本スクロールスピード");
            g.edit_int(cfg.base_speed, X + 164, Y + 50, 58, 20, true, 10, 1000);
            g.label(X + 253, Y + 56, "レーンカバー変化間隔");
            g.label(X + 398, Y + 56, "MARGIN");
            g.edit_int(cfg.shutter_margin, X + 462, Y + 50, 58, 20, true, 1, 100);
            g.label(X + 28, Y + 88, "ミスBGA表示時間");
            g.edit_int(cfg.poor_bga_ms, X + 164, Y + 80, 58, 20, true, 0, 10000);
            g.label(X + 233, Y + 94, "ms");
            g.label(X + 28, Y + 120, "最低連打間隔(これ以下の連打は無効になります)");
            g.edit_int(cfg.input_interval, X + 362, Y + 108, 58, 20, true, 0, 200);
            g.label(X + 433, Y + 124, "ms");
            g.label(X + 28, Y + 155, "選曲リスト");
            g.label(X + 138, Y + 155, "最初");
            g.edit_int(cfg.select_speed_first, X + 164, Y + 148, 58, 20, true, 10, 2000);
            g.label(X + 233, Y + 155, "ms / song");
            g.label(X + 323, Y + 155, "以降");
            g.edit_int(cfg.select_speed_next, X + 362, Y + 148, 58, 20, true, 10, 2000);
            g.label(X + 433, Y + 155, "ms / song");
            g.check(X + 28, Y + 194, "画像フォントを無効にする", d_false, false);
            g.label(X + 236, Y + 194, "無効時のフォント", false);
            g.edit_str(d_font, X + 326, Y + 186, 116, 22, false);
            g.button(X + 452, Y + 186, 74, 22, "SELECT", false);
            g.check(X + 28, Y + 224, "選曲にポプコンを使う", d_false, false);
            g.check(X + 236, Y + 224, "フォルダランプ(曲が多いと非常に重くなります)", cfg.folder_lamp);
            g.check(X + 28, Y + 252, "1/3キーを選曲に割り当てる", d_false, false);
            g.check(X + 236, Y + 252, "システムキー(windowsキー等)の無効化", d_false, false);
            g.check(X + 28, Y + 280, "システムログの出力(重くなります)", d_false, false);
            g.check(X + 236, Y + 280, "スキンセレクトでプレビューを表示しない", d_false, false);
            g.check(X + 28, Y + 308, "暗転中に曲のロードを行う", d_false, false);
            g.check(X + 236, Y + 308, "瞬殺された/escを押したスコアもIR登録する", d_false, false);
            g.check(X + 28, Y + 338, "右クリックで曲を終了させない", cfg.no_right_click_exit);
            g.check(X + 236, Y + 338, "上下キーを速度変更に割り当てる", d_true, false);
        } else if (tab == 4) {
            g.label(X + 28, Y + 28, "これらのオプションはジュークボックスの構成によっては不具合を起こす可能性があります");
            g.check(X + 28, Y + 58, "難度変更の挙動をbeta1のものに戻す", d_false, false);
            g.check(X + 28, Y + 87, "鍵盤数ALLを使用しない", d_false, false);
            g.check(X + 290, Y + 87, "難度ALLを使用しない", d_false, false);
            g.check(X + 28, Y + 116, "鍵盤数SINGLE(5+7同時表示)を使用しない", d_false, false);
            g.check(X + 290, Y + 116, "鍵盤数DOUBLE(10+14同時表示)を使用しない", d_false, false);
            g.check(X + 28, Y + 146, "DP譜面は無視する", d_false, false);
            g.check(X + 290, Y + 146, "9ボタン譜面は無視する", d_false, false);
            g.check(X + 28, Y + 176, "7鍵譜面が存在する場合5鍵譜面を無視する", d_false, false);
            g.check(X + 290, Y + 176, "難度フィルタを使用しない", d_false, false);
            g.label(X + 28, Y + 212, "レベルバー最大値");
            g.label(X + 28, Y + 240, "5/10KEYS");
            g.edit_int(cfg.levelbar_5, X + 102, Y + 226, 58, 22, true, 1, 99);
            g.label(X + 28, Y + 268, "7/14KEYS");
            g.edit_int(cfg.levelbar_7, X + 102, Y + 255, 58, 22, true, 1, 99);
            g.label(X + 28, Y + 296, "9BUTTONS");
            g.edit_int(cfg.levelbar_9, X + 102, Y + 284, 58, 22, false, 1, 99);  // 9 ボタンは扱わない
        } else if (tab == 5) {
            g.group(X + 10, Y + 8, 558, 90, "DISPLAY");
            g.check(X + 38, Y + 38, "16bitモードで起動する(フルスクリーンモードのみ)", d_false, false);
            g.check(X + 38, Y + 68, "垂直同期(プレイ中ちらつくなら変えてみてください)", cfg.vsync);
            g.group(X + 10, Y + 114, 558, 186, "SOUND");
#ifdef _WIN32
            g.label(X + 48, Y + 154, "OUTPUT TYPE");
            g.combo(&cfg.audio_exclusive, X + 182, Y + 145, 250, 22, {"WASAPI（共有）", "WASAPI（排他）"}, cfg.audio_exclusive ? 1 : 0,
                    [&](int i) { cfg.audio_exclusive = i == 1; });
#else
            g.label(X + 48, Y + 154, "OUTPUT TYPE", false);
#ifdef __APPLE__
            const char* out_type = "Core Audio";
#else
            const char* out_type = "ALSA / PulseAudio";
#endif
            g.combo(&d_buffer + 1, X + 182, Y + 145, 250, 22, {out_type}, 0, [](int) {}, false);
#endif
            g.label(X + 48, Y + 195, "PLAYBACK DRIVER", false);
            g.combo(&d_buffer + 2, X + 182, Y + 187, 250, 22, {"既定のデバイス"}, 0, [](int) {}, false);
            g.label(X + 48, Y + 235, "バッファ数 (低くすれば音の出が早くなりますがノイズが増えます)");
            g.edit_int(cfg.audio_buffer, X + 462, Y + 226, 58, 22, true, 0, 4096);
            g.check(X + 48, Y + 268, "FMOD Exを使用しない(どうしても発音遅延やノイズが気になるならチェックを入れてください)", d_true, false);
            g.check(X + 22, Y + 328, "高精度なタイマーを使用する(基本的に外して下さい)", d_true, false);
        } else if (tab == 6) {
            // BMjoy だけの設定（LR2 SETUP には無い）
            if (g.button(X + 268, Y + 10, 146, 22, "LR2 から引き継ぐ", !job.running)) {
                pick_folder(window, [&](const std::string& dir) {
                    job.running = true, job.done = false;
                    job.status = "LR2 から引き継いでいます";
                    job.th = std::thread([&job, dir, base = cfg] {
                        AppConfig c = base;
                        SongDb db;
                        Lr2ImportResult r;
                        if (!db.open(song_db_path())) r.error = "曲データベースを開けません";
                        else r = import_from_lr2(dir, c, db, [&job](const std::string& s) {
                            std::lock_guard<std::mutex> lock(job.mu);
                            job.status = s;
                        }, base.lr2_root);
                        // 素材（スキン・サウンドセット・BGM など）も BMjoy のフォルダ（実行ファイルの横）に写し、そこから読む
                        AssetCopyResult ac;
                        if (r.ok) {
                            if (c.lr2_root.empty()) c.lr2_root = dir;
                            const char* exe_dir = SDL_GetBasePath();
                            ac = copy_lr2_assets(c, exe_dir ? exe_dir : "", [&job](const std::string& s) {
                                std::lock_guard<std::mutex> lock(job.mu);
                                job.status = s;
                            });
                        }
                        std::lock_guard<std::mutex> lock(job.mu);
                        job.assets = ac;
                        job.cfg = c;
                        job.res = r;
                        job.song_dirs = c.song_dirs;
                        job.custom_folder_dirs = c.custom_folder_dirs;
                        job.tables = c.tables;
                        job.skin_custom = c.skin_custom;
                        job.done = true;
                    });
                });
            }
            if (g.button(X + 420, Y + 10, 146, 22, "曲データベースの再作成")) {
                SongDb db;
                message = db.open(song_db_path()) && db.clear_charts() ? "次回の起動時に曲データベースを再作成します"
                                                                      : "曲データベースを開けません";
            }
            g.check(X + 20, Y + 12, "曲フォルダを監視する", cfg.watch_folders);
            g.label(X + 20, Y + 44, "素材のフォルダ");
            g.edit_str(cfg.lr2_root, X + 124, Y + 38, 360, 20);
            if (g.button(X + 492, Y + 37, 74, 22, "参照")) pick_folder(window, [&](const std::string& p) { cfg.lr2_root = p; });
            g.label(X + 20, Y + 74, "解像度");
            static const std::vector<std::string> res_names = {"スキンに合わせる", "SD（640×480）", "HD（1280×720）", "FHD（1920×1080）"};
            static const char* res_vals[] = {"", "sd", "hd", "fhd"};
            int ri = 0;
            while (ri < 4 && cfg.skin_res != res_vals[ri]) ++ri;
            g.combo(&cfg.skin_res, X + 124, Y + 68, 180, 20, res_names, ri == 4 ? 0 : ri, [&](int i) { cfg.skin_res = res_vals[i]; });
            g.label(X + 316, Y + 74, "fps の上限");
            static const int fps_vals[] = {0, -1, 60, 120, 144, 240, 360, 480, 1000};
            static const std::vector<std::string> fps_names = {"モニタに合わせる", "上限なし", "60", "120", "144", "240", "360", "480", "1000"};
            int fi = 0;
            while (fi < 9 && fps_vals[fi] != cfg.fps) ++fi;
            g.combo(&cfg.fps, X + 392, Y + 68, 174, 20, fps_names, fi == 9 ? 0 : fi, [&](int i) { cfg.fps = fps_vals[i]; });
            struct SkinRow { const char* name; std::string* path; };
            SkinRow skins[] = {{"選曲スキン", &cfg.select_skin}, {"プレイスキン", &cfg.play_skin}, {"リザルトスキン", &cfg.result_skin},
                               {"コースリザルト", &cfg.courseresult_skin}};
            float y = Y + 102;
            for (SkinRow& sr : skins) {
                g.label(X + 20, y + 6, sr.name);
                std::string shown = sr.path->empty() ? "（既定）" : *sr.path;
                g.edit_str(*sr.path, X + 124, y, 280, 20);
                if (sr.path->empty() && g.focus != sr.path) g.label(X + 128, y + 4, "（既定）", false);
                std::string* target = sr.path;
                if (g.button(X + 412, y - 1, 74, 22, "参照")) pick_skin(window, [target](const std::string& p) { *target = p; });
                if (g.button(X + 492, y - 1, 74, 22, "既定に戻す")) sr.path->clear();
                y += 28;
            }
            g.label(X + 20, y + 10, "難易度表（URL）");
            g.listview(X + 20, y + 28, 546, 76, "url", cfg.tables, table_sel, table_scroll);
            float by = y + 112;
            g.edit_str(table_input, X + 20, by, 380, 20);
            if (g.button(X + 410, by - 1, 74, 22, "追加", !table_input.empty())) {
                if (std::find(cfg.tables.begin(), cfg.tables.end(), table_input) == cfg.tables.end()) cfg.tables.push_back(table_input);
                table_input.clear();
            }
            bool has = table_sel >= 0 && table_sel < static_cast<int>(cfg.tables.size());
            if (g.button(X + 492, by - 1, 74, 22, "登録解除", has)) cfg.tables.erase(cfg.tables.begin() + table_sel), table_sel = -1;
            g.check(X + 20, by + 30, "選曲中に曲のプレビューを鳴らす", cfg.preview);
#ifdef _WIN32
            g.check(X + 280, by + 30, "コントローラーを DirectInput でも探す", cfg.joystick_directinput);
#endif
        } else if (tab == 8) {
            // MOD：使うものに印を付ける。プレーに影響する MOD（判定・譜面・入力）を使うと、記録はふだんのものと分かれる
            float y = Y + 12;
            const float row_h = 22;
            const int rows = 8;
            g.sunken({X + 10, y, 558, rows * row_h + 4}, kHi);
            mod_scroll = std::clamp(mod_scroll, 0, std::max(0, static_cast<int>(mods.size()) - rows));
            for (int i = mod_scroll; i < static_cast<int>(mods.size()) && i < mod_scroll + rows; ++i) {
                const ModManifest& m = mods[i];
                float ry = y + 2 + (i - mod_scroll) * row_h;
                if (i == mod_sel) g.fill({X + 12, ry, 554, row_h}, kLight);
                auto it = std::find(cfg.enabled_mods.begin(), cfg.enabled_mods.end(), m.id);
                bool on = it != cfg.enabled_mods.end();
                std::string name = m.name + "  " + m.version + (m.official ? "  （公式）" : "") + (m.affects_play() ? "  ＊記録を分ける" : "");
                if (g.check(X + 18, ry + 4, name, on)) {
                    if (on) cfg.enabled_mods.push_back(m.id);
                    else cfg.enabled_mods.erase(it);
                    mod_sel = i;
                } else if (g.clicked({X + 12, ry, 554, row_h})) {
                    mod_sel = i;
                }
            }
            if (g.wheel != 0 && g.mx >= X + 10 && g.mx < X + 568 && g.my >= y && g.my < y + rows * row_h)
                mod_scroll -= static_cast<int>(g.wheel);
            if (mods.empty()) g.label(X + 20, y + 8, "MOD が見つかりません（" + user_mods_dir() + " に入れる）", false);
            y += rows * row_h + 14;
            if (mod_sel >= 0 && mod_sel < static_cast<int>(mods.size())) {
                const ModManifest& m = mods[mod_sel];
                g.label(X + 14, y, m.author.empty() ? m.id : m.id + "  作者：" + m.author);
                g.label(X + 14, y + 18, m.description);
                std::string parts;
                for (const CustomGaugeDef& gd : m.gauges) parts += (parts.empty() ? "" : "、") + gd.name;
                if (!parts.empty()) g.label(X + 14, y + 36, "ゲージ：" + parts);
                parts.clear();
                for (const CustomJudgeDef& jd : m.judges) parts += (parts.empty() ? "" : "、") + jd.name;
                if (!parts.empty()) g.label(X + 14, y + 54, "判定：" + parts);
            }
            // 組み合わせの問題（判定の MOD が 2 つなど）
            std::vector<const ModManifest*> on;
            for (const std::string& id : cfg.enabled_mods)
                for (const ModManifest& m : mods)
                    if (m.id == id) on.push_back(&m);
            float wy = Y + 300;
            for (const std::string& w : mod_conflicts(on)) g.label(X + 14, wy, "！ " + w), wy += 18;
            for (const std::string& e : mod_errors) g.label(X + 14, wy, e, false), wy += 18;
        } else {
            // ハイスピ（仕様書6章）
            g.label(X + 28, Y + 30, "ハイスピのモード");
            g.combo(&cfg.hs_mode, X + 182, Y + 22, 200, 22, {"クラシック", "フローティング"}, std::clamp(cfg.hs_mode, 0, 1),
                    [&](int i) { cfg.hs_mode = i; });
            bool fl = cfg.hs_mode == 1;
            g.group(X + 10, Y + 62, 558, 150, "フローティング");
            g.label(X + 38, Y + 92, "緑数字", fl);
            g.edit_int(cfg.fhs_green, X + 182, Y + 84, 58, 20, fl, 50, 3000);
            g.label(X + 38, Y + 124, "基準の BPM", fl);
            g.combo(&cfg.fhs_base, X + 182, Y + 116, 200, 22, {"開始", "最大", "最小", "一番長く使われる BPM"},
                    std::clamp(cfg.fhs_base, 0, 3), [&](int i) { cfg.fhs_base = i; }, fl);
            g.label(X + 38, Y + 156, "ギアチェン", fl);
            g.combo(&cfg.gear_mode, X + 182, Y + 148, 200, 22, {"カバー追従", "緑数字に戻す"}, std::clamp(cfg.gear_mode, 0, 1),
                    [&](int i) { cfg.gear_mode = i; }, fl);
            g.group(X + 10, Y + 226, 558, 60, "LIFT");
            g.check(X + 38, Y + 256, "LIFT を使う", cfg.lift);
            g.label(X + 182, Y + 256, "量", cfg.lift);
            g.edit_int(cfg.lift_v, X + 210, Y + 248, 58, 20, cfg.lift, 0, 100);
            g.label(X + 300, Y + 312, "判定の自動調整");
            g.combo(&cfg.auto_adjust, X + 400, Y + 304, 150, 22, {"OFF", "ON", "SILENT"}, std::clamp(cfg.auto_adjust, 0, 2),
                    [&](int i) { cfg.auto_adjust = i; });
            g.label(X + 28, Y + 312, "LN の種類");
            g.combo(&cfg.ln_mode, X + 182, Y + 304, 100, 22, {"LN", "CN", "HCN"}, std::clamp(cfg.ln_mode, 0, 2),
                    [&](int i) { cfg.ln_mode = i; });
        }
        g.combo_popup();

        if (job.running) {
            std::lock_guard<std::mutex> lock(job.mu);
            message = job.status;
            if (job.done) {
                job.th.join();
                job.running = false;
                if (job.res.ok) {
                    cfg.song_dirs = job.song_dirs;
                    cfg.custom_folder_dirs = job.custom_folder_dirs;
                    cfg.tables = job.tables;
                    cfg.skin_custom = job.skin_custom;
                    if (job.assets.ok) {
                        // 素材の場所は BMjoy のフォルダに付け替えたもの
                        const AppConfig& n = job.cfg;
                        cfg.lr2_root = n.lr2_root, cfg.play_skin = n.play_skin, cfg.result_skin = n.result_skin;
                        cfg.courseresult_skin = n.courseresult_skin, cfg.select_skin = n.select_skin, cfg.decide_skin = n.decide_skin;
                        cfg.keyconfig_skin = n.keyconfig_skin, cfg.sound_set = n.sound_set;
                        cfg.skin_custom = n.skin_custom, cfg.skin_adjust = n.skin_adjust, cfg.custom_folder_dirs = n.custom_folder_dirs;
                    }
                    message = "LR2 から引き継ぎました（曲フォルダ " + std::to_string(job.res.song_dirs) + "、譜面 " +
                              std::to_string(job.res.charts) + "、スコア " + std::to_string(job.res.scores) + "、難易度表 " +
                              std::to_string(job.res.tables) + "、スキン " + std::to_string(job.res.skins) + "）";
                } else {
                    message = job.res.error;
                }
            }
        }
        if (!message.empty()) g.label(10, kH - 50 + 14, message);
        if (g.button(10, kH - 29, 87, 21, "BMjoyの起動") || autostart) start = true, autostart = false;
        g.button(115, kH - 29, 87, 21, "プレイオプション", false);
        g.button(409, kH - 29, 83, 21, "動画作成", false);
        if (g.button(514, kH - 29, 87, 21, "終了")) {
            g.unfocus();
            cfg.save(cfg_path);
            quit = true;
        }

        if (start) {
            start = false;
            g.unfocus();
            std::string err;
            if (!cfg.save(cfg_path)) {
                message = "設定を保存できません: " + cfg_path;
            } else if (std::printf("本体を起動します\n"), !launch_body(err)) {
                message = err;
                std::printf("%s\n", err.c_str());
            } else {
                quit = true;
            }
        }
        if (const char* shot = dev_env("BMJ_LAUNCHER_SHOT")) {
            // 開発用：BMJ_LAUNCHER_TAB のタブを1枚保存して終わる
            static int frames = 0;
            if (const char* t = dev_env("BMJ_LAUNCHER_TAB")) tab = std::atoi(t);
            if (++frames == 10) {
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
        }
        SDL_RenderPresent(r);
    }
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
