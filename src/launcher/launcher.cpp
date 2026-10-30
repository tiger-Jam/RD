// 起動前の設定アプリ（LR2 の LR2.exe「LR2 SETUP」に相当）。見た目とタブの並びは LR2 SETUP と同じにしてある
// （LR2beta3 付属マニュアル help/8.html の画像）。RD で使えない項目は同じ場所に灰色で置き、RD だけの項目は
// 最後の「RD」タブにまとめる。「RDの起動」で本体（RDbody）を立ち上げて、自分は閉じる
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <random>
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
#include "app/bgm_set.h"
#include "audio/system_sound.h"
#include "app/lr2_import.h"
#include "app/skin_catalog.h"
#include "db/song_db.h"
#include "mod/mod.h"
#include "render/text_cache.h"
#include "app/service.h"
#include "util/data_dir.h"
#include "util/log.h"
#include "util/dev.h"
#include "net/borrow.h"
#include "yyjson.h"

namespace fs = std::filesystem;
using namespace rd;

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

// start はダイアログを開く場所（空なら OS に任せる。Windows は前に開いた場所になる）
void pick_folder(SDL_Window* w, std::function<void(const std::string&)> apply, const std::string& start = {}) {
    {
        std::lock_guard<std::mutex> lock(g_dialog.mu);
        g_dialog.apply = std::move(apply);
        g_dialog.ready = false;
    }
    static std::string where;  // ダイアログが閉じるまで生きている文字列（SDL は呼んだあとも読むことがある）
    where = start;
    SDL_ShowOpenFolderDialog(dialog_cb, nullptr, w, where.empty() ? nullptr : where.c_str(), false);
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
    std::wstring exe = wide(dir + "RDbody.exe");
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, wide(dir).c_str(), &si, &pi)) {
        err = "RDbody.exe を起動できない（" + std::to_string(GetLastError()) + "）";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::string exe = dir + "RDbody";
    if (!fs::exists(exe)) {
        err = "RDbody が見つからない: " + exe;
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

// 設定アプリ（自分）を起動し直す（新しい版に入れ替えたあと）
void relaunch_self() {
    const char* base = SDL_GetBasePath();
    const std::string dir = base ? base : "";
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        w.resize(n > 0 ? n - 1 : 0);
        return w;
    };
    std::wstring exe = wide(dir + "RD.exe");
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, wide(dir).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#endif
}

// インターネットに出す口の番号（Tailscale の funnel が使える 443・8443・10000 のうち、ほかと重なりにくいもの）
constexpr int kFunnelPort = 8443;

#ifdef _WIN32
std::wstring tailscale_exe() {
    wchar_t pf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    return (n ? std::wstring(pf) : std::wstring(L"C:\\Program Files")) + L"\\Tailscale\\tailscale.exe";
}

// コマンドを黒い窓を出さずに動かし、標準出力を返す（終わるまで最大 10 秒待つ）
std::string run_capture(std::wstring cmd) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr, si.hStdError = wr, si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}
#endif

// この PC の Tailscale の名前（例 tiger-pc.tailxxxx.ts.net）。Tailscale が無ければ空
std::string tailscale_name() {
#ifdef _WIN32
    const std::string json = run_capture(L"\"" + tailscale_exe() + L"\" status --json --peers=false");
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    const char* v = doc ? yyjson_get_str(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "Self"), "DNSName")) : nullptr;
    std::string name = v ? v : "";
    yyjson_doc_free(doc);
    while (!name.empty() && name.back() == '.') name.pop_back();
    return name;
#else
    return {};
#endif
}

// 合言葉を作る（見間違えやすい 0・o・1・l・i を除いた英小文字と数字で 12 文字）
std::string make_serve_key() {
    static const char kChars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::string k;
    for (int i = 0; i < 12; ++i) k += kChars[rd() % (sizeof(kChars) - 1)];
    return k;
}

// 譜面を貸す（RDbody --serve）を、ログインしている間ずっと動かす（Windows のタスクスケジューラ）。
// 設定を変えたら、動いているものを止めて登録し直し、すぐ動かす。貸さないなら登録を消す
void apply_serve(const AppConfig& cfg, std::string& message) {
#ifdef _WIN32
    const char* base = SDL_GetBasePath();
    const std::string dir = base ? base : "";
    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        w.resize(n > 0 ? n - 1 : 0);
        return w;
    };
    // schtasks を黒い窓を出さずに動かし、終わるのを待つ
    auto run = [](std::wstring cmd) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, 15000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code == 0;
    };
    const std::wstring task = L"RD 譜面の配信";
    run(L"schtasks /End /TN \"" + task + L"\"");
    // 前の名前（BMjoy）のときに登録したものは消す
    run(L"schtasks /End /TN \"BMjoy 譜面の配信\""), run(L"schtasks /Delete /F /TN \"BMjoy 譜面の配信\"");
    // 作りかけ（WIP）の間は、前に登録したものも止めて消す
    const bool serving = kShareReady && cfg.serve && static_cast<int>(cfg.serve_key.size()) >= kMinServeKey;
    // インターネットに出す口（Tailscale の funnel）。貸さないときと、外に出さないときは閉じる
    const std::wstring ts = L"\"" + tailscale_exe() + L"\"";
    if (!serving || !cfg.serve_public) run(ts + L" funnel --https=" + std::to_wstring(kFunnelPort) + L" off");
    if (!serving) {
        run(L"schtasks /Delete /F /TN \"" + task + L"\"");
        if (kShareReady && cfg.serve) message = "合言葉が " + std::to_string(kMinServeKey) + " 文字より短いので、譜面を貸しません";
        return;
    }
    const std::wstring exe = wide(dir + "RDbody.exe");
    const bool ok = run(L"schtasks /Create /F /SC ONLOGON /RL LIMITED /TN \"" + task + L"\" /TR \"\\\"" + exe + L"\\\" --serve\"") &&
                    run(L"schtasks /Run /TN \"" + task + L"\"");
    message = ok ? "譜面を貸し始めました（ポート " + std::to_string(cfg.serve_port) + "）" : "譜面の配信を登録できません";
    if (ok && cfg.serve_public &&
        !run(ts + L" funnel --bg --https=" + std::to_wstring(kFunnelPort) + L" http://127.0.0.1:" + std::to_wstring(cfg.serve_port)))
        message = "譜面を貸し始めましたが、インターネットには出せません（Tailscale の funnel が使えるか確かめてください）";
#else
    (void)cfg;
    if (cfg.serve) message = "譜面を貸すのは Windows だけです（RDbody --serve を動かしてください）";
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
    int combo_first = 0, combo_rows = 0;  // 一覧の見えている先頭の行と、見えている行の数（窓に入らなければホイールで送る）
    std::function<void(int)> combo_pick;
    // 開いているのが色の見本（color_button）なら true。選んだ色は color_target に入る
    bool combo_colors = false;
    uint32_t* color_target = nullptr;

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
            combo_colors = false;
            // 窓に収まる行の数だけ見せる。下に入らなければ、広いほう（上か下）に開く
            const int n = static_cast<int>(items.size());
            const int below = static_cast<int>((kH - (y + h) - 4) / 18), above = static_cast<int>((y - 4) / 18);
            const bool up = n > below && above > below;
            combo_rows = std::max(1, std::min(n, up ? above : below));
            const float ph = combo_rows * 18.0f + 2;
            combo_rect = {x, up ? y - ph : y + h, w, ph};
            combo_first = std::clamp(sel - combo_rows / 2, 0, std::max(0, n - combo_rows));  // 選んでいる行が見えるように
            combo_items = items;
            combo_pick = std::move(pick);
            pressed = false;
        }
    }
    // 色の四角（押すと色の見本が開き、選んだ色になる）
    void color_button(uint32_t* c, float x, float y, float w, float h, bool enabled = true) {
        SDL_FRect rc{x, y, w, h};
        sunken(rc, kHi);
        fill({x + 3, y + 3, w - 6, h - 6}, enabled ? rgb(*c) : kFace);
        if (enabled && clicked(rc)) {
            combo_open = c;
            combo_colors = true;
            color_target = c;
            combo_rect = {x, y + h, 4 * 26 + 4, 4 * 26 + 4};
            pressed = false;
        }
    }
    static SDL_Color rgb(uint32_t c) { return SDL_Color{static_cast<Uint8>(c >> 16), static_cast<Uint8>(c >> 8), static_cast<Uint8>(c), 255}; }
    // 開いているコンボボックスの一覧（ほかの部品より手前に描く）
    void combo_popup() {
        if (!combo_open) return;
        if (combo_colors) {
            static const uint32_t palette[16] = {0xff3030, 0xff9020, 0xffd800, 0xa0ff30, 0x30d040, 0x30e0ff, 0x3070ff, 0x2030a0,
                                                 0x8040ff, 0xff40d0, 0xff90c0, 0xa06030, 0xffffff, 0xc0c0c0, 0x808080, 0x202020};
            fill(combo_rect, kFace);
            frame(combo_rect, kText);
            for (int i = 0; i < 16; ++i) {
                SDL_FRect cell{combo_rect.x + 3 + (i % 4) * 26.0f, combo_rect.y + 3 + (i / 4) * 26.0f, 24, 24};
                fill(cell, rgb(palette[i]));
                frame(cell, in(cell) ? kSel : kShadow);
                if (in(cell) && pressed) {
                    if (color_target) *color_target = palette[i];
                    combo_open = nullptr, combo_colors = false, pressed = false;
                    return;
                }
            }
            if (pressed) combo_open = nullptr, combo_colors = false, pressed = false;
            return;
        }
        fill(combo_rect, SDL_Color{255, 255, 255, 255});
        frame(combo_rect, kText);
        const int n = static_cast<int>(combo_items.size());
        if (n > combo_rows) {
            // 入りきらない一覧：ホイールで送る（下の部品には渡さない）。右の端に今の位置を出す
            if (in(combo_rect) && wheel != 0) combo_first -= static_cast<int>(wheel) * 3, wheel = 0;
            combo_first = std::clamp(combo_first, 0, n - combo_rows);
            const float track = combo_rect.h - 2, thumb = std::max(12.0f, track * combo_rows / n);
            fill({combo_rect.x + combo_rect.w - 7, combo_rect.y + 1 + (track - thumb) * combo_first / (n - combo_rows), 5, thumb}, kShadow);
        }
        for (int i = combo_first; i < std::min(n, combo_first + combo_rows); ++i) {
            SDL_FRect row{combo_rect.x + 1, combo_rect.y + 1 + (i - combo_first) * 18.0f, combo_rect.w - (n > combo_rows ? 10 : 2), 18};
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
    // gray：先頭のこの数の行は灰色で書く（変えられない見本）
    void listview(float x, float y, float w, float h, const std::string& header, const std::vector<std::string>& rows,
                  int& sel, int& scroll, int gray = 0) {
        SDL_FRect rc{x, y, w, h};
        sunken(rc, kHi);
        fill({x + 2, y + 2, w - 4, 16}, kFace);
        raised({x + 2, y + 2, w - 24, 16});
        raised({x + w - 22, y + 2, 20, 16});
        label(x + 6, y + 3, header);
        int visible = static_cast<int>((h - 22) / 16);
        if (in(rc) && wheel != 0 && !(combo_open && in(combo_rect))) scroll -= static_cast<int>(wheel) * 3;  // 開いている一覧の上なら、そちらを送る
        scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));
        for (int i = 0; i < visible && scroll + i < static_cast<int>(rows.size()); ++i) {
            int k = scroll + i;
            SDL_FRect row{x + 2, y + 20 + i * 16.0f, w - 4, 16};
            if (k == sel) fill(row, kSel);
            SDL_Rect clip{static_cast<int>(row.x), static_cast<int>(row.y), static_cast<int>(row.w), static_cast<int>(row.h)};
            SDL_SetRenderClipRect(r, &clip);
            text->draw(row.x + 4, row.y + 2, font, k == sel ? SDL_Color{255, 255, 255, 255} : k < gray ? kGray : kText, rows[k], 0, 0, 0);
            SDL_SetRenderClipRect(r, nullptr);
            if (clicked(row)) sel = k;
        }
    }
};

