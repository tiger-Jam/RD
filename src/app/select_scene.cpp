#include "app/select_scene.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app/play_session.h"
#include "audio/system_sound.h"
#include "db/difficulty_table.h"
#include "game/gauge.h"
#include "input/input.h"
#include "input/joystick_init.h"
#include "render/text_cache.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

struct Entry {
    enum class Kind { Folder, Chart, Missing } kind = Kind::Folder;
    std::string label, sub, badge;  // badge はレベルの表示（例：sl5）
    const ChartRecord* chart = nullptr;
    TableChart missing;
    std::vector<Entry> children;
    int owned = 0, total = 0;  // フォルダの中で持っている譜面の数
};

// 曲の一覧（裏で読み直して、できたら差し替える）
struct Library {
    std::vector<ChartRecord> charts;
    std::vector<DifficultyTable> tables;
    std::unordered_map<std::string, ScoreRecord> scores;  // sha256 → スコア
};

SDL_Color lamp_color(int lamp) {
    switch (lamp) {
    case 1: return {150, 30, 30, 255};    // FAILED
    case 2: return {90, 210, 90, 255};    // EASY
    case 3: return {80, 160, 255, 255};   // NORMAL
    case 4: return {240, 240, 240, 255};  // HARD
    case 5: return {255, 210, 60, 255};   // FULL COMBO
    default: return {60, 60, 70, 255};    // NO PLAY
    }
}

const char* lamp_name(int lamp) {
    static const char* names[] = {"NO PLAY", "FAILED", "EASY CLEAR", "CLEAR", "HARD CLEAR", "FULL COMBO"};
    return lamp >= 0 && lamp <= 5 ? names[lamp] : "?";
}

// 一覧の木を作る：難易度表（レベル別）と曲フォルダ
std::vector<Entry> build_tree(const Library& lib) {
    std::unordered_map<std::string, const ChartRecord*> by_md5;
    for (const ChartRecord& c : lib.charts) by_md5.emplace(c.md5, &c);
    auto chart_entry = [&](const ChartRecord* c, const std::string& badge) {
        Entry e;
        e.kind = Entry::Kind::Chart;
        e.chart = c;
        e.label = c->title + (c->subtitle.empty() ? "" : " " + c->subtitle);
        e.sub = c->artist;
        e.badge = badge;
        return e;
    };
    std::vector<Entry> root;
    for (const DifficultyTable& t : lib.tables) {
        Entry tf;
        tf.label = t.name.empty() ? t.url : t.name;
        tf.sub = t.warning.empty() ? (t.from_cache ? "手元の版" : "") : t.warning;
        for (const std::string& lv : t.levels) {
            Entry lf;
            lf.badge = t.symbol + lv;
            lf.label = t.symbol + lv;
            for (const TableChart& tc : t.charts) {
                if (tc.level != lv) continue;
                auto it = by_md5.find(tc.md5);
                if (it != by_md5.end()) {
                    lf.children.push_back(chart_entry(it->second, lf.badge));
                    lf.owned++;
                } else {
                    Entry m;
                    m.kind = Entry::Kind::Missing;
                    m.missing = tc;
                    m.label = tc.title;
                    m.sub = tc.artist;
                    m.badge = lf.badge;
                    lf.children.push_back(m);
                }
                lf.total++;
            }
            tf.owned += lf.owned;
            tf.total += lf.total;
            tf.children.push_back(std::move(lf));
        }
        root.push_back(std::move(tf));
    }
    // 曲フォルダ：実際のフォルダごとにまとめる
    Entry songs;
    songs.label = "曲フォルダ";
    std::map<std::string, std::vector<const ChartRecord*>> by_folder;
    for (const ChartRecord& c : lib.charts) by_folder[c.folder].push_back(&c);
    for (auto& [folder, list] : by_folder) {
        Entry f;
        f.label = fs::path(folder).filename().string();
        std::sort(list.begin(), list.end(), [](const ChartRecord* a, const ChartRecord* b) {
            return a->notes < b->notes;
        });
        f.sub = list.empty() ? "" : list.front()->title;
        for (const ChartRecord* c : list) {
            f.children.push_back(chart_entry(c, std::to_string(c->playlevel)));
            f.owned++;
            f.total++;
        }
        songs.owned += f.owned;
        songs.total += f.total;
        songs.children.push_back(std::move(f));
    }
    root.push_back(std::move(songs));
    return root;
}

}  // namespace

