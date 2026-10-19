#include "mod/script.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

// Lua は C++ として組んでいる（CMakeLists.txt）ので、extern "C" の lua.hpp ではなく素のヘッダを読む
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// 1 回の呼び出しで動かしてよい命令の数（止まらないスクリプトで本体が固まらないように）。
// フォルダを作るときは譜面の数だけ回るので多めにする
constexpr long long kStepsPerCall = 5'000'000;
constexpr long long kStepsFolders = 2'000'000'000;
constexpr long long kHookEvery = 10'000;
constexpr size_t kMemoryLimit = 256u << 20;  // 1 つの MOD が使ってよいメモリ

const char* kJudgeNames[6] = {"EMPTY_POOR", "POOR", "BAD", "GOOD", "GREAT", "PGREAT"};

}  // namespace

bool mod_file_name_ok(const std::string& name) {
    if (name.empty() || name.size() > 200) return false;
    fs::path p(name);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p)
        if (part == ".." || part == ".") return false;
    for (char c : name)
        if (c == ':' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

// MOD 1 つぶんの Lua の環境
class ModScript {
public:
    ModScript(const ModManifest& m, std::string data_dir)
        : id_(m.id), name_(m.name), dir_(m.dir), touches_(m.touches), data_dir_(std::move(data_dir)) {}
    ~ModScript() {
        if (L_) lua_close(L_);
    }

    bool load(const std::string& script, std::string& err) {
        L_ = lua_newstate(&ModScript::alloc, this);
        if (!L_) return err = id_ + ": Lua を始められない", false;
        *static_cast<ModScript**>(lua_getextraspace(L_)) = this;
        open_libs();
        std::ifstream f(fs::path(dir_) / script, std::ios::binary);
        if (!f) return err = id_ + ": スクリプトが無い: " + script, false;
        std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // 文字のスクリプトだけ（バイトコードは読まない）
        if (luaL_loadbufferx(L_, code.data(), code.size(), ("@" + script).c_str(), "t") != LUA_OK) {
            err = id_ + ": " + lua_tostring(L_, -1);
            return false;
        }
        if (!call(0, kStepsPerCall)) return err = error_, false;
        return true;
    }

    bool alive() const { return L_ && error_.empty(); }
    bool touches(ModTouch t) const { return (touches_ & static_cast<uint32_t>(t)) != 0; }
    const std::string& error() const { return error_; }
    const std::string& id() const { return id_; }

    // 窓口の関数 name があるか
    bool has(const char* name) {
        if (!alive()) return false;
        bool f = lua_getglobal(L_, name) == LUA_TFUNCTION;
        lua_pop(L_, 1);
        return f;
    }
    // 窓口の関数 name を、push_arg が積んだ 1 つの引数で呼ぶ。read_result があれば戻り値を 1 つ読む。
    // 引数を作るところから守られた呼び出しの中で行う（メモリが足りないときも本体を落とさない）
    bool run(const char* name, const std::function<void(lua_State*)>& push_arg, long long steps,
             const std::function<void(lua_State*)>& read_result = {}) {
        if (!has(name)) return false;
        struct Ctx {
            const char* name;
            const std::function<void(lua_State*)>* push_arg;
            const std::function<void(lua_State*)>* read_result;
        } ctx{name, &push_arg, &read_result};
        lua_pushcfunction(L_, [](lua_State* L) -> int {
            auto* c = static_cast<Ctx*>(lua_touserdata(L, 1));
            lua_getglobal(L, c->name);
            (*c->push_arg)(L);
            lua_call(L, 1, c->read_result && *c->read_result ? 1 : 0);
            if (c->read_result && *c->read_result) (*c->read_result)(L);
            return 0;
        });
        lua_pushlightuserdata(L_, &ctx);
        return call(1, steps);
    }

    // 積んである関数と nargs 個の引数を呼ぶ。失敗したらこの MOD を止める
    bool call(int nargs, long long steps, int nresults = 0) {
        steps_left_ = steps;
        lua_sethook(L_, &ModScript::hook, LUA_MASKCOUNT, static_cast<int>(kHookEvery));
        int base = lua_gettop(L_) - nargs;
        lua_pushcfunction(L_, &ModScript::traceback);
        lua_insert(L_, base);
        int rc = lua_pcall(L_, nargs, nresults, base);
        lua_remove(L_, base);
        lua_sethook(L_, nullptr, 0, 0);
        if (rc != LUA_OK) {
            const char* m = lua_tostring(L_, -1);
            error_ = id_ + ": " + (m ? m : "スクリプトが失敗した");
            lua_pop(L_, 1);
            return false;
        }
        return true;
    }

    std::vector<ModDrawCmd>* draw_sink = nullptr;

private:
    static ModScript* self(lua_State* L) { return *static_cast<ModScript**>(lua_getextraspace(L)); }

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
        auto* s = static_cast<ModScript*>(ud);
        size_t old = ptr ? osize : 0;
        if (nsize == 0) {
            s->memory_ -= old;
            std::free(ptr);
            return nullptr;
        }
        if (nsize > old && s->memory_ + (nsize - old) > kMemoryLimit) return nullptr;  // Lua は「メモリが足りない」で止まる
        void* p = std::realloc(ptr, nsize);
        if (p) s->memory_ = s->memory_ - old + nsize;
        return p;
    }

    static void hook(lua_State* L, lua_Debug*) {
        ModScript* s = self(L);
        s->steps_left_ -= kHookEvery;
        if (s->steps_left_ < 0) luaL_error(L, "時間がかかりすぎた（1 回の呼び出しで動かせる量を超えた）");
    }

    static int traceback(lua_State* L) {
        const char* m = lua_tostring(L, 1);
        luaL_traceback(L, L, m ? m : "スクリプトが失敗した", 1);
        return 1;
    }

    void open_libs() {
        static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                        {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                        {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine}};
        for (const luaL_Reg& l : libs) {
            luaL_requiref(L_, l.name, l.func, 1);
            lua_pop(L_, 1);
        }
        // ファイルを読んで動かす関数と、バイトコードを読める load は外す
        for (const char* name : {"dofile", "loadfile", "load", "require", "collectgarbage"}) {
            lua_pushnil(L_);
            lua_setglobal(L_, name);
        }
        lua_pushcfunction(L_, &ModScript::l_log);
        lua_setglobal(L_, "print");

        lua_newtable(L_);
        static const luaL_Reg fns[] = {{"log", &ModScript::l_log},
                                       {"write_file", &ModScript::l_write_file},
                                       {"append_file", &ModScript::l_append_file},
                                       {"read_file", &ModScript::l_read_file},
                                       {"read_mod_file", &ModScript::l_read_mod_file},
                                       {"time", &ModScript::l_time},
                                       {"date", &ModScript::l_date},
                                       {"draw_rect", &ModScript::l_draw_rect},
                                       {"draw_text", &ModScript::l_draw_text},
                                       {nullptr, nullptr}};
        luaL_setfuncs(L_, fns, 0);
        lua_newtable(L_);
        for (int i = 0; i < 6; ++i) {
            lua_pushinteger(L_, i);
            lua_setfield(L_, -2, kJudgeNames[i]);
        }
        lua_setfield(L_, -2, "JUDGE");
        lua_pushstring(L_, id_.c_str());
        lua_setfield(L_, -2, "id");
        lua_setglobal(L_, "bmj");
    }

    // bmj.log(...)：黒い画面に出す
    static int l_log(lua_State* L) {
        int n = lua_gettop(L);
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        for (int i = 1; i <= n; ++i) {
            if (i > 1) luaL_addchar(&b, ' ');
            luaL_tolstring(L, i, nullptr);
            luaL_addvalue(&b);
        }
        luaL_pushresult(&b);
        std::printf("MOD %s: %s\n", self(L)->id_.c_str(), lua_tostring(L, -1));
        return 0;
    }

    // 書けたら true。書けなければ nil と理由（スクリプトは止めない）
    static int write_impl(lua_State* L, bool append) {
        size_t nlen = 0, tlen = 0;
        const char* n = luaL_checklstring(L, 1, &nlen);
        const char* t = luaL_checklstring(L, 2, &tlen);
        ModScript* s = self(L);
        bool ok = false;
        const char* why = "名前が使えない";
        {
            std::string name(n, nlen);
            if (mod_file_name_ok(name)) {
                fs::path p = fs::path(s->data_dir_) / fs::u8path(name);
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
                if (f) f.write(t, static_cast<std::streamsize>(tlen));
                ok = static_cast<bool>(f);
                why = "書けない";
            }
        }
        if (ok) return lua_pushboolean(L, 1), 1;
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    static int l_write_file(lua_State* L) { return write_impl(L, false); }
    static int l_append_file(lua_State* L) { return write_impl(L, true); }

    static int read_impl(lua_State* L, const std::string& root) {
        size_t nlen = 0;
        const char* n = luaL_checklstring(L, 1, &nlen);
        bool ok = false;
        {
            std::string name(n, nlen), text;
            if (mod_file_name_ok(name)) {
                std::ifstream f(fs::path(root) / fs::u8path(name), std::ios::binary);
                if (f) {
                    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    lua_pushlstring(L, text.data(), text.size());
                    ok = true;
                }
            }
        }
        if (!ok) lua_pushnil(L);
        return 1;
    }
    // bmj.read_file(name)：自分が書いたファイル。bmj.read_mod_file(name)：MOD のフォルダのファイル
    static int l_read_file(lua_State* L) { return read_impl(L, self(L)->data_dir_); }
    static int l_read_mod_file(lua_State* L) { return read_impl(L, self(L)->dir_); }

    static int l_time(lua_State* L) {
        lua_pushinteger(L, static_cast<lua_Integer>(std::time(nullptr)));
        return 1;
    }
    // bmj.date(書式, 時刻)：strftime と同じ書式（既定 "%Y-%m-%d %H:%M:%S"、時刻の既定は今）
    static int l_date(lua_State* L) {
        const char* fmt = luaL_optstring(L, 1, "%Y-%m-%d %H:%M:%S");
        std::time_t t = static_cast<std::time_t>(luaL_optinteger(L, 2, static_cast<lua_Integer>(std::time(nullptr))));
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[256];
        size_t n = std::strlen(fmt) < 100 ? std::strftime(buf, sizeof(buf), fmt, &tm) : 0;
        lua_pushlstring(L, buf, n);
        return 1;
    }

    // bmj.draw_rect(x, y, w, h, 色 0xRRGGBB, 不透明度 0〜255)
    static int l_draw_rect(lua_State* L) {
        ModDrawCmd c;
        c.kind = ModDrawCmd::Kind::Rect;
        c.x = static_cast<float>(luaL_checknumber(L, 1)), c.y = static_cast<float>(luaL_checknumber(L, 2));
        c.w = static_cast<float>(luaL_checknumber(L, 3)), c.h = static_cast<float>(luaL_checknumber(L, 4));
        c.rgb = static_cast<uint32_t>(luaL_optinteger(L, 5, 0xffffff)) & 0xffffff;
        c.alpha = static_cast<uint8_t>(std::clamp<lua_Integer>(luaL_optinteger(L, 6, 255), 0, 255));
        ModScript* s = self(L);
        if (s->draw_sink && s->draw_sink->size() < 4096) s->draw_sink->push_back(c);
        return 0;
    }
    // bmj.draw_text(x, y, 文字, 大きさ, 色, 不透明度, 揃え 0 左 1 中央 2 右)
    static int l_draw_text(lua_State* L) {
        lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
        size_t len = 0;
        const char* t = luaL_checklstring(L, 3, &len);
        lua_Integer size = luaL_optinteger(L, 4, 16), rgb = luaL_optinteger(L, 5, 0xffffff), a = luaL_optinteger(L, 6, 255),
                    align = luaL_optinteger(L, 7, 0);
        ModScript* s = self(L);
        if (s->draw_sink && s->draw_sink->size() < 4096) {
            ModDrawCmd c;
            c.kind = ModDrawCmd::Kind::Text;
            c.x = static_cast<float>(x), c.y = static_cast<float>(y);
            c.text.assign(t, std::min<size_t>(len, 256));
            c.size = static_cast<int>(std::clamp<lua_Integer>(size, 6, 128));
            c.rgb = static_cast<uint32_t>(rgb) & 0xffffff;
            c.alpha = static_cast<uint8_t>(std::clamp<lua_Integer>(a, 0, 255));
            c.align = static_cast<int>(std::clamp<lua_Integer>(align, 0, 2));
            s->draw_sink->push_back(std::move(c));
        }
        return 0;
    }

    lua_State* L_ = nullptr;
    std::string id_, name_, dir_;
    uint32_t touches_ = 0;
    std::string data_dir_;
    std::string error_;
    size_t memory_ = 0;
    long long steps_left_ = 0;
};

namespace {

void set_str(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void set_int(lua_State* L, const char* k, long long v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, -2, k);
}
void set_num(lua_State* L, const char* k, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void set_bool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

std::unique_ptr<ModScript> open_script(const ModManifest& m, const std::string& data_root, std::string& err) {
    auto s = std::make_unique<ModScript>(m, (fs::path(data_root) / fs::u8path(m.id)).string());
    if (!s->load(m.script, err)) return nullptr;
    return s;
}

// フォルダの表 { title = "…", charts = { sha256, … }, folders = { … } } を読む（深さは 4 まで）
void read_folder(lua_State* L, int idx, ModFolder& out, int depth) {
    idx = lua_absindex(L, idx);
    if (lua_getfield(L, idx, "title") == LUA_TSTRING) out.title = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "charts") == LUA_TTABLE) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TSTRING) out.charts.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (depth < 4 && lua_getfield(L, idx, "folders") == LUA_TTABLE) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TTABLE) {
                ModFolder f;
                read_folder(L, -1, f, depth + 1);
                if (!f.title.empty()) out.children.push_back(std::move(f));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

}  // namespace