// 本体のランプのゲームの中での名前と、色（色はスキンの絵で決まる。設定画面の見本用に W-MIX の絵の色を写した）
const char* const kStandardLampNames[6] = {"FAILED", "EASY CLEAR", "CLEAR", "HARD CLEAR", "FULL COMBO", "PERFECT"};
// その時刻（ms）の色。FULL COMBO は黄・白・水色の 3 コマを 0.1 秒で 1 周する。W-MIX に PERFECT の絵は無く、FULL COMBO の絵で出る
uint32_t standard_lamp_color(int i, uint64_t ms) {
    static const uint32_t fixed[4] = {0xff2020, 0x80ff20, 0xffc000, 0xe0e0e0};
    static const uint32_t fc[3] = {0xffc000, 0xe0e0e0, 0x20e0ff};
    return i < 4 ? fixed[std::clamp(i, 0, 3)] : fc[(ms % 100) * 3 / 100];
}

// MOD の詳細の画面。GAS の順番（gauge_order）、カスタムゲージの一覧（gauges）、カスタムランプの一覧（lamps）を変える。
// 設定が 2 つ以上ある MOD は、上のボタンで page を切り替える。done は「戻る」（保存して一覧へ）
void draw_mod_detail(Gui& g, std::vector<ModManifest>& mods, ModManifest& m, const std::vector<std::string>& enabled, const std::string& lr2_root,
                     float X, float Y, int& page, int& sel, int& scroll, int& add_sel, const std::function<void()>& done) {
    // ほかの MOD のゲージ・ランプは、使っている（印を付けた）ものだけ出す
    auto in_use = [&](const ModManifest& o) { return &o == &m || std::find(enabled.begin(), enabled.end(), o.id) != enabled.end(); };
    resolve_gauge_lamps(mods);  // 名前（CUSTOM1 …）とランプを、いまの中身で引き直す
    g.label(X + 14, Y + 12, m.name + " の詳細");
    page = std::clamp(page, 0, std::max(0, static_cast<int>(m.settings.size()) - 1));
    if (m.settings.size() > 1)
        for (size_t i = 0; i < m.settings.size(); ++i)
            if (g.button(X + 324 + i * 84.0f, Y + 8, 80, 22, m.settings[i].name, static_cast<int>(i) != page)) {
                g.unfocus();
                page = static_cast<int>(i), sel = 0, scroll = 0;
            }
    if (m.settings.empty()) return;
    const ModSetting& st = m.settings[page];
    auto num = [&](double& v, float x, float y, float w) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        g.edit_box(&v, x, y, w, 20, buf, true, [&v](const std::string& s) {
            char* end = nullptr;
            double d = std::strtod(s.c_str(), &end);
            if (end && end != s.c_str()) v = d;
        });
    };
    // 新しい id（同じ MOD の中で重ならない）
    auto new_id = [](const std::string& head, auto& list) {
        for (int n = 1;; ++n) {
            std::string id = head + std::to_string(n);
            if (std::none_of(list.begin(), list.end(), [&](const auto& x) { return x.id == id; })) return id;
        }
    };
    if (st.type == "gauge_order") {
        // 選べるゲージ：本体の 6 つと、MOD のカスタムゲージ（鍵は「MOD/ゲージ」）
        std::vector<std::pair<std::string, std::string>> all;  // 鍵, 名前
        for (const char* k : kGaugeKeys) all.push_back({k, k});
        for (const ModManifest& o : mods)
            for (const CustomGaugeDef& cg : o.gauges) all.push_back({o.id + "/" + cg.id, cg.name});
        auto name_of = [&](const std::string& k) {
            for (const auto& [key, nm] : all)
                if (key == k) return nm;
            return k + "（無い）";
        };
        std::vector<std::string> rows;
        for (const std::string& k : m.gas_order) rows.push_back(name_of(k));
        g.label(X + 14, Y + 36, st.name);
        g.listview(X + 14, Y + 54, 260, 200, "上から順に落ちる", rows, sel, scroll);
        const int n = static_cast<int>(m.gas_order.size());
        const bool has = sel >= 0 && sel < n;
        if (g.button(X + 284, Y + 54, 80, 22, "上へ", has && sel > 0)) std::swap(m.gas_order[sel], m.gas_order[sel - 1]), --sel;
        if (g.button(X + 284, Y + 80, 80, 22, "下へ", has && sel + 1 < n)) std::swap(m.gas_order[sel], m.gas_order[sel + 1]), ++sel;
        if (g.button(X + 284, Y + 106, 80, 22, "外す", has && n > 1)) m.gas_order.erase(m.gas_order.begin() + sel), sel = std::min(sel, n - 2);
        std::vector<std::string> addable_keys, addable_names;
        for (const auto& [key, nm] : all)
            if (std::find(m.gas_order.begin(), m.gas_order.end(), key) == m.gas_order.end()) addable_keys.push_back(key), addable_names.push_back(nm);
        add_sel = std::clamp(add_sel, 0, std::max(0, static_cast<int>(addable_keys.size()) - 1));
        g.combo(&add_sel, X + 284, Y + 146, 170, 22, addable_names, add_sel, [&](int i) { add_sel = i; }, !addable_keys.empty());
        if (g.button(X + 460, Y + 146, 60, 22, "足す", !addable_keys.empty())) m.gas_order.push_back(addable_keys[add_sel]), sel = n;
    } else if (st.type == "gauges") {
        // 一覧の頭に見本（灰色。変えられない）：本体のゲージと、使っているほかの MOD のゲージ（EX-HARD など）。そのあとにこの MOD のゲージ
        static const std::vector<CustomGaugeDef> standard = standard_gauge_samples();
        std::vector<CustomGaugeDef> samples = standard;
        for (const ModManifest& o : mods)
            if (&o != &m && in_use(o))
                for (CustomGaugeDef og : o.gauges) {
                    // ランプの鍵は、この MOD から見た形（「MOD/id」）にする
                    const bool std_lamp = std::find(std::begin(kStandardLampKeys), std::end(kStandardLampKeys), og.lamp) != std::end(kStandardLampKeys);
                    if (!std_lamp && og.lamp.find('/') == std::string::npos) og.lamp = o.id + "/" + og.lamp;
                    samples.push_back(og);
                }
        const int ns = static_cast<int>(samples.size());
        std::vector<std::string> rows;
        for (const CustomGaugeDef& cg : samples) rows.push_back(cg.name);
        for (const CustomGaugeDef& cg : m.gauges) rows.push_back(cg.name);
        g.label(X + 14, Y + 36, st.name);
        g.listview(X + 14, Y + 54, 140, 230, "ゲージ", rows, sel, scroll, ns);
        bool sample = sel >= 0 && sel < ns;
        int mi = sel - ns;  // MOD のゲージの番号
        // 番号の付くゲージ（CUSTOM1 …）は、スキンに用意したコマの数まで
        const bool can_add = m.gauge_label.empty() || static_cast<int>(m.gauges.size()) < kMaxNumberedGauges;
        if (g.button(X + 14, Y + 290, 66, 22, "足す", can_add)) {
            g.unfocus();
            // 選んでいるもの（見本でも）を元にする。何も選んでいなければ HARD
            CustomGaugeDef ng = sample ? samples[sel] : mi >= 0 && mi < static_cast<int>(m.gauges.size()) ? m.gauges[mi] : samples[1];
            ng.id = new_id("gauge", m.gauges);
            ng.frame = -1;
            m.gauges.push_back(ng);
            sel = ns + static_cast<int>(m.gauges.size()) - 1;
        }
        bool has = mi >= 0 && mi < static_cast<int>(m.gauges.size());
        if (g.button(X + 88, Y + 290, 66, 22, "消す", has)) {
            g.unfocus();
            m.gauges.erase(m.gauges.begin() + mi);
            sel = std::max(0, std::min(sel, ns + static_cast<int>(m.gauges.size()) - 1));
        }
        // 足す・消すで選んでいる行が変わったので、引き直す
        sample = sel >= 0 && sel < ns;
        mi = sel - ns;
        has = mi >= 0 && mi < static_cast<int>(m.gauges.size());
        if (sample || has) {
            CustomGaugeDef ro_copy = sample ? samples[sel] : CustomGaugeDef{};
            CustomGaugeDef& cg = sample ? ro_copy : m.gauges[mi];
            const bool ed = !sample;  // 見本は見るだけ
            auto numf = [&](double& v, float x, float y, float w) {
                if (ed) return num(v, x, y, w);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", v);
                g.edit_box(&v, x, y, w, 20, buf, false, [](const std::string&) {});
            };
            const float x0 = X + 166, x1 = X + 230;
            g.label(x0, Y + 58, "名前");
            if (m.gauge_label.empty() && ed) {
                g.edit_str(cg.name, x1, Y + 54, 150);
                g.label(x0 + 240, Y + 58, "ID"), g.edit_str(cg.id, x1 + 196, Y + 54, 110);
            } else {
                g.label(x1, Y + 58, cg.name);
            }
            g.label(x0, Y + 84, "種類");
            g.combo(&cg.survival, x1, Y + 80, 150, 22, {"落ちたら終わり", "最後の値で決める"}, cg.survival ? 0 : 1,
                    [&cg](int i) { cg.survival = i == 0; }, ed);
            g.label(x0, Y + 110, "始まり"), numf(cg.initial, x1, Y + 106, 50);
            g.label(x1 + 60, Y + 110, "クリア", !cg.survival), numf(cg.border, x1 + 100, Y + 106, 50);
            g.label(x1 + 160, Y + 110, "下限", !cg.survival), numf(cg.floor, x1 + 190, Y + 106, 50);
            // 判定ごとの増減：決まった値と、TOTAL に比べた値（TOTAL ÷ 総ノーツ数 × この値）
            static const char* jn[6] = {"空POOR", "POOR", "BAD", "GOOD", "GREAT", "PGREAT"};
            g.label(x1, Y + 134, "増減"), g.label(x1 + 60, Y + 134, "TOTAL 比");
            g.label(x1 + 180, Y + 134, "増減"), g.label(x1 + 240, Y + 134, "TOTAL 比");
            for (int k = 0; k < 6; ++k) {
                const int jk = 5 - k;  // PGREAT から
                const float cx = k < 3 ? 0 : 180, cy = Y + 150 + (k % 3) * 24;
                g.label(x0 + cx, cy + 4, jn[jk]);
                numf(cg.delta[jk].fixed, x1 + cx, cy, 50);
                numf(cg.delta[jk].total, x1 + cx + 60, cy, 50);
            }
            g.label(x0, Y + 228, "地雷"), numf(cg.mine_mul, x1, Y + 224, 50);
            g.label(x1 + 60, Y + 228, "残り"), numf(cg.low_hp, x1 + 90, Y + 224, 40);
            g.label(x1 + 134, Y + 228, "以下で減り ×"), numf(cg.low_hp_mul, x1 + 214, Y + 224, 40);
            // クリアしたときに付くランプ：本体のランプ（ゲームの中の名前）と、カスタムランプ（ほかの MOD のものも選べる）。横に色の見本
            std::vector<std::string> lamp_keys, lamp_names;
            std::vector<uint32_t> lamp_cols;
            for (int k = 1; k < 4; ++k)
                lamp_keys.push_back(kStandardLampKeys[k]), lamp_names.push_back(kStandardLampNames[k]),
                    lamp_cols.push_back(m.standard_lamps[k].colors > 0 ? m.standard_lamps[k].color_at(SDL_GetTicks()) : standard_lamp_color(k, SDL_GetTicks()));
            if (cg.lamp == kStandardLampKeys[0]) lamp_keys.push_back(kStandardLampKeys[0]), lamp_names.push_back(kStandardLampNames[0]), lamp_cols.push_back(standard_lamp_color(0, SDL_GetTicks()));
            for (const CustomLampDef& l : m.lamps) lamp_keys.push_back(l.id), lamp_names.push_back(l.name), lamp_cols.push_back(l.color_at(SDL_GetTicks()));
            for (const ModManifest& o : mods)
                if (&o != &m && in_use(o))
                    for (const CustomLampDef& l : o.lamps)
                        lamp_keys.push_back(o.id + "/" + l.id), lamp_names.push_back(l.name + "（" + o.name + "）"),
                            lamp_cols.push_back([&] {
                                auto it = m.lamp_colors.find(o.id + "/" + l.id);  // ここで色を変えていればその色
                                return it != m.lamp_colors.end() && it->second.colors > 0 ? it->second.color_at(SDL_GetTicks()) : l.color_at(SDL_GetTicks());
                            }());
            int lamp_sel = -1;
            for (size_t i = 0; i < lamp_keys.size(); ++i)
                if (lamp_keys[i] == cg.lamp) lamp_sel = static_cast<int>(i);
            g.label(x0, Y + 254, "ランプ");
            g.combo(&cg.lamp, x1, Y + 250, 150, 22, lamp_names, lamp_sel, [&cg, lamp_keys](int i) { cg.lamp = lamp_keys[i]; }, ed);
            if (lamp_sel >= 0) g.fill({x1 + 156, Y + 252, 26, 18}, Gui::rgb(lamp_cols[lamp_sel])), g.frame({x1 + 156, Y + 252, 26, 18}, kDark);
            g.label(x1 + 188, Y + 254, "がクリアで付く");
            // ゲージの色：1 色か、80% 未満と 80% 以上の 2 色（落ちたら終わりのゲージは 1 色だけ）。四角を押すと色の見本が開く
            g.label(x0, Y + 280, "色");
            const bool two = cg.colors == 2 && !cg.survival;
            if (ed) {
                g.combo(&cg.colors, x1, Y + 276, 60, 22, {"1 色", "2 色"}, cg.colors == 2 ? 1 : 0, [&cg](int i) { cg.colors = i + 1; },
                        !cg.survival);
                // 帯と同じく、左が 80% 未満・右が 80% 以上
                if (two) {
                    g.label(x1 + 70, Y + 280, "80%未満"), g.color_button(&cg.color_low, x1 + 122, Y + 276, 36, 22);
                    g.label(x1 + 168, Y + 280, "80%以上"), g.color_button(&cg.color, x1 + 220, Y + 276, 36, 22);
                } else {
                    g.color_button(&cg.color, x1 + 70, Y + 276, 36, 22);
                }
            } else if (sel < static_cast<int>(standard.size())) {
                // 本体のゲージ：スキンの絵のままか、1 色・2 色に変える（遊ぶときのゲージもこの色になる）
                GaugeColors& gc = m.standard_gauges[sel];
                std::vector<std::string> opts = {"スキンの絵のまま", "1 色"};
                if (!cg.survival) opts.push_back("2 色");
                gc.colors = std::min(gc.colors, static_cast<int>(opts.size()) - 1);
                g.combo(&gc.colors, x1, Y + 276, 120, 22, opts, gc.colors, [&gc](int i) { gc.colors = i; });
                if (gc.colors == 2) {
                    g.label(x1 + 128, Y + 280, "80%未満"), g.color_button(&gc.color_low, x1 + 180, Y + 276, 36, 22);
                    g.label(x1 + 222, Y + 280, "80%以上"), g.color_button(&gc.color, x1 + 274, Y + 276, 36, 22);
                } else if (gc.colors == 1) {
                    g.color_button(&gc.color, x1 + 128, Y + 276, 36, 22);
                }
                if (gc.colors > 0)
                    for (int i = 0; i < 50; ++i) {
                        const uint32_t c = gc.colors == 2 && i < 40 ? gc.color_low : gc.color;
                        g.fill({x1 + i * 5.0f, Y + 304, 4, 12}, Gui::rgb(c));
                    }
            } else {
                // ほかの MOD のゲージ：その色を見せる（左が 80% 未満、右が 80% 以上）
                g.fill({x1, Y + 278, 36, 18}, Gui::rgb(two ? cg.color_low : cg.color)), g.frame({x1, Y + 278, 36, 18}, kDark);
                if (two) g.fill({x1 + 42, Y + 278, 36, 18}, Gui::rgb(cg.color)), g.frame({x1 + 42, Y + 278, 36, 18}, kDark);
            }
            // 見本の帯（遊ぶときのゲージと同じく 50 こまで、右の 10 こまが 80% 以上）
            if (ed)
                for (int i = 0; i < 50; ++i) {
                    const uint32_t c = two && i < 40 ? cg.color_low : cg.color;
                    g.fill({x1 + i * 5.0f, Y + 304, 4, 12}, SDL_Color{static_cast<Uint8>(c >> 16), static_cast<Uint8>(c >> 8), static_cast<Uint8>(c), 255});
                }
        }
    } else if (st.type == "lamps") {
        // 一覧の頭に見本（灰色）：本体のランプ（色だけ変えられる）と、使っているほかの MOD のランプ（EX-HARD など。見るだけ）。
        // そのあとにこの MOD のランプ
        static const std::vector<CustomLampDef> samples = standard_lamp_samples();
        const int nstd = static_cast<int>(samples.size());
        std::vector<std::pair<std::string, const CustomLampDef*>> others;  // 鍵（MOD/id）とランプ
        for (const ModManifest& o : mods)
            if (&o != &m && in_use(o))
                for (const CustomLampDef& l : o.lamps) others.push_back({o.id + "/" + l.id, &l});
        const int ns = nstd + static_cast<int>(others.size());
        std::vector<std::string> rows;
        for (int i = 0; i < nstd; ++i) rows.push_back(kStandardLampNames[i]);
        for (const auto& [key, l] : others) rows.push_back(l->name);
        for (const CustomLampDef& l : m.lamps) rows.push_back(l.name);
        g.label(X + 14, Y + 36, st.name);
        g.listview(X + 14, Y + 54, 140, 230, "ランプ", rows, sel, scroll, ns);
        int mi = sel - ns;
        bool has = mi >= 0 && mi < static_cast<int>(m.lamps.size());
        if (g.button(X + 14, Y + 290, 66, 22, "足す")) {
            g.unfocus();
            // 選んでいるもの（ほかの MOD のランプでも）を元にする
            const CustomLampDef* base = has ? &m.lamps[mi] : sel >= nstd && sel < ns ? others[sel - nstd].second : nullptr;
            CustomLampDef nl = base ? *base : CustomLampDef{};
            nl.id = new_id("lamp", m.lamps);
            if (!base) nl.name = "NEW LAMP", nl.rank = 4.5;
            m.lamps.push_back(nl);
            sel = ns + static_cast<int>(m.lamps.size()) - 1;
        }
        if (g.button(X + 88, Y + 290, 66, 22, "消す", has)) {
            g.unfocus();
            m.lamps.erase(m.lamps.begin() + mi);
            sel = std::max(0, std::min(sel, ns + static_cast<int>(m.lamps.size()) - 1));
        }
        // 足す・消すで選んでいる行が変わったので、引き直す
        mi = sel - ns;
        has = mi >= 0 && mi < static_cast<int>(m.lamps.size());
        const uint64_t now = SDL_GetTicks();
        const float x0 = X + 166, x1 = X + 214;
        // 本体のランプのいまの色（色を変えていればその色、無ければスキンの絵の色）
        auto std_color = [&](int i) {
            const CustomLampDef& o = m.standard_lamps[i];
            return o.colors > 0 ? o.color_at(static_cast<double>(now)) : standard_lamp_color(i, now);
        };
        // ほかの MOD のランプのいまの色（ここで変えていればその色）
        auto other_color = [&](const std::string& key, const CustomLampDef& l) {
            auto it = m.lamp_colors.find(key);
            return it != m.lamp_colors.end() && it->second.colors > 0 ? it->second.color_at(static_cast<double>(now)) : l.color_at(static_cast<double>(now));
        };
        // 選曲画面のバーの見本（左端の帯がランプ）
        auto bar_sample = [&](float y, uint32_t c, const std::string& title) {
            g.fill({x1, y, 170, 22}, SDL_Color{40, 44, 60, 255});
            g.fill({x1 + 2, y + 2, 8, 18}, Gui::rgb(c));
            g.text->draw(x1 + 18, y + 5, g.font, SDL_Color{255, 255, 255, 255}, title, 0, 0, 0);
        };
        const CustomLampDef* cur = nullptr;  // いま選んでいるランプ（本体のものは見本）
        // 色のえらび方（本体のランプは「スキンの絵のまま」も選べる）、色、点滅の速さを 1 行ずつ
        // keep：「〜のまま」の選択肢の名前（空なら無し）。from：「〜のまま」から変えるときに写す色
        auto color_rows = [&](CustomLampDef& l, const std::string& keep, const CustomLampDef* from = nullptr) {
            g.label(x0, Y + 86, "色");
            if (!keep.empty())
                g.combo(&l.colors, x1, Y + 82, 120, 22, {keep, "1 色", "点滅"}, l.colors, [&l, from](int i) {
                    if (l.colors == 0 && i > 0 && from) l.color = from->color, l.color2 = from->color2, l.blink_ms = from->blink_ms;
                    l.colors = i;
                });
            else
                g.combo(&l.colors, x1, Y + 82, 120, 22, {"1 色", "点滅"}, l.colors == 2 ? 1 : 0, [&l](int i) { l.colors = i + 1; });
            if (l.colors >= 1) g.color_button(&l.color, x1, Y + 108, 36, 22);
            if (l.colors == 2) {
                g.label(x1 + 44, Y + 112, "と"), g.color_button(&l.color2, x1 + 62, Y + 108, 36, 22);
                static const int speeds[3] = {1000, 500, 250};
                int sp = l.blink_ms >= 750 ? 0 : l.blink_ms >= 375 ? 1 : 2;
                g.label(x0, Y + 138, "速さ");
                g.combo(&l.blink_ms, x1, Y + 134, 120, 22, {"ゆっくり", "ふつう", "速い"}, sp, [&l](int i) { l.blink_ms = speeds[i]; });
            }
        };
        if (sel >= 0 && sel < nstd) {
            g.label(x0, Y + 58, "名前"), g.label(x1, Y + 58, kStandardLampNames[sel]);
            color_rows(m.standard_lamps[sel], "スキンの絵のまま");
            g.label(x0, Y + 172, "見え方"), bar_sample(Y + 168, std_color(sel), "曲名");
            cur = &samples[sel];
        } else if (sel >= nstd && sel < ns) {
            // ほかの MOD のランプ（EX-HARD など）：名前と強さはその MOD のまま、色だけここで変えられる
            const auto& [key, l] = others[sel - nstd];
            CustomLampDef keep;
            keep.colors = 0;  // 初めは MOD の色のまま
            CustomLampDef& over = m.lamp_colors.try_emplace(key, keep).first->second;
            g.label(x0, Y + 58, "名前"), g.label(x1, Y + 58, l->name);
            color_rows(over, "MOD の色のまま", l);
            if (over.colors == 0) {
                g.fill({x1, Y + 110, 36, 18}, Gui::rgb(l->color)), g.frame({x1, Y + 110, 36, 18}, kDark);
                if (l->colors == 2) g.label(x1 + 44, Y + 112, "と"), g.fill({x1 + 62, Y + 110, 36, 18}, Gui::rgb(l->color2)), g.frame({x1 + 62, Y + 110, 36, 18}, kDark);
            }
            g.label(x0, Y + 172, "見え方"), bar_sample(Y + 168, other_color(key, *l), "曲名");
            cur = l;
        } else if (has) {
            CustomLampDef& l = m.lamps[mi];
            l.as = std::clamp(static_cast<int>(l.rank), 1, 4);  // 絵と記録は、すぐ下の本体のランプのもの
            g.label(x0, Y + 58, "名前"), g.edit_str(l.name, x1, Y + 54, 150);
            color_rows(l, "");
            g.label(x0, Y + 172, "見え方"), bar_sample(Y + 168, l.color_at(static_cast<double>(now)), "曲名");
            // このランプを付けるゲージ
            std::string users;
            for (const ModManifest& o : mods)
                for (const CustomGaugeDef& cg : o.gauges)
                    if (cg.lamp_key == m.id + "/" + l.id) users += (users.empty() ? "" : "、") + cg.name;
            // 長ければ「強い順」の一覧の手前で切る
            g.label(x0, Y + 204, "使うゲージ");
            const SDL_Rect clip{static_cast<int>(x1 + 30), static_cast<int>(Y + 200), static_cast<int>(X + 392 - x1 - 30), 20};
            SDL_SetRenderClipRect(g.r, &clip);
            g.label(x1 + 30, Y + 204, users.empty() ? "なし" : users);
            SDL_SetRenderClipRect(g.r, nullptr);
            cur = &l;
        }
        // 強い順の一覧（本体のランプと、使える全部のカスタムランプ）。同じ譜面にランプがいくつかあれば、上のものが出る
        struct Rung {
            std::string name;
            double rank;
            uint32_t color;
            const CustomLampDef* lamp;
        };
        std::vector<Rung> ladder;
        for (int i = 0; i < nstd; ++i) ladder.push_back({kStandardLampNames[i], samples[i].rank, std_color(i), &samples[i]});
        for (const ModManifest& o : mods)
            if (in_use(o))
                for (const CustomLampDef& l : o.lamps)
                    ladder.push_back({l.name, l.rank, &o == &m ? l.color_at(static_cast<double>(now)) : other_color(o.id + "/" + l.id, l), &l});
        std::stable_sort(ladder.begin(), ladder.end(), [](const Rung& a, const Rung& b) { return a.rank > b.rank; });
        const float lx = X + 400, ly = Y + 36;
        g.label(lx, ly, "強い順（上ほど強い）");
        g.sunken({lx, ly + 18, 164, 234}, kHi);
        int at = -1;
        for (size_t i = 0; i < ladder.size() && i < 14; ++i) {
            const float ry = ly + 20 + i * 16.0f;
            const bool me = ladder[i].lamp == cur;
            if (me) at = static_cast<int>(i), g.fill({lx + 2, ry, 160, 16}, kSel);
            g.fill({lx + 6, ry + 3, 10, 10}, Gui::rgb(ladder[i].color));
            g.text->draw(lx + 22, ry + 2, g.font, me ? SDL_Color{255, 255, 255, 255} : kText, ladder[i].name, 0, 0, 0);
        }
        // 上へ・下へ：FAILED より上、FULL COMBO より下で動かす。動かしたら、新しく隣になった 2 つの間の値にする
        const bool movable = has && at >= 0;
        const bool can_up = movable && at >= 1 && ladder[at - 1].rank < 5;
        const bool can_down = movable && at + 1 < static_cast<int>(ladder.size()) && ladder[at + 1].rank > 1;
        if (g.button(lx, Y + 290, 78, 22, "上へ", can_up)) {
            const double hi = at >= 2 ? ladder[at - 2].rank : ladder[at - 1].rank + 1;
            m.lamps[mi].rank = (ladder[at - 1].rank + hi) / 2;
        }
        if (g.button(lx + 86, Y + 290, 78, 22, "下へ", can_down)) {
            const double lo = at + 2 < static_cast<int>(ladder.size()) ? ladder[at + 2].rank : 0;
            m.lamps[mi].rank = (ladder[at + 1].rank + lo) / 2;
        }
    }
    if (st.type == "skin_colors") {
        // プレースキンの部品の色。左が当てるものの一覧（上から順に当てる）、右が選んでいるものの中身
        static const char* const part_keys[] = {"note", "ln", "mine", "bomb", "hold", "beam", "key", "judge", "combo", "judgeline", "line", "gauge", "frame"};
        static const char* const part_names[] = {"ノーツ", "LN", "地雷", "ボム", "LN のボム", "キービーム", "鍵盤", "判定の文字", "コンボ", "判定ライン", "小節線", "ゲージ", "フレーム"};
        static const char* const lane_keys[] = {"scratch", "white", "blue"};
        static const char* const lane_names[] = {"皿", "白鍵", "青鍵"};
        static const char* const judge_keys[] = {"pgreat", "great", "good", "bad", "poor", "empty_poor"};
        static const char* const judge_names[] = {"PGREAT", "GREAT", "GOOD", "BAD", "POOR", "空POOR"};
        static const char* const mode_keys[] = {"hue", "range", "map", "mul"};
        static const std::vector<std::string> mode_names = {"色相をずらす", "近い色だけずらす", "グラデーションマップ", "灰色にして掛ける"};
        auto has = [](const std::vector<std::string>& v, const char* k) { return std::find(v.begin(), v.end(), k) != v.end(); };
        // 一覧の行：部品の名前を「・」でつなぐ
        auto summary = [&](const SkinColorFilter& f) {
            std::string t;
            for (size_t i = 0; i < std::size(part_keys); ++i)
                if (has(f.parts, part_keys[i])) t += (t.empty() ? "" : "・") + std::string(part_names[i]);
            if (t.empty()) t = "（部品なし）";
            // スキンを決めていれば、そのファイルの名前も付ける
            if (!f.skins.empty()) t += " / " + fs::u8path(from_portable(f.skins[0])).stem().string();
            return t;
        };
        std::vector<std::string> rows;
        for (const SkinColorFilter& f : m.skin_colors) rows.push_back(summary(f));
        g.label(X + 14, Y + 36, st.name);
        g.listview(X + 14, Y + 54, 140, 230, "上から順に当てる", rows, sel, scroll);
        const int n = static_cast<int>(m.skin_colors.size());
        if (g.button(X + 14, Y + 290, 66, 22, "足す")) {
            g.unfocus();
            SkinColorFilter f;
            if (sel >= 0 && sel < n) f = m.skin_colors[sel];  // 選んでいるものを元にする
            else f.parts = {"note"}, f.hue = 120;
            m.skin_colors.push_back(f);
            sel = n;
        }
        if (g.button(X + 88, Y + 290, 66, 22, "消す", sel >= 0 && sel < n)) {
            g.unfocus();
            m.skin_colors.erase(m.skin_colors.begin() + sel);
            sel = std::min(sel, n - 2);
        }
        if (sel >= 0 && sel < static_cast<int>(m.skin_colors.size())) {
            SkinColorFilter& f = m.skin_colors[sel];
            const float x0 = X + 166, x1 = X + 220;
            // 印の付け外しで、一覧に足す・外す
            auto toggles = [&](std::vector<std::string>& v, const char* const* keys, const char* const* names, size_t cnt, float y, float w,
                               int per_row, bool enabled) {
                for (size_t i = 0; i < cnt; ++i) {
                    bool on = has(v, keys[i]);
                    if (g.check(x1 + (i % per_row) * w, y + (i / per_row) * 20.0f, names[i], on, enabled)) {
                        if (on) v.push_back(keys[i]);
                        else v.erase(std::remove(v.begin(), v.end(), keys[i]), v.end());
                    }
                }
            };
            // 効かせるスキン：全部か、1 つのプレースキン（7 鍵）
            {
                static SkinCatalog catalog;
                static std::string scanned;
                if (scanned != lr2_root) catalog.scan(lr2_root), scanned = lr2_root;
                std::vector<std::string> names = {"全部"}, paths = {""};
                if (auto it = catalog.entries().find(0); it != catalog.entries().end())
                    for (const SkinEntry& e : it->second)
                        names.push_back(e.header.title + " (" + fs::u8path(e.path).stem().string() + ")"), paths.push_back(to_portable(e.path));
                int cur = 0;
                if (!f.skins.empty())
                    for (size_t i = 1; i < paths.size(); ++i)
                        if (fs::u8path(from_portable(paths[i])).lexically_normal() == fs::u8path(from_portable(f.skins[0])).lexically_normal())
                            cur = static_cast<int>(i);
                g.label(x0, Y + 34, "スキン");
                g.combo(&f.skins, x1, Y + 30, 340, 22, names, cur, [&f, paths](int i) {
                    f.skins.clear();
                    if (!paths[i].empty()) f.skins.push_back(paths[i]);
                });
            }
            g.label(x0, Y + 58, "部品");
            toggles(f.parts, part_keys, part_names, std::size(part_keys), Y + 58, 84, 4, true);
            const bool lanes_on = has(f.parts, "note") || has(f.parts, "ln") || has(f.parts, "mine") || has(f.parts, "bomb") ||
                                  has(f.parts, "hold") || has(f.parts, "beam") || has(f.parts, "key");
            // 空の一覧は「全部」なので、全部に印を付けて見せる
            if (lanes_on && f.lanes.empty()) f.lanes.assign(std::begin(lane_keys), std::end(lane_keys));
            g.label2(x0, Y + 142, "レーン", lanes_on);
            toggles(f.lanes, lane_keys, lane_names, 3, Y + 142, 60, 3, lanes_on);
            const bool judges_on = has(f.parts, "judge") || has(f.parts, "combo");
            if (judges_on && f.judges.empty()) f.judges.assign(std::begin(judge_keys), std::end(judge_keys));
            g.label2(x0, Y + 164, "判定", judges_on);
            toggles(f.judges, judge_keys, judge_names, 6, Y + 164, 70, 5, judges_on);
            int mode = 0;
            for (int i = 0; i < 4; ++i)
                if (f.mode == mode_keys[i]) mode = i;
            g.label(x0, Y + 212, "方式");
            g.combo(&f.mode, x1, Y + 208, 150, 22, mode_names, mode, [&f](int i) { f.mode = mode_keys[i]; });
            // 方式ごとの値（色相は度、彩度・強さは %）
            const float py = Y + 238;
            if (mode <= 1) {
                float x = x1;
                if (mode == 1) {
                    g.label(x0, py + 4, "変える色"), num(f.from, x, py, 44), g.label(x + 48, py + 4, "度");
                    g.label(x + 70, py + 4, "幅"), num(f.width, x + 88, py, 40), x += 150;
                }
                g.label(mode == 1 ? x : x0, py + 4, mode == 1 ? "ずらす" : "色相");
                const float xx = mode == 1 ? x + 40 : x1;
                num(f.hue, xx, py, 44), g.label(xx + 48, py + 4, "度");
                g.label(x0, py + 30, "彩度"), num(f.sat, x1, py + 26, 44), g.label(x1 + 48, py + 30, "%");
                if (mode == 0) g.label(x1 + 74, py + 30, "明るさ"), num(f.lit, x1 + 120, py + 26, 44), g.label(x1 + 168, py + 30, "（-50〜50）");
            } else {
                g.label(x0, py + 4, "色"), g.color_button(&f.color, x1, py, 36, 22);
                g.label(x1 + 48, py + 4, "強さ"), num(f.mix, x1 + 82, py, 44), g.label(x1 + 130, py + 4, "%");
                // グラデーションマップのいちばん明るい所（100% なら白、下げると色に近づく。白い絵にも色を付けたいとき）
                if (mode == 2) g.label(x0, py + 30, "明るい所"), num(f.white, x1, py + 26, 44), g.label(x1 + 48, py + 30, "%");
            }
            // 見本：上が元の色（色相の帯と灰色の帯）、下が変えたあと
            const float sy = Y + 294, sw = 7;
            for (int i = 0; i < 48; ++i) {
                uint8_t px[4] = {0, 0, 0, 255};
                if (i < 36) {
                    // 色相の帯（鮮やかな色）
                    const double h = i * 10.0 / 60.0, c = 1, xk = c * (1 - std::fabs(std::fmod(h, 2.0) - 1));
                    double rgb[3] = {0, 0, 0};
                    const int hi = static_cast<int>(h) % 6;
                    const int ci[6][2] = {{0, 1}, {1, 0}, {1, 2}, {2, 1}, {2, 0}, {0, 2}};
                    rgb[ci[hi][0]] = c, rgb[ci[hi][1]] = xk;
                    for (int k = 0; k < 3; ++k) px[k] = static_cast<uint8_t>(rgb[k] * 255);
                } else {
                    px[0] = px[1] = px[2] = static_cast<uint8_t>((i - 36) * 255 / 11);
                }
                g.fill({x1 + i * sw, sy - 6, sw, 6}, SDL_Color{px[0], px[1], px[2], 255});
                recolor_pixel(px, f);
                g.fill({x1 + i * sw, sy, sw, 12}, SDL_Color{px[0], px[1], px[2], 255});
            }
            g.label(x0, sy - 2, "見本");
        }
    }
    if (st.type == "bgm") {
        // BGM のセット：セットごとに、選曲・決定の BGM の候補（手元のサウンドセットの中の BGM）をいくつでも入れる。
        // 使うセットの候補から、選曲画面に入るたびにランダムに 1 つずつ選ぶ
        static std::string listed;
        static std::vector<BgmChoice> sel_choices, dec_choices;
        static int edit = 0, sel_row[2] = {-1, -1}, sel_scroll[2] = {0, 0}, pick[2] = {0, 0};
        if (listed != lr2_root) {
            listed = lr2_root;
            sel_choices = bgm_choices(lr2_root, kSysSelect), dec_choices = bgm_choices(lr2_root, kSysDecide);
        }
        auto name_of = [](const std::vector<BgmChoice>& choices, const std::string& key) {
            for (const BgmChoice& c : choices)
                if (same_bgm_key(c.key, key)) return c.name;
            return key;  // 見つからないものはそのまま見せる
        };
        g.label(X + 14, Y + 36, st.name);
        // 使うセット（先頭は「使わない」＝サウンドセットのまま）
        std::vector<std::string> use_names = {"使わない（サウンドセットのまま）"};
        int use_cur = 0;
        for (size_t i = 0; i < m.bgm_sets.size(); ++i) {
            use_names.push_back(m.bgm_sets[i].name);
            if (m.bgm_sets[i].name == m.bgm_use) use_cur = static_cast<int>(i) + 1;
        }
        g.label(X + 14, Y + 64, "使うセット");
        g.combo(&m.bgm_use, X + 100, Y + 58, 260, 20, use_names, use_cur,
                [&m](int i) { m.bgm_use = i == 0 ? std::string() : m.bgm_sets[i - 1].name; });
        // 直すセット
        g.label(X + 14, Y + 94, "直すセット");
        edit = std::clamp(edit, 0, std::max(0, static_cast<int>(m.bgm_sets.size()) - 1));
        if (!m.bgm_sets.empty()) {
            std::vector<std::string> names;
            for (const auto& b : m.bgm_sets) names.push_back(b.name);
            g.combo(&edit, X + 100, Y + 88, 160, 20, names, edit, [](int i) { edit = i; });
        }
        if (g.button(X + 270, Y + 87, 60, 22, "追加")) {
            std::string name;
            for (int n = static_cast<int>(m.bgm_sets.size()) + 1;; ++n) {
                name = "SET" + std::to_string(n);
                if (std::none_of(m.bgm_sets.begin(), m.bgm_sets.end(), [&](const auto& b) { return b.name == name; })) break;
            }
            m.bgm_sets.push_back({name, {}, {}});
            edit = static_cast<int>(m.bgm_sets.size()) - 1;
        }
        if (m.bgm_sets.empty()) return;
        auto& set = m.bgm_sets[edit];
        if (g.button(X + 336, Y + 87, 60, 22, "削除")) {
            if (m.bgm_use == set.name) m.bgm_use.clear();
            m.bgm_sets.erase(m.bgm_sets.begin() + edit);
            edit = std::max(0, edit - 1);
            return;
        }
        // 名前（使うセットに選んでいれば、そちらの名前も直す）
        g.label(X + 410, Y + 94, "名前");
        const std::string before = set.name;
        g.edit_str(set.name, X + 446, Y + 88, 110, 20);
        if (set.name != before && m.bgm_use == before) m.bgm_use = set.name;
        // 選曲・決定の候補の一覧と、足す・外す
        auto column = [&](int k, float x, const char* title, std::vector<std::string>& list, const std::vector<BgmChoice>& choices) {
            std::vector<std::string> rows;
            for (const std::string& key : list) rows.push_back(name_of(choices, key));
            g.listview(x, Y + 120, 268, 150, std::string(title) + "（" + std::to_string(list.size()) + " 曲）", rows, sel_row[k], sel_scroll[k]);
            std::vector<std::string> names;
            for (const BgmChoice& c : choices) names.push_back(c.name);
            pick[k] = std::clamp(pick[k], 0, std::max(0, static_cast<int>(names.size()) - 1));
            if (!names.empty()) g.combo(&pick[k], x, Y + 276, 268, 20, names, pick[k], [k](int i) { pick[k] = i; });
            if (g.button(x, Y + 302, 80, 22, "足す", !choices.empty()) &&
                std::none_of(list.begin(), list.end(), [&](const std::string& key) { return same_bgm_key(key, choices[pick[k]].key); }))
                list.push_back(choices[pick[k]].key);
            const bool has = sel_row[k] >= 0 && sel_row[k] < static_cast<int>(list.size());
            if (g.button(x + 88, Y + 302, 80, 22, "外す", has)) list.erase(list.begin() + sel_row[k]), sel_row[k] = -1;
        };
        column(0, X + 14, "選曲の BGM", set.select, sel_choices);
        column(1, X + 290, "決定の BGM", set.decide, dec_choices);
    }
    if (st.type == "metronome") {
        // メトロノームの音量（0〜500%。曲の BGM の音量にも合わせて鳴る）
        g.label(X + 14, Y + 40, st.name);
        g.label(X + 40, Y + 70, "音量");
        g.edit_int(m.metronome_volume, X + 90, Y + 66, 60, 20, true, 0, kMaxMetronomeVolume);
        g.label(X + 156, Y + 70, "%");
    }
    if (st.type == "judges") {
        // 一覧の頭に本体の判定の見本（灰色。変えられない）、そのあとにこの MOD の判定
        static const std::vector<CustomJudgeDef> samples = standard_judge_samples();
        const int ns = static_cast<int>(samples.size());
        std::vector<std::string> rows;
        for (const CustomJudgeDef& j : samples) rows.push_back((j.id == m.judge_use ? "★ " : "") + j.name);
        for (size_t i = 0; i < m.judges.size(); ++i)
            rows.push_back((m.judges[i].id == m.judge_use ? "★ " : "") + std::string("JUDGE") + std::to_string(i + 1) + " " + m.judges[i].name);
        g.label(X + 14, Y + 36, st.name);
        // 遊ぶ判定（選ばなければ判定は変わらず、記録も分けない）
        {
            // 並びは選曲画面の判定の欄と同じ：使わない（OFF）・本体の判定・JUDGE1〜
            std::vector<std::string> names = {"使わない"}, ids = {""};
            for (const char* k : kStandardJudgeIds) names.push_back(k), ids.push_back(k);
            for (size_t i = 0; i < m.judges.size(); ++i)
                names.push_back("JUDGE" + std::to_string(i + 1) + " " + m.judges[i].name), ids.push_back(m.judges[i].id);
            int use = 0;
            for (size_t i = 0; i < ids.size(); ++i)
                if (ids[i] == m.judge_use) use = static_cast<int>(i);
            g.label(X + 270, Y + 34, "遊ぶ判定");
            g.combo(&m.judge_use, X + 334, Y + 30, 200, 22, names, use, [&m, ids](int i) { m.judge_use = ids[i]; });
        }
        g.listview(X + 14, Y + 54, 140, 230, "判定", rows, sel, scroll, ns);
        int mi = sel - ns;
        // 選曲画面の判定の欄に出せるのは JUDGE1〜9（スキンに用意したコマの数）まで
        if (g.button(X + 14, Y + 290, 66, 22, "足す", m.judges.size() < 9)) {
            g.unfocus();
            // 選んでいるもの（見本でも）を元にする。何も選んでいなければ #RANK どおり
            CustomJudgeDef nj = sel >= 0 && sel < ns ? samples[sel] : mi >= 0 && mi < static_cast<int>(m.judges.size()) ? m.judges[mi] : samples[0];
            nj.id = new_id("judge", m.judges);
            if (sel >= 0 && sel < ns) nj.name = std::string(samples[sel].name) + " の改造";
            m.judges.push_back(nj);
            sel = ns + static_cast<int>(m.judges.size()) - 1;
        }
        if (g.button(X + 88, Y + 290, 66, 22, "消す", mi >= 0 && mi < static_cast<int>(m.judges.size()))) {
            g.unfocus();
            if (m.judges[mi].id == m.judge_use) m.judge_use.clear();
            m.judges.erase(m.judges.begin() + mi);
            sel = std::max(0, std::min(sel, ns + static_cast<int>(m.judges.size()) - 1));
        }
        // 足す・消すで選んでいる行が変わったので、引き直す
        mi = sel - ns;
        const bool sample = sel >= 0 && sel < ns, has = mi >= 0 && mi < static_cast<int>(m.judges.size());
        if (sample || has) {
            CustomJudgeDef ro = sample ? samples[sel] : CustomJudgeDef{};
            CustomJudgeDef& j = sample ? ro : m.judges[mi];
            const bool ed = !sample;
            const float x0 = X + 166, x1 = X + 250;
            g.label(x0, Y + 62, "名前");
            if (ed) g.edit_str(j.name, x1, Y + 58, 180);
            else g.label(x1, Y + 62, j.name);
            g.label(x1, Y + 88, "早い側"), g.label(x1 + 70, Y + 88, "遅い側"), g.label(x1 + 140, Y + 88, "（ms）");
            static const char* names[4] = {"PGREAT", "GREAT", "GOOD", "BAD"};
            int* w[4] = {j.pgreat, j.great, j.good, j.bad};
            for (int k = 0; k < 4; ++k) {
                const float y = Y + 104 + k * 24;
                g.label(x0, y + 4, names[k]);
                g.edit_int(w[k][0], x1, y, 60, 20, ed, 0, 1000);
                g.edit_int(w[k][1], x1 + 70, y, 60, 20, ed, 0, 1000);
            }
            g.label(x0, Y + 206, "空POOR"), g.edit_int(j.empty_poor, x1, Y + 202, 60, 20, ed, 0, 5000);
            g.label(x1 + 70, Y + 206, "（早い側だけ）");
            g.check(x0, Y + 232, "#RANK に合わせて伸び縮み（NORMAL の幅で書く）", j.rank_scaled, ed);
            // 幅の図：真ん中がちょうど、左が早い・右が遅い。外から BAD・GOOD・GREAT・PGREAT
            const float cx = X + 166 + 190, sc = 190.0f / 220.0f, by = Y + 262;
            g.fill({cx - 190, by, 380, 16}, SDL_Color{40, 44, 60, 255});
            const SDL_Color cols[4] = {{255, 255, 255, 255}, {255, 210, 60, 255}, {90, 200, 90, 255}, {150, 90, 200, 255}};
            for (int k = 3; k >= 0; --k) {
                const float l = std::min(w[k][0], 220) * sc, r = std::min(w[k][1], 220) * sc;
                g.fill({cx - l, by + 2, l + r, 12}, cols[k]);
            }
            g.line(cx, by - 2, cx, by + 18, kText);
            g.label(cx - 190, by + 20, "← 早い"), g.label(cx + 190, by + 20, "遅い →", true, 2);
            // 色の意味
            static const char* legend[4] = {"PGREAT", "GREAT", "GOOD", "BAD"};
            float lx = cx - 120;
            for (int k = 0; k < 4; ++k) {
                g.fill({lx, by + 22, 10, 10}, cols[k]), g.frame({lx, by + 22, 10, 10}, kDark);
                lx += 14 + g.label(lx + 14, by + 20, legend[k]) + 8;
            }
        }
    }
    if (g.button(X + 484, Y + 312, 80, 24, "戻る")) {
        g.unfocus();
        done();
    }
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDLの初期化に失敗: %s\n", SDL_GetError());
        return 1;
    }
    AppConfig cfg;
    const std::string cfg_path = config_path();
    std::error_code first_ec;
    // 初めての起動（設定ファイルがまだ無い）なら、案内の画面から始める
    const bool first_run = !fs::exists(fs::u8path(cfg_path), first_ec);
    load_config(cfg);
    // ログを残し、前に残したログを送る。前の更新でどけた古いファイルを消す