void run_select(App& app) {
    SDL_Renderer* r = app.renderer;
    TextCache text(r);
    app.set_view(1280, 720);

    // 手元にあるものですぐに一覧を出し、曲フォルダの読み直しと難易度表の更新は裏で行う
    auto lib = std::make_shared<Library>();
    lib->charts = app.db.all_charts();
    for (const std::string& u : app.cfg.tables) {
        DifficultyTable t;
        if (!load_cached_table(table_cache_dir(), u, t)) t.url = u, t.warning = "まだ取得していない";
        lib->tables.push_back(std::move(t));
    }
    for (const ScoreRecord& s : app.db.all_scores()) lib->scores[s.sha256] = s;
    std::vector<Entry> root = build_tree(*lib);

    std::mutex mu;
    std::shared_ptr<Library> pending;
    std::atomic<bool> busy{false};
    std::string status = "";
    std::thread worker;
    auto refresh = [&] {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        worker = std::thread([&, dirs = app.cfg.song_dirs, urls = app.cfg.tables] {
            // 曲データベースは別の接続で書く（書き込みはこのスレッドだけ）
            SongDb wdb;
            ScanStats st;
            if (wdb.open(song_db_path()))
                st = wdb.scan(dirs, static_cast<int>(std::max(2u, std::thread::hardware_concurrency())));
            auto next = std::make_shared<Library>();
            next->charts = wdb.all_charts();
            for (const ScoreRecord& s : wdb.all_scores()) next->scores[s.sha256] = s;
            for (const std::string& u : urls) next->tables.push_back(update_table(table_cache_dir(), u));
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d 譜面（追加 %d／変更 %d／削除 %d）%.1f 秒", st.found, st.added, st.updated,
                          st.removed, st.seconds);
            std::lock_guard<std::mutex> lock(mu);
            pending = next;
            status = buf;
            busy = false;
        });
    };
    refresh();

    struct Level {
        const std::vector<Entry>* list;
        int cursor;
        std::string name;
    };
    std::vector<Level> stack{{&root, 0, "BMjoy"}};
    app.mixer.play_system(kSysSelect, true);  // 選曲BGM（ループ）
    bool autoplay = false;
    InputMapper input(app.keycfg);
    std::vector<std::string> joy_names;
    bool joy_reported = false;
    int repeat_dir = 0;
    uint64_t repeat_next = 0;
    bool quit = false;

    auto current = [&]() -> const Entry* {
        const Level& lv = stack.back();
        if (lv.list->empty()) return nullptr;
        return &(*lv.list)[std::clamp(lv.cursor, 0, static_cast<int>(lv.list->size()) - 1)];
    };
    auto move = [&](int d) {
        Level& lv = stack.back();
        int n = static_cast<int>(lv.list->size());
        if (n) lv.cursor = (lv.cursor + d + n) % n;
        app.mixer.play_system(kSysScratch);
    };
    // 一覧を差し替えたあと、いま見ているフォルダの位置を名前でたどり直す
    auto rebuild = [&] {
        std::vector<std::string> path;
        std::vector<int> cursors;
        for (size_t i = 1; i < stack.size(); ++i) path.push_back(stack[i].name);
        for (const Level& lv : stack) cursors.push_back(lv.cursor);
        root = build_tree(*lib);
        stack = {{&root, cursors[0], "BMjoy"}};
        for (size_t i = 0; i < path.size(); ++i) {
            const std::vector<Entry>& list = *stack.back().list;
            auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.label == path[i]; });
            if (it == list.end()) break;
            stack.push_back({&it->children, cursors[i + 1], path[i]});
        }
    };
    auto decide = [&] {
        const Entry* e = current();
        if (!e) return;
        if (e->kind == Entry::Kind::Folder) {
            stack.push_back({&e->children, 0, e->label});
            app.mixer.play_system(kSysFolderOpen);
        } else if (e->kind == Entry::Kind::Chart) {
            PlayOptions po;
            po.autoplay = autoplay;
            std::string path = e->chart->path;
            app.mixer.stop_system(kSysSelect);
            PlayExit ex = run_play(app, path, po);
            app.mixer.play_system(kSysSelect, true);
            app.set_view(1280, 720);
            input = InputMapper(app.keycfg);
            // スコアを読み直す
            for (const ScoreRecord& s : app.db.all_scores()) lib->scores[s.sha256] = s;
            if (ex == PlayExit::Quit) quit = true;
        } else {
            std::lock_guard<std::mutex> lock(mu);
            status = "この譜面は持っていない：" + e->missing.title;
        }
    };
    auto back = [&] {
        if (stack.size() > 1) {
            stack.pop_back();
            app.mixer.play_system(kSysFolderClose);
        }
    };

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                if (SDL_Joystick* j = SDL_OpenJoystick(e.jdevice.which)) {
                    const char* name = SDL_GetJoystickName(j);
                    std::printf("コントローラー接続: %s（ボタン %d、軸 %d、ハット %d）\n", name ? name : "?",
                                SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickHats(j));
                    joy_names.push_back(name ? name : "?");
                }
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) std::printf("ウィンドウにフォーカスが来た\n");
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) std::printf("ウィンドウのフォーカスが外れた\n");
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_UP: move(-1); break;
                case SDLK_DOWN: move(1); break;
                case SDLK_PAGEUP: move(-10); break;
                case SDLK_PAGEDOWN: move(10); break;
                case SDLK_RETURN:
                case SDLK_RIGHT: if (!e.key.repeat) decide(); break;
                case SDLK_LEFT:
                case SDLK_BACKSPACE: back(); break;
                case SDLK_ESCAPE:
                    if (e.key.repeat) break;
                    if (stack.size() > 1) back();
                    else quit = true;
                    break;
                case SDLK_F5: refresh(); break;
                case SDLK_F2:
                    autoplay = !autoplay;
                    app.mixer.play_system(kSysOptionChange);
                    break;
                case SDLK_TAB: {
                    // ゲージ：GROOVE → EASY → HARD → DEATH → G-ATTACK → P-ATTACK
                    static const int order[] = {kGroove, kEasy, kHard, kDeath, kGAttack, kPAttack};
                    int i = 0;
                    while (i < 6 && order[i] != app.cfg.gauge) ++i;
                    app.cfg.gauge = order[(i + 1) % 6];
                    app.mixer.play_system(kSysOptionChange);
                    break;
                }
                case SDLK_F4: {
                    bool fsc = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(app.window, !fsc);
                    break;
                }
                default: break;
                }
            }
            input.handle(e);
        }
        // コントローラ：白鍵で決定、黒鍵で戻る、皿で上下（LR2の選曲画面と同じ）
        uint64_t now = SDL_GetTicksNS();
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        input.collect(now, lanes, buttons);
        for (const ButtonInput& b : buttons) {
            if (!b.down) {
                if (b.button == kScratchA || b.button == kScratchB) repeat_dir = 0;
                continue;
            }
            if (b.button == kKey1 || b.button == kKey1 + 2 || b.button == kKey1 + 4 || b.button == kKey1 + 6) decide();
            else if (b.button == kKey1 + 1 || b.button == kKey1 + 3 || b.button == kKey1 + 5) back();
            else if (b.button == kScratchA || b.button == kScratchB) {
                repeat_dir = b.button == kScratchA ? 1 : -1;
                move(repeat_dir);
                repeat_next = now + 300'000'000ull;
            }
        }
        if (repeat_dir != 0 && now >= repeat_next) {
            move(repeat_dir);
            repeat_next = now + 60'000'000ull;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            if (pending) {
                lib = pending;
                pending.reset();
                rebuild();
            }
        }

        // 描画
        SDL_SetRenderDrawColor(r, 12, 12, 20, 255);
        SDL_RenderClear(r);
        SDL_Color white{235, 235, 240, 255}, dim{140, 140, 155, 255}, accent{255, 210, 90, 255};
        std::string crumb;
        for (size_t i = 0; i < stack.size(); ++i) crumb += (i ? "  ›  " : "") + stack[i].name;
        text.draw(24, 16, 22, white, crumb, 0, 760);

        const Level& lv = stack.back();
        const std::vector<Entry>& list = *lv.list;
        int n = static_cast<int>(list.size());
        int rows = 15, top = std::clamp(lv.cursor - rows / 2, 0, std::max(0, n - rows));
        for (int i = 0; i < rows && top + i < n; ++i) {
            const Entry& en = list[top + i];
            float y = 60 + i * 40.0f;
            bool sel = top + i == lv.cursor;
            SDL_FRect bg{20, y, 780, 36};
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, sel ? 60 : 26, sel ? 56 : 26, sel ? 90 : 38, 255);
            SDL_RenderFillRect(r, &bg);
            SDL_Color badge_bg{50, 50, 70, 255};
            int lamp = 0;
            if (en.kind == Entry::Kind::Chart) {
                auto it = lib->scores.find(en.chart->sha256);
                if (it != lib->scores.end()) lamp = it->second.clear;
            }
            SDL_Color lc = lamp_color(lamp);
            SDL_FRect lampbar{20, y, 8, 36};
            SDL_SetRenderDrawColor(r, lc.r, lc.g, lc.b, 255);
            if (en.kind != Entry::Kind::Folder) SDL_RenderFillRect(r, &lampbar);
            if (!en.badge.empty() && en.kind != Entry::Kind::Folder) {
                SDL_FRect b{36, y + 6, 64, 24};
                SDL_SetRenderDrawColor(r, badge_bg.r, badge_bg.g, badge_bg.b, 255);
                SDL_RenderFillRect(r, &b);
                text.draw(68, y + 8, 18, accent, en.badge, 1, 60);
            }
            SDL_Color tc = en.kind == Entry::Kind::Missing ? dim : white;
            float tx = en.kind == Entry::Kind::Folder ? 40.0f : 112.0f;
            text.draw(tx, y + 3, 20, tc, en.label, 0, 560);
            if (en.kind == Entry::Kind::Folder) {
                char cnt[64];
                std::snprintf(cnt, sizeof(cnt), "%d / %d", en.owned, en.total);
                text.draw(790, y + 8, 18, dim, cnt, 2);
            } else {
                text.draw(tx + 4, y + 22, 13, dim, en.sub, 0, 520);
                if (en.kind == Entry::Kind::Missing) text.draw(790, y + 10, 15, dim, "未所持", 2);
            }
        }
        if (n == 0) text.draw(40, 80, 20, dim, "（空）");
        char pos[32];
        std::snprintf(pos, sizeof(pos), "%d / %d", n ? lv.cursor + 1 : 0, n);
        text.draw(800, 664, 16, dim, pos, 2);

        // 右側：選んでいる譜面の情報
        float px = 830;
        if (const Entry* cur = current()) {
            if (cur->kind == Entry::Kind::Chart) {
                const ChartRecord& c = *cur->chart;
                text.draw(px, 70, 24, white, c.title, 0, 420);
                if (!c.subtitle.empty()) text.draw(px, 100, 16, dim, c.subtitle, 0, 420);
                text.draw(px, 124, 17, white, c.artist, 0, 420);
                if (!c.subartist.empty()) text.draw(px, 146, 14, dim, c.subartist, 0, 420);
                char b[160];
                std::snprintf(b, sizeof(b), "GENRE  %s", c.genre.c_str());
                text.draw(px, 180, 16, dim, b, 0, 420);
                if (c.bpm_min != c.bpm_max) std::snprintf(b, sizeof(b), "BPM  %.0f - %.0f（主 %.0f）", c.bpm_min, c.bpm_max, c.bpm_main);
                else std::snprintf(b, sizeof(b), "BPM  %.0f", c.bpm_main);
                text.draw(px, 204, 16, white, b);
                std::snprintf(b, sizeof(b), "NOTES  %d%s   %d:%02d", c.notes, c.ln ? "（LNあり）" : "", c.length_ms / 60000,
                              c.length_ms / 1000 % 60);
                text.draw(px, 228, 16, white, b);
                std::snprintf(b, sizeof(b), "#RANK %d   TOTAL %d%s", c.rank, c.total, c.scratch ? "" : "   皿なし");
                text.draw(px, 252, 16, dim, b);
                ScoreRecord sc;
                auto it = lib->scores.find(c.sha256);
                if (it != lib->scores.end()) sc = it->second;
                text.draw(px, 300, 26, lamp_color(sc.clear).a ? lamp_color(sc.clear) : white, lamp_name(sc.clear));
                int max_ex = c.notes * 2;
                std::snprintf(b, sizeof(b), "EX SCORE  %d / %d（%.2f%%）", sc.exscore, max_ex,
                              max_ex ? sc.exscore * 100.0 / max_ex : 0.0);
                text.draw(px, 340, 17, white, b);
                if (sc.bp >= 0 && sc.playcount > 0) std::snprintf(b, sizeof(b), "BP  %d    MAX COMBO  %d", sc.bp, sc.max_combo);
                else std::snprintf(b, sizeof(b), "BP  -");
                text.draw(px, 366, 17, white, b);
                std::snprintf(b, sizeof(b), "PLAY  %d    CLEAR  %d", sc.playcount, sc.clearcount);
                text.draw(px, 392, 17, dim, b);
            } else if (cur->kind == Entry::Kind::Missing) {
                text.draw(px, 70, 24, dim, cur->missing.title, 0, 420);
                text.draw(px, 104, 17, dim, cur->missing.artist, 0, 420);
                text.draw(px, 140, 16, white, "この譜面は手元に無い");
                if (!cur->missing.url.empty()) text.draw(px, 168, 13, dim, "本体：" + cur->missing.url, 0, 420);
                if (!cur->missing.url_diff.empty()) text.draw(px, 188, 13, dim, "差分：" + cur->missing.url_diff, 0, 420);
            } else {
                text.draw(px, 70, 24, white, cur->label, 0, 420);
                if (!cur->sub.empty()) text.draw(px, 104, 15, dim, cur->sub, 0, 420);
                char b[64];
                std::snprintf(b, sizeof(b), "所持 %d / %d", cur->owned, cur->total);
                text.draw(px, 130, 17, white, b);
            }
        }
        static const char* gauges[] = {"GROOVE", "HARD", "DEATH", "EASY", "P-ATTACK", "G-ATTACK"};
        char opt[128];
        std::snprintf(opt, sizeof(opt), "GAUGE %s   %s   HS %d.%02d", gauges[std::clamp(app.cfg.gauge, 0, 5)],
                      autoplay ? "AUTO PLAY" : "", app.cfg.hi_speed / 100, app.cfg.hi_speed % 100);
        text.draw(px, 620, 16, accent, opt);
        std::string st_text;
        {
            std::lock_guard<std::mutex> lock(mu);
            st_text = busy ? "更新中…" : status;
        }
        text.draw(px, 644, 14, dim, st_text, 0, 420);
        // コントローラーの状態（Windows では準備に時間がかかることがある）
        std::string joy_text;
        if (!joystick_init().ready()) joy_text = "コントローラーを準備中…（キーボードは使える）";
        else if (joy_names.empty()) joy_text = "コントローラー：なし";
        else joy_text = "コントローラー：" + joy_names.front() + (joy_names.size() > 1 ? " ほか" : "");
        for (const std::string& nm : joystick_init().open_new()) {
            std::printf("コントローラー接続: %s\n", nm.c_str());
            joy_names.push_back(nm);
        }
        if (joystick_init().ready() && !joy_reported) {
            std::printf("コントローラーの準備が終わった（%dms）\n", joystick_init().took_ms());
            joy_reported = true;
        }
        if (!(SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS)) joy_text = "ウィンドウをクリックすると操作できます";
        text.draw(px, 600, 14, joystick_init().ready() ? dim : accent, joy_text, 0, 420);
        text.draw(24, 690, 14, dim,
                  "↑↓ 選ぶ　Enter/→ 決定　Esc/← 戻る　TAB ゲージ　F2 オート　F5 更新　（コントローラ：白鍵 決定／黒鍵 戻る／皿 選ぶ）");
        if (std::getenv("BMJ_DEBUG_LOOP")) {
            // 開発用：選曲画面の1秒あたりの描画回数と、1回にかかった時間
            static uint64_t t = SDL_GetTicksNS();
            static int frames = 0;
            static double sum = 0, mx = 0;
            double ms = (SDL_GetTicksNS() - now) / 1e6;
            ++frames, sum += ms, mx = std::max(mx, ms);
            if (SDL_GetTicksNS() - t > 1000000000ull) {
                std::printf("選曲画面 %dfps、1回 平均%.2fms 最大%.2fms\n", frames, sum / frames, mx);
                frames = 0, sum = 0, mx = 0;
                t = SDL_GetTicksNS();
            }
        }
        bool settled;
        {
            std::lock_guard<std::mutex> lock(mu);
            settled = !busy && !pending;
        }
        if (const char* shot = std::getenv("BMJ_SELECT_SHOT"); shot && settled) {
            // 開発用：更新が終わってから1枚保存して終わる。BMJ_SELECT_KEYS の文字で操作してから撮る（d=下 e=決定）
            static size_t step = 0;
            const char* keys = std::getenv("BMJ_SELECT_KEYS");
            if (keys && keys[step]) {
                char k = keys[step++];
                if (k == 'd') move(1);
                else if (k == 'D') move(10);
                else if (k == 'u') move(-1);
                else if (k == 'e') decide();
                else if (k == 'b') back();
            } else {
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
        }
        SDL_RenderPresent(r);
        SDL_DelayPrecise(4'000'000);
    }
    app.mixer.stop_system(kSysSelect);
    if (worker.joinable()) worker.join();
}

}  // namespace bmj