ModScripts::ModScripts() = default;
ModScripts::~ModScripts() = default;

void ModScripts::load(const std::vector<const ModManifest*>& mods, const std::string& data_root) {
    scripts_.clear();
    for (const ModManifest* m : mods) {
        if (m->script.empty()) continue;
        std::string err;
        if (auto s = open_script(*m, data_root, err)) scripts_.push_back(std::move(s));
        else errors_.push_back(err);
    }
}

void ModScripts::play_start(const ModPlayInfo& info) {
    for (auto& s : scripts_) {
        if (!s->has("on_play_start")) continue;
        bool ok = s->run("on_play_start", [&](lua_State* L) {
            lua_newtable(L);
            set_str(L, "sha256", info.sha256), set_str(L, "md5", info.md5), set_str(L, "path", info.path);
            set_str(L, "title", info.title), set_str(L, "subtitle", info.subtitle), set_str(L, "artist", info.artist);
            set_str(L, "genre", info.genre), set_str(L, "gauge", info.gauge);
            set_int(L, "playlevel", info.playlevel), set_int(L, "notes", info.notes), set_int(L, "total", info.total);
            set_int(L, "rank", info.rank), set_int(L, "keys", info.keys), set_int(L, "length_ms", info.length_ms);
            set_num(L, "bpm_min", info.bpm_min), set_num(L, "bpm_max", info.bpm_max);
            set_int(L, "random", info.random);
            set_bool(L, "autoplay", info.autoplay), set_bool(L, "replay", info.replay), set_bool(L, "course", info.course);
            set_int(L, "view_w", info.view_w), set_int(L, "view_h", info.view_h);
            // windows = { pgreat = { 早, 遅 }, great = …, good = …, bad = … }
            lua_newtable(L);
            const std::pair<const char*, const int*> w[] = {{"pgreat", info.pgreat}, {"great", info.great}, {"good", info.good}, {"bad", info.bad}};
            for (const auto& [k, v] : w) {
                lua_createtable(L, 2, 0);
                lua_pushinteger(L, v[0]), lua_rawseti(L, -2, 1);
                lua_pushinteger(L, v[1]), lua_rawseti(L, -2, 2);
                lua_setfield(L, -2, k);
            }
            lua_setfield(L, -2, "windows");
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

void ModScripts::judge(const ModJudgeInfo& e) {
    for (auto& s : scripts_) {
        if (!s->has("on_judge")) continue;
        bool ok = s->run("on_judge", [&](lua_State* L) {
            lua_newtable(L);
            set_num(L, "time_ms", e.time_ms);
            set_int(L, "lane", e.lane);
            set_int(L, "judge", e.judge);
            set_str(L, "name", e.mine ? "MINE" : kJudgeNames[std::clamp(e.judge, 0, 5)]);
            if (e.has_offset) set_int(L, "offset_ms", e.offset_ms);
            set_bool(L, "fast", e.fast), set_bool(L, "mine", e.mine), set_bool(L, "ln_end", e.ln_end);
            set_int(L, "combo", e.combo), set_int(L, "exscore", e.exscore);
            set_num(L, "hp", e.hp);
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

void ModScripts::play_end(const ModPlayResult& r) {
    for (auto& s : scripts_) {
        if (!s->has("on_play_end")) continue;
        bool ok = s->run("on_play_end", [&](lua_State* L) {
            lua_newtable(L);
            set_int(L, "clear", r.clear), set_int(L, "exscore", r.exscore), set_int(L, "bp", r.bp);
            set_int(L, "max_combo", r.max_combo), set_int(L, "total_notes", r.total_notes);
            set_int(L, "fast", r.fast), set_int(L, "slow", r.slow);
            set_bool(L, "quit_early", r.quit_early);
            lua_newtable(L);
            for (int i = 0; i < 6; ++i) set_int(L, kJudgeNames[i], r.judge[i]);
            lua_setfield(L, -2, "judges");
            lua_newtable(L);
            for (const auto& [k, v] : r.gauges) set_num(L, k.c_str(), v);
            lua_setfield(L, -2, "gauges");
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

const std::vector<ModDrawCmd>& ModScripts::draw(const ModDrawState& st) {
    draw_.clear();
    for (auto& s : scripts_) {
        if (!s->touches(ModTouch::Visual) || !s->has("on_draw")) continue;
        s->draw_sink = &draw_;
        bool ok = s->run("on_draw", [&](lua_State* L) {
            lua_newtable(L);
            set_num(L, "now_ms", st.now_ms), set_num(L, "song_ms", st.song_ms);
            set_int(L, "combo", st.combo), set_int(L, "exscore", st.exscore);
            set_num(L, "hp", st.hp);
            set_int(L, "view_w", st.view_w), set_int(L, "view_h", st.view_h);
        }, kStepsPerCall);
        s->draw_sink = nullptr;
        if (!ok) errors_.push_back(s->error());
    }
    return draw_;
}

std::vector<std::string> ModScripts::take_errors() {
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

std::vector<ModFolder> build_mod_folders(const std::vector<const ModManifest*>& mods, const std::vector<ChartRecord>& charts,
                                         const std::unordered_map<std::string, ScoreRecord>& scores,
                                         const std::string& data_root, std::vector<std::string>* errors) {
    std::vector<ModFolder> out;
    for (const ModManifest* m : mods) {
        if (m->script.empty() || !(m->touches & static_cast<uint32_t>(ModTouch::Control))) continue;
        std::string err;
        auto s = open_script(*m, data_root, err);
        if (!s) {
            if (errors) errors->push_back(err);
            continue;
        }
        if (!s->has("build_folders")) continue;
        bool ok = s->run("build_folders", [&](lua_State* L) {
            // 譜面の一覧：{ sha256, md5, title, …, clear, exscore, bp, playcount }（記録が無ければ clear = 0）
            lua_createtable(L, static_cast<int>(charts.size()), 0);
            lua_Integer i = 0;
            for (const ChartRecord& c : charts) {
                lua_createtable(L, 0, 26);
                set_str(L, "sha256", c.sha256), set_str(L, "md5", c.md5), set_str(L, "path", c.path), set_str(L, "folder", c.folder);
                set_str(L, "title", c.title), set_str(L, "subtitle", c.subtitle), set_str(L, "artist", c.artist);
                set_str(L, "genre", c.genre);
                set_int(L, "playlevel", c.playlevel), set_int(L, "difficulty", c.difficulty), set_int(L, "notes", c.notes);
                set_int(L, "ln", c.ln), set_int(L, "rank", c.rank), set_int(L, "total", c.total), set_int(L, "keys", c.keys);
                set_num(L, "bpm_min", c.bpm_min), set_num(L, "bpm_max", c.bpm_max), set_num(L, "bpm_main", c.bpm_main);
                set_int(L, "length_ms", c.length_ms), set_bool(L, "scratch", c.scratch), set_int(L, "added", c.added);
                auto it = scores.find(c.sha256);
                if (it != scores.end()) {
                    set_int(L, "clear", it->second.clear), set_int(L, "exscore", it->second.exscore);
                    set_int(L, "bp", it->second.bp), set_int(L, "playcount", it->second.playcount);
                } else {
                    set_int(L, "clear", 0);
                }
                lua_rawseti(L, -2, ++i);
            }
        }, kStepsFolders, [&](lua_State* L) {
            // 戻り値はフォルダの配列
            if (!lua_istable(L, -1)) return;
            lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer k = 1; k <= n; ++k) {
                if (lua_rawgeti(L, -1, k) == LUA_TTABLE) {
                    ModFolder f;
                    read_folder(L, -1, f, 0);
                    if (!f.title.empty()) out.push_back(std::move(f));
                }
                lua_pop(L, 1);
            }
        });
        if (!ok && errors) errors->push_back(s->error());
    }
    return out;
}

}  // namespace bmj