#ifdef RD_DEV
    if (!output_is_redirected())
#endif
        start_log("launcher");
    remove_old_update_files();
    upload_logs_async(cfg);
    // 譜面を貸す設定（変わったら、保存するときに配信を登録し直す）
    auto serve_state = [](const AppConfig& c) { return std::make_tuple(c.serve, c.serve_port, c.serve_key, c.serve_public); };
    auto serve_applied = serve_state(cfg);

    // 窓は大きさを変えられる（中身は縦横の比を保って拡大する）。画面の拡大率（Windows の表示スケール）の分だけ大きく開く
    SDL_Window* window = SDL_CreateWindow("RD SETUP", kW, kH, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (window) {
        // 窓の座標が実際のピクセルの OS（Windows）では、表示スケールの分だけ大きくする（macOS は OS が拡大する）
        float content = SDL_GetWindowDisplayScale(window) / std::max(0.01f, SDL_GetWindowPixelDensity(window));
        if (const char* z = dev_env("RD_LAUNCHER_ZOOM")) content = static_cast<float>(std::atof(z));  // 開発用：この倍率で開く
        if (content > 1.01f) {
            SDL_SetWindowSize(window, static_cast<int>(kW * content), static_cast<int>(kH * content));
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        SDL_SetWindowMinimumSize(window, kW / 2, kH / 2);
        SDL_ShowWindow(window);
    }
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

    // RD に無い項目の見た目だけの値（LR2 SETUP の既定に合わせる）
    bool d_false = false, d_true = true;
    int d_flash = 0, d_buffer = 128;
    std::string d_font = "HG丸ｺﾞｼｯｸM-PRO";

    int tab = 0;
    // MOD（仕様書14章）。公式のあとに利用者のもの
    std::vector<std::string> mod_errors;
    std::vector<ModManifest> mods = scan_mods(official_mods_dir(), true, &mod_errors);  // 公式の MOD だけ
    // 詳細の設定（GAS の順番・カスタムゲージ）を重ねる
    for (ModManifest& m : mods) apply_mod_settings(m, mod_settings_path(m.id));
    resolve_gauge_lamps(mods);
    // 詳細の設定は、変えたらすぐ書く（「戻る」を押さずに閉じたり起動したりしても消えないように）。
    // 書いた中身を覚えておき、毎フレーム比べて変わっていたら書く
    std::map<std::string, std::string> saved_settings;
    for (const ModManifest& m : mods)
        if (!m.settings.empty()) saved_settings[m.id] = mod_settings_json(m);
    auto save_changed_settings = [&] {
        for (const ModManifest& m : mods) {
            if (m.settings.empty()) continue;
            std::string j = mod_settings_json(m);
            if (j == saved_settings[m.id]) continue;
            if (save_mod_settings(m, mod_settings_path(m.id))) saved_settings[m.id] = std::move(j);
            else std::printf("MOD の設定を書けない: %s\n", mod_settings_path(m.id).c_str());
        }
    };
    int mod_sel = mods.empty() ? -1 : 0, mod_scroll = 0;
    // プレイヤーの作成・削除（PLAYER の欄）
    bool new_player = false, delete_armed = false;
    std::string new_player_id;
    // MOD の詳細の画面（-1 なら一覧）。det_sel は詳細の中の一覧で選んでいる行
    int mod_detail = -1, det_page = 0, det_sel = 0, det_scroll = 0, det_add = 0;
    if (const char* d = dev_env("RD_LAUNCHER_DETAIL"))  // 開発用：その MOD の詳細を開いておく
        for (size_t i = 0; i < mods.size(); ++i)
            if (mods[i].id == d) mod_detail = mod_sel = static_cast<int>(i);
    if (const char* d = dev_env("RD_LAUNCHER_PAGE")) det_page = std::atoi(d);  // 開発用：詳細の何枚目を開くか
    if (const char* d = dev_env("RD_LAUNCHER_ROW")) det_sel = std::atoi(d);    // 開発用：詳細の一覧の何行目を選ぶか
    // プレイヤーを切り替える：今の人の設定を書いてから、その人の設定と MOD の詳細の設定を読み直す
    auto switch_player = [&](const std::string& id) {
        save_changed_settings();
        save_config(cfg);
        AppConfig next;
        load_config(next, id);
        cfg = next;
        save_config(cfg);  // 共通の設定に、今のプレイヤーを書く
        mods = scan_mods(official_mods_dir(), true, &mod_errors);
        for (ModManifest& m : mods) apply_mod_settings(m, mod_settings_path(m.id));
        resolve_gauge_lamps(mods);
        saved_settings.clear();
        for (const ModManifest& m : mods)
            if (!m.settings.empty()) saved_settings[m.id] = mod_settings_json(m);
        mod_detail = -1;
    };
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
    // 新しい版を確かめて入れ替える（裏で動かす。入れ替えている間は起動させない）
    struct UpdateJob {
        std::thread th;
        std::mutex mu;
        std::string status;
        std::string moved_to;  // 受け取り口の新しい住所
        bool running = false, done = false, ok = false;
        ~UpdateJob() {
            if (th.joinable()) th.join();
        }
    } update;
    if (cfg.auto_update) {
        update.running = true;
        update.th = std::thread([&update, c = cfg] {
            UpdateInfo u = check_update(c);
            std::string err;
            bool ok = false;
            if (u.available) {
                std::printf("新しい版 %s がある\n", u.version.c_str());
                ok = apply_update(u, [&update](const std::string& s) {
                    std::lock_guard<std::mutex> lock(update.mu);
                    update.status = s;
                }, err);
                if (!ok) std::printf("更新できない: %s\n", err.c_str());
            } else if (!u.error.empty()) {
                std::printf("新しい版を確かめられない: %s\n", u.error.c_str());
            }
            std::lock_guard<std::mutex> lock(update.mu);
            update.ok = ok;
            update.moved_to = u.moved_to;
            update.status = ok ? "新しい版 " + u.version + " に入れ替えました。起動し直します" : u.available ? "更新できません：" + err : "";
            update.done = true;
        });
    }
    // 借りる先につながるか試す（裏で動かす）
    struct BorrowTest {
        std::thread th;
        std::mutex mu;
        std::string result;
        bool running = false, done = false;
        ~BorrowTest() {
            if (th.joinable()) th.join();
        }
    } borrow_test;
    bool quit = false, start = false;
    bool autostart = dev_env("RD_LAUNCHER_AUTOSTART") != nullptr;  // 開発用：すぐ起動する
    // LR2 から引き継ぐ（RD タブのボタンと、初めての起動の案内から）
    auto start_lr2_import = [&](const std::string& dir) {
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
            // 素材（スキン・サウンドセット・BGM など）も RD のフォルダ（実行ファイルの横）に写し、そこから読む
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
    };
    if (const char* d = dev_env("RD_LAUNCHER_IMPORT")) start_lr2_import(d);  // 開発用：この LR2 のフォルダから引き継ぐ
    // 初めての起動の案内（-1 なら出さない。0 = 始め方を選ぶ 1 = 曲フォルダ 2 = 難易度表）
    int guide = first_run ? 0 : -1;
    if (const char* gs = dev_env("RD_LAUNCHER_GUIDE")) guide = std::atoi(gs);  // 開発用：案内の画面を出す

    while (!quit) {
        // 字は実際に映る大きさで作る（拡大してもつぶれない・ぼやけない）。作り直すと全部の字を作るので重い。
        // 窓の大きさを変えている間は作り直さず（今の字を拡大して描く）、大きさが 0.3 秒変わらなくなってから、0.25 刻みの倍率で 1 回だけ作る
        {
            static int last_w = 0, last_h = 0;
            static Uint64 changed_at = 0;
            int ow = 0, oh = 0;
            SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
            if (ow != last_w || oh != last_h) last_w = ow, last_h = oh, changed_at = SDL_GetTicks();
            if (ow > 0 && oh > 0 && SDL_GetTicks() - changed_at >= 300) {
                const float s = std::min(static_cast<float>(ow) / kW, static_cast<float>(oh) / kH);
                text.set_scale(std::max(1.0f, std::round(s * 4.0f) / 4.0f));
            }
        }
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
        // 開発用：RD_LAUNCHER_CLICKS の「x,y;x,y;…」（/ でも区切れる）を、5 フレーム目から 3 フレームごとに 1 つずつ押したことにする
        static std::vector<std::pair<float, float>> dev_clicks = [] {
            std::vector<std::pair<float, float>> v;
            if (const char* c = dev_env("RD_LAUNCHER_CLICKS"))
                for (const char* q = c; *q;) {
                    float x = 0, y = 0;
                    int n = 0;
                    if (std::sscanf(q, "%f,%f%n", &x, &y, &n) != 2) break;
                    v.push_back({x, y});
                    q += n;
                    if (*q == ';' || *q == '/') ++q;  // Windows の起動の口では ; が使えないので / でも区切れる
                }
            return v;
        }();
        static int dev_frame = 0;
        if (++dev_frame >= 5 && (dev_frame - 5) % 3 == 0 && static_cast<size_t>((dev_frame - 5) / 3) < dev_clicks.size()) {
            const auto [cx, cy] = dev_clicks[(dev_frame - 5) / 3];
            g.mx = cx, g.my = cy, g.pressed = true;
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
        static const char* tabs[] = {"MAIN", "JUKEBOX1", "JUKEBOX2", "OPTION", "SELECT", "SYSTEM", "RD", "PLAY", "MOD", "SHARE"};
        const SDL_FRect page{10, 56, 590, 368};
        g.fill(page, kFace);
        g.raised(page);
        float tx = 12;
        for (int i = 0; i < 10; ++i) {
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
            if (guide < 0 && g.clicked(rc)) tab = i;
            tx += w;
        }

        const float X = page.x, Y = page.y;  // ページの左上（以下の座標は LR2 SETUP の画面に合わせてある）
        if (guide >= 0) {
            // 初めての起動の案内。タブの上に重ねて出し、終わったら普通の設定の画面に戻る
            g.fill({page.x - 2, 36, page.w + 4, page.h + 22}, kFace);
            g.raised({page.x, 40, page.w, page.h + 16});
            const float gx = X + 24, gy = 56;
            if (guide == 0) {
                g.label(gx, gy, "RD へようこそ");
                g.label(gx, gy + 30, "はじめに、遊ぶための準備をします。");
                g.label(gx, gy + 50, "LR2 を使っていたなら、曲フォルダ・記録・スキン・設定をまとめて引き継げます。");
                if (g.button(gx, gy + 90, 240, 30, "LR2 から引き継ぐ", !job.running))
                    pick_folder(window, [&](const std::string& dir) {
                        start_lr2_import(dir);
                        guide = -1, tab = 6;  // 引き継ぎの進み具合は RD タブの下に出る
                    });
                g.label(gx + 256, gy + 99, "LR2 のフォルダ（LR2body.exe がある所）を選びます");
                if (g.button(gx, gy + 136, 240, 30, "新しく始める")) guide = 1;
                g.label(gx + 256, gy + 145, "曲フォルダと難易度表を登録します");
                if (g.button(gx, gy + 182, 240, 30, "あとで設定する")) guide = -1;
                g.label(gx + 256, gy + 191, "いつもの設定の画面へ進みます");
            } else if (guide == 1) {
                g.label(gx, gy, "1 / 2　曲フォルダ");
                g.label(gx, gy + 24, "BMS の曲が入っているフォルダを登録します。フォルダをここへドロップしても登録できます。");
                g.listview(gx, gy + 50, 530, 200, "path", cfg.song_dirs, song_sel, song_scroll);
                if (g.button(gx, gy + 260, 74, 22, "追加"))
                    pick_folder(window, [&](const std::string& p) {
                        if (std::find(cfg.song_dirs.begin(), cfg.song_dirs.end(), p) == cfg.song_dirs.end()) cfg.song_dirs.push_back(p);
                    });
                bool has = song_sel >= 0 && song_sel < static_cast<int>(cfg.song_dirs.size());
                if (g.button(gx + 84, gy + 260, 74, 22, "登録解除", has)) cfg.song_dirs.erase(cfg.song_dirs.begin() + song_sel), song_sel = -1;
                if (g.button(gx + 376, gy + 300, 74, 24, "戻る")) guide = 0;
                if (g.button(gx + 456, gy + 300, 74, 24, "次へ", !cfg.song_dirs.empty())) guide = 2;
            } else {
                g.label(gx, gy, "2 / 2　難易度表");
                g.label(gx, gy + 24, "使いたい難易度表の URL を登録します（あとから RD タブでも登録できます）。");
                g.listview(gx, gy + 50, 530, 170, "url", cfg.tables, table_sel, table_scroll);
                g.edit_str(table_input, gx, gy + 230, 380, 20);
                if (g.button(gx + 390, gy + 229, 64, 22, "追加", !table_input.empty())) {
                    if (std::find(cfg.tables.begin(), cfg.tables.end(), table_input) == cfg.tables.end()) cfg.tables.push_back(table_input);
                    table_input.clear();
                }
                bool has = table_sel >= 0 && table_sel < static_cast<int>(cfg.tables.size());
                if (g.button(gx + 462, gy + 229, 68, 22, "登録解除", has)) cfg.tables.erase(cfg.tables.begin() + table_sel), table_sel = -1;
                if (g.button(gx + 376, gy + 300, 74, 24, "戻る")) guide = 1;
                if (g.button(gx + 456, gy + 300, 74, 24, "完了")) {
                    guide = -1;
                    save_config(cfg);
                    message = "準備ができました。「RDの起動」で始めると、最初に曲フォルダを読み込みます";
                }
            }
        } else if (tab == 0) {
            // LR2 SETUP（100201）の MAIN と同じ並び。プレイヤーと IR は RD に無いので灰色
            // プレイヤー（LR2 SETUP と同じ並び）。スコア・オプション・キー・スキン・MOD はプレイヤーごと。パスワードは IR 用なので灰色
            g.group(X + 9, Y + 9, 556, 99, "PLAYER");
            g.label(X + 26, Y + 40, "PLAYER ID");
            const std::vector<std::string> players = list_players();
            if (new_player) {
                g.edit_str(new_player_id, X + 90, Y + 33, 142, 21);
                if (g.button(X + 251, Y + 32, 80, 23, "作成", !new_player_id.empty())) {
                    std::string err;
                    if (create_player(new_player_id, cfg.player, &err)) {
                        switch_player(new_player_id);
                        message = "プレイヤー " + new_player_id + " を作りました（設定とキーは前のプレイヤーのものを写しました）";
                        new_player = false;
                    } else {
                        message = err;
                    }
                }
                if (g.button(X + 340, Y + 32, 80, 23, "やめる")) new_player = false;
            } else {
                int cur = 0;
                for (size_t i = 0; i < players.size(); ++i)
                    if (players[i] == cfg.player) cur = static_cast<int>(i);
                // 選んだときの処理はフレームの終わりに呼ばれるので、一覧は写して持たせる
                g.combo(&cfg.player, X + 90, Y + 33, 142, 19, players, cur, [&, players](int i) {
                    if (players[i] != cfg.player) switch_player(players[i]), delete_armed = false;
                });
                g.label(X + 251, Y + 40, "PASSWORD");
                std::string pw;
                g.edit_str(pw, X + 324, Y + 33, 107, 21, false);
            }
            if (g.button(X + 27, Y + 72, 143, 23, "新規プレイヤーデータ作成", !new_player)) new_player = true, new_player_id.clear(), delete_armed = false;
            // 消すのは 2 回押したとき。最後の 1 人は消せない
            if (g.button(X + 189, Y + 72, 143, 23, delete_armed ? "本当に消す" : "プレイヤーデータ削除", !new_player && players.size() > 1)) {
                if (!delete_armed) {
                    delete_armed = true;
                    message = "もう一度押すと、プレイヤー " + cfg.player + " の記録と設定を消します";
                } else {
                    const std::string gone = cfg.player;
                    std::string next;
                    for (const std::string& p : players)
                        if (p != gone && next.empty()) next = p;
                    switch_player(next);
                    std::string err;
                    message = delete_player(gone, &err) ? "プレイヤー " + gone + " を消しました" : err;
                    delete_armed = false;
                }
            }
            bool windowed = !cfg.fullscreen;
            if (g.check(X + 19, Y + 132, "ウインドウモード", windowed)) cfg.fullscreen = !windowed;
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
            // RD だけの設定（LR2 SETUP には無い）
            if (g.button(X + 20, Y + 10, 180, 22, "LR2 から引き継ぐ", !job.running)) {
                pick_folder(window, start_lr2_import);
            }
            if (g.button(X + 206, Y + 10, 180, 22, "曲の一覧を作り直す")) {
                SongDb db;
                message = db.open(song_db_path()) && db.clear_charts() ? "次に起動したときに曲の一覧を作り直します"
                                                                      : "曲の一覧を開けません";
            }
            g.check(X + 20, Y + 42, "曲フォルダに入れた曲を自動で読み込む", cfg.watch_folders);
            g.label(X + 20, Y + 72, "スキンと音のフォルダ");
            g.edit_str(cfg.lr2_root, X + 164, Y + 66, 320, 20);
            if (g.button(X + 492, Y + 65, 74, 22, "参照"))
                pick_folder(window, [&](const std::string& p) { cfg.lr2_root = p; }, cfg.lr2_root.empty() ? base_dir() : cfg.lr2_root);
            g.label(X + 20, Y + 100, "スキンの解像度");
            static const std::vector<std::string> res_names = {"スキンに合わせる", "SD（640×480）", "HD（1280×720）", "FHD（1920×1080）"};
            static const char* res_vals[] = {"", "sd", "hd", "fhd"};
            int ri = 0;
            while (ri < 4 && cfg.skin_res != res_vals[ri]) ++ri;
            g.combo(&cfg.skin_res, X + 164, Y + 94, 140, 20, res_names, ri == 4 ? 0 : ri, [&](int i) { cfg.skin_res = res_vals[i]; });
            g.label(X + 316, Y + 100, "描画の上限（fps）");
            static const int fps_vals[] = {0, -1, 60, 120, 144, 240, 360, 480, 1000};
            static const std::vector<std::string> fps_names = {"モニタに合わせる", "上限なし", "60", "120", "144", "240", "360", "480", "1000"};
            int fi = 0;
            while (fi < 9 && fps_vals[fi] != cfg.fps) ++fi;
            g.combo(&cfg.fps, X + 426, Y + 94, 140, 20, fps_names, fi == 9 ? 0 : fi, [&](int i) { cfg.fps = fps_vals[i]; });
            // スキンはゲームの中のスキンセレクトで選ぶ（LR2 と同じ）。スキンセレクトの画面のスキンだけはここで選ぶ
            //（LR2 標準のスキンセレクトには、スキンセレクトのスキンを替えるボタンが無いので）
            {
                static SkinCatalog catalog;
                static std::string scanned;
                const std::string root = cfg.lr2_root.empty() ? base_dir() : from_portable(cfg.lr2_root);
                if (scanned != root) catalog.scan(root), scanned = root;
                std::vector<std::string> names = {"LR2 の既定"}, paths = {""};
                if (auto it = catalog.entries().find(9); it != catalog.entries().end())
                    for (const SkinEntry& e : it->second)
                        names.push_back(e.header.title + " (" + fs::u8path(e.path).stem().string() + ")"), paths.push_back(e.path);
                int cur = 0;
                for (size_t i = 1; i < paths.size(); ++i)
                    if (!cfg.skinselect_skin.empty() &&
                        fs::u8path(paths[i]).lexically_normal() == fs::u8path(cfg.skinselect_skin).lexically_normal())
                        cur = static_cast<int>(i);
                g.label(X + 20, Y + 128, "スキン選択画面のスキン");
                g.combo(&cfg.skinselect_skin, X + 164, Y + 122, 300, 20, names, cur, [&cfg, paths](int i) { cfg.skinselect_skin = paths[i]; });
            }
            float y = Y + 148;
            g.label(X + 20, y + 10, "難易度表の URL");
            g.listview(X + 20, y + 26, 546, 76, "url", cfg.tables, table_sel, table_scroll);
            float by = y + 108;
            g.edit_str(table_input, X + 20, by, 380, 20);
            if (g.button(X + 410, by - 1, 74, 22, "追加", !table_input.empty())) {
                if (std::find(cfg.tables.begin(), cfg.tables.end(), table_input) == cfg.tables.end()) cfg.tables.push_back(table_input);
                table_input.clear();
            }
            bool has = table_sel >= 0 && table_sel < static_cast<int>(cfg.tables.size());
            if (g.button(X + 492, by - 1, 74, 22, "登録解除", has)) cfg.tables.erase(cfg.tables.begin() + table_sel), table_sel = -1;
            g.check(X + 20, by + 28, "選曲中に曲のプレビューを鳴らす", cfg.preview);
            g.check(X + 20, by + 50, "キー音を軽く読み込む（16 ビット）", cfg.sound_16bit);
            g.check(X + 300, by + 50, "メモリを節約する", cfg.low_memory);
            g.check(X + 20, by + 72, "ログを作者へ送る", cfg.log_upload);
            g.check(X + 300, by + 72, "自動で新しい版に更新する", cfg.auto_update);
            g.check(X + 20, by + 94, "LR2 の形でも曲とスコアを書き出す（外部ツール用）", cfg.lr2_export);
#ifdef _WIN32
            g.check(X + 300, by + 28, "古いコントローラーも探す（DirectInput）", cfg.joystick_directinput);
#endif
        } else if (tab == 9) {
            // 譜面の貸し借り（仕様書10章「借りて返す」）。持っていない譜面を、遊ぶときだけ自分のほかの PC から借りる
            // 作りかけ（WIP）の間は、全部を灰色にして触れないようにする
            const bool on = kShareReady, serve_on = on && cfg.serve;
            if (!on) g.label(X + 520, Y + 16, "WIP");
            g.label(X + 20, Y + 16, "貸す（この PC の譜面を、ほかの PC に）", on);
            g.check(X + 34, Y + 40, "譜面を貸す", cfg.serve, on);
            g.check(X + 200, Y + 40, "インターネットからも借りられるようにする", cfg.serve_public, serve_on);
            g.label(X + 34, Y + 70, "ポート", serve_on);
            g.edit_int(cfg.serve_port, X + 124, Y + 64, 70, 20, serve_on, 1, 65535);
            g.label(X + 34, Y + 98, "合言葉", serve_on);
            g.edit_str(cfg.serve_key, X + 124, Y + 92, 240, 20, serve_on);
            if (g.button(X + 372, Y + 91, 60, 22, "作る", serve_on)) g.unfocus(), cfg.serve_key = make_serve_key();
            // 借りる側に入れる住所（Tailscale の名前から）
            static std::string ts_name;
            static bool ts_asked = false;
            if (!ts_asked && on) ts_asked = true, ts_name = tailscale_name();
            if (serve_on) {
                const std::string here = cfg.serve_public && !ts_name.empty()
                                             ? "https://" + ts_name + ":" + std::to_string(kFunnelPort)
                                             : "http://" + (ts_name.empty() ? std::string("<この PC の名前>") : ts_name) + ":" + std::to_string(cfg.serve_port);
                g.label(X + 34, Y + 126, "住所");
                g.label(X + 124, Y + 126, here);
                if (g.button(X + 488, Y + 119, 60, 22, "写す")) SDL_SetClipboardText(here.c_str()), message = "住所を写しました";
            }
            g.label(X + 20, Y + 160, "借りる（ほかの PC の譜面を）", on);
            g.label(X + 34, Y + 188, "借りる先", on);
            g.edit_str(cfg.borrow_url, X + 124, Y + 182, 300, 20, on);
            g.label(X + 34, Y + 216, "合言葉", on);
            g.edit_str(cfg.borrow_key, X + 124, Y + 210, 240, 20, on);
            g.label(X + 34, Y + 244, "置き場の上限（MB）", on);
            g.edit_int(cfg.borrow_cache_mb, X + 170, Y + 238, 90, 20, on, 100, 10000000);
            // つながるか試す（貸せる譜面の一覧を取ってみる）
            if (g.button(X + 432, Y + 181, 60, 22, "試す", on && !cfg.borrow_url.empty() && !borrow_test.running)) {
                g.unfocus();
                borrow_test.running = true, borrow_test.done = false;
                borrow_test.th = std::thread([&borrow_test, src = BorrowSource{cfg.borrow_url, cfg.borrow_key}] {
                    std::vector<std::pair<std::string, std::string>> list;
                    std::string err;
                    const bool ok = borrow_list(src, list, err);
                    std::lock_guard<std::mutex> lock(borrow_test.mu);
                    borrow_test.result = ok ? "つながりました（借りられる譜面 " + std::to_string(list.size()) + "）"
                                            : err.find("403") != std::string::npos ? "合言葉が違います"
                                                                                   : "つながりません：" + err;
                    borrow_test.done = true;
                });
                message = "借りる先につないでいます";
            }
        } else if (tab == 8 && mod_detail >= 0 && mod_detail < static_cast<int>(mods.size())) {
            draw_mod_detail(g, mods, mods[mod_detail], cfg.enabled_mods, cfg.lr2_root.empty() ? base_dir() : from_portable(cfg.lr2_root), X, Y, det_page, det_sel, det_scroll, det_add, [&] { mod_detail = -1; });
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
                std::string name = m.name + "  " + m.version + (m.affects_play() ? "  ＊記録を分ける" : "");
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
            if (mods.empty()) g.label(X + 20, y + 8, "MOD が見つかりません", false);
            y += rows * row_h + 14;
            if (mod_sel >= 0 && mod_sel < static_cast<int>(mods.size())) {
                const ModManifest& m = mods[mod_sel];
                if (!m.settings.empty() && g.button(X + 488, y - 4, 76, 22, "詳細")) mod_detail = mod_sel, det_page = 0, det_sel = 0, det_scroll = 0, det_add = 0;
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
                    // 引き継いだ設定を全部使う。素材を写せなかったときだけ、素材の場所は今のままにする
                    AppConfig n = job.cfg;
                    if (!job.assets.ok) {
                        n.lr2_root = cfg.lr2_root, n.play_skin = cfg.play_skin, n.result_skin = cfg.result_skin;
                        n.courseresult_skin = cfg.courseresult_skin, n.select_skin = cfg.select_skin, n.decide_skin = cfg.decide_skin;
                        n.keyconfig_skin = cfg.keyconfig_skin, n.skinselect_skin = cfg.skinselect_skin, n.sound_set = cfg.sound_set, n.skin_adjust = cfg.skin_adjust;
                    }
                    cfg = n;
                    // 引き継いだ設定とキーを、新しく作ったプレイヤーにも入れる（作ったときは引き継ぐ前の設定を写していた）
                    save_config(cfg);
                    for (const std::string& id : job.res.new_players)
                        for (const char* f : {"config.txt", "keyconfig.txt"}) {
                            std::error_code ec;
                            fs::copy_file(fs::u8path(player_dir() + f), fs::u8path(data_dir() + "Player/" + id + "/" + f),
                                          fs::copy_options::overwrite_existing, ec);
                        }
                    // LR2 で使っていたプレイヤーに切り替える。最初の起動で自動でできたプレイヤー（player）は、記録が無ければ消す
                    const std::string before = player_id();
                    if (!job.res.player.empty() && job.res.player != before) {
                        switch_player(job.res.player);
                        const bool from_lr2 = std::find(job.res.players.begin(), job.res.players.end(), before) != job.res.players.end();
                        SongDb old;
                        if (before == "player" && !from_lr2 && old.open(song_db_path(), before) && old.own_player_stats().playcount == 0) {
                            old.close();
                            std::string err;
                            if (delete_player(before, &err)) std::printf("記録の無いプレイヤー %s を消した\n", before.c_str());
                        }
                    }
                    message = "LR2 から引き継ぎました（プレイヤー " + std::to_string(job.res.players.size()) + "、曲フォルダ " +
                              std::to_string(job.res.song_dirs) + "、譜面 " +
                              std::to_string(job.res.charts) + "、スコア " + std::to_string(job.res.scores) + "、難易度表 " +
                              std::to_string(job.res.tables) + "、スキン " + std::to_string(job.res.skins) + "、設定 " +
                              std::to_string(job.res.settings) + "、お気に入り " + std::to_string(job.res.favorites) +
                              (job.res.keyconfig ? "、キーコンフィグ" : "") + "）";
                } else {
                    message = job.res.error;
                }
                std::printf("LR2 からの引き継ぎ：%s\n", message.c_str());
            }
        }
        if (borrow_test.running) {
            std::lock_guard<std::mutex> lock(borrow_test.mu);
            if (borrow_test.done) borrow_test.th.join(), borrow_test.running = false, message = borrow_test.result;
        }
        bool updating = false;
        if (update.running) {
            std::lock_guard<std::mutex> lock(update.mu);
            if (!update.status.empty()) message = update.status;
            updating = !update.done;
            if (update.done) {
                update.th.join();
                update.running = false;
                if (!update.moved_to.empty()) {
                    std::printf("受け取り口が %s へ移った\n", update.moved_to.c_str());
                    cfg.service_url = update.moved_to;
                    save_config(cfg);
                }
                if (update.ok) {
                    // 新しい版の設定アプリを起動し直し、譜面の配信も新しい版で動かし直す
                    g.unfocus();
                    save_changed_settings();
                    save_config(cfg);
                    if (cfg.serve) apply_serve(cfg, message);
                    relaunch_self();
                    quit = true;
                }
            }
        }
        if (!message.empty()) g.label(10, kH - 50 + 14, message);
        if (g.button(10, kH - 29, 87, 21, "RDの起動", !updating) || (autostart && !updating)) start = true, autostart = false;
        g.button(115, kH - 29, 87, 21, "プレイオプション", false);
        g.button(409, kH - 29, 83, 21, "動画作成", false);
        if (g.button(514, kH - 29, 87, 21, "終了")) {
            g.unfocus();
            save_config(cfg);
            if (serve_state(cfg) != serve_applied) apply_serve(cfg, message), serve_applied = serve_state(cfg);
            quit = true;
        }

        if (start) {
            start = false;
            g.unfocus();
            std::string err;
            // 配信は起動したそばから設定を読むので、先に保存してから登録し直す
            if (serve_state(cfg) != serve_applied) save_config(cfg), apply_serve(cfg, message), serve_applied = serve_state(cfg);
            if (!save_config(cfg)) {
                message = "設定を保存できません: " + cfg_path;
            } else if (save_changed_settings(), std::printf("本体を起動します\n"), !launch_body(err)) {
                message = err;
                std::printf("%s\n", err.c_str());
            } else {
                quit = true;
            }
        }
        if (const char* shot = dev_env("RD_LAUNCHER_SHOT")) {
            // 開発用：RD_LAUNCHER_TAB のタブを1枚保存して終わる
            static int frames = 0;
            if (const char* t = dev_env("RD_LAUNCHER_TAB")) tab = std::atoi(t);
            // 押す操作（RD_LAUNCHER_CLICKS）があれば、押し終わってから撮る
            if (++frames == 10 + 3 * static_cast<int>(dev_clicks.size())) {
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
        }
        save_changed_settings();  // MOD の詳細の設定が変わっていれば書く
        SDL_RenderPresent(r);
    }
    // 打ちかけの欄も確定してから、残っている変更を書く
    g.unfocus();
    save_changed_settings();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(window);
    // SDL_Quit は呼ばない。Windows では終わるまで 20 秒ほどかかることがあり、その間このプロセスが残って、
    // 更新で入れ替えた古いファイル（.old）を掴んだままになる。片付けはプロセスが終わるときに OS がする
    return 0;
}
