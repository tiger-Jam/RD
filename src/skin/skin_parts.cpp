#include "skin/skin_parts.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "skin/skin_color.h"
#include "skin/skin_renderer.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// 部品のセット。添字は一覧の並び順。フレームは切らずに 1 枚に描く
enum Part { kNotes, kJudge, kBomb, kBeam, kKey, kGauge, kLane, kCover, kFrame, kPartCount };
const char* const kPartDir[kPartCount] = {"notes", "judge", "bomb", "keybeam", "key", "gauge", "lane", "cover", "frame"};

struct Element {
    Part part = kFrame;
    std::string name;  // セットの中での名前（ファイル名にもする）
    std::string what;  // CSV の命令
    const SkinSrc* src = nullptr;
    const SkinDst* dst = nullptr;
    std::vector<std::string> files;  // 書き出した PNG（out_dir から見たパス。コマ順）
    int fw = 0, fh = 0;              // 1 コマの大きさ
};

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else if (c == '<') o += "\\u003c";  // </script> で切れないように
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

bool save_png(SDL_Surface* s, const fs::path& path) {
    std::u8string u8 = path.u8string();
    return SDL_SavePNG(s, reinterpret_cast<const char*>(u8.c_str()));
}

bool save_crop(const SkinImage& img, int x, int y, int w, int h, const fs::path& path) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > img.w || y + h > img.h) return false;
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32,
                                           const_cast<uint8_t*>(img.rgba.data()) + (static_cast<size_t>(y) * img.w + x) * 4, img.w * 4);
    if (!s) return false;
    bool ok = save_png(s, path);
    SDL_DestroySurface(s);
    return ok;
}

// レーンの呼び名（0 が皿、1〜9 が鍵盤、10〜 が 2P 側）
std::string lane_name(int l) { return (l >= 10 ? "2p_" : "") + (l % 10 == 0 ? std::string("S") : std::to_string(l % 10)); }

// #SRC_IMAGE がどのセットに入るか（skin_color の見分け方と同じ）
Part part_of_image(const SkinObject& o, float judge_bottom, std::string& name) {
    int lane;
    bool on;
    const SkinImagePart p = image_part(o, judge_bottom, lane, on);
    const std::string key = lane >= 0 ? lane_name(lane) + (on ? "_on" : "_off") : "";
    const int t = o.dst.timer ? o.dst.timer : o.src.timer;
    switch (p) {
    case SkinImagePart::Bomb: return name = "bomb_" + lane_name(lane), kBomb;
    case SkinImagePart::Hold: return name = "hold_" + lane_name(lane), kBomb;
    case SkinImagePart::Beam: return name = "beam_" + key, kBeam;
    case SkinImagePart::Key: return name = "key_" + key, kKey;
    case SkinImagePart::JudgeFx: return name = "judge_fx", kJudge;
    case SkinImagePart::GaugeFx: return name = t <= 43 ? "gauge_up" : "gauge_max", kGauge;
    case SkinImagePart::Frame: break;
    }
    return kFrame;
}

// フレームだけを残したスキンを、ソフトウェアの描画で 1 枚の絵にする（曲が始まって少し経ったところ）
bool render_frame(const Lr2Skin& sk, const std::vector<bool>& is_frame_image, const fs::path& path) {
    Lr2Skin f = sk;
    f.image_objs.clear();
    for (size_t i = 0; i < sk.image_objs.size(); ++i)
        if (is_frame_image[i]) f.image_objs.push_back(sk.image_objs[i]);
    f.sliders.clear();
    for (const SkinObject& o : sk.sliders)
        if (o.src.op[2] != 4 && o.src.op[2] != 5) f.sliders.push_back(o);
    f.note_dst = {};
    f.line = {}, f.judgeline = {}, f.gauge = {};
    f.nowjudge = {}, f.nowcombo = {};

    SDL_Surface* surf = SDL_CreateSurface(sk.width, sk.height, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return false;
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf);
    if (!r) {
        SDL_DestroySurface(surf);
        return false;
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    bool ok = false;
    {
        SkinRenderer sr(r);
        sr.upload(f);
        SkinTimers timers;
        for (int id : {0, 1, 40, 41}) timers.set(id, 0);
        PlaySkinState st;
        st.now_ms = 5000;
        st.timers = &timers;
        // 読み込んだときのオプションのまま（遊んでいる間に変わるものは、読み込んだときの値で決める）
        st.option = [&](int op) {
            if (op == 0) return true;
            const int i = op < 0 ? -op : op;
            // 80 読み込み中 / 81 読み込み完了 は、読み込みが終わったあとにする
            const bool v = i == 81 || (i != 80 && i < 1000 && sk.op[i]);
            return op < 0 ? !v : v;
        };
        st.number = [](int) { return -1; };
        st.bargraph = [](int, double&, double&) { return false; };
        st.slider = [](int, int&, int&, int&) { return false; };
        st.text = [](int) { return std::string(); };
        st.button = [](int) { return 0; };
        sr.draw_play(f, st);
        SDL_RenderPresent(r);
        ok = save_png(surf, path);
    }
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surf);
    return ok;
}

}  // namespace

bool export_skin_parts(const Lr2Skin& sk, const std::string& out_dir, std::string& error) {
    const fs::path root = fs::u8path(out_dir);
    std::error_code ec;
    for (const char* d : kPartDir) fs::create_directories(root / d, ec);
    if (ec) {
        error = "フォルダを作れない: " + out_dir;
        return false;
    }

    const float judge_bottom = judge_line_bottom(sk);

    std::vector<Element> els;
    auto add = [&](Part p, std::string name, std::string what, const SkinSrc* s, const SkinDst* d) {
        if (!s || !s->valid) return;
        Element e;
        e.part = p, e.name = std::move(name), e.what = std::move(what), e.src = s, e.dst = d;
        els.push_back(std::move(e));
    };

    struct NoteKind {
        const std::array<SkinSrc, 20>* arr;
        const char* name;
        const char* what;
    };
    const NoteKind notes[] = {{&sk.note, "note", "#SRC_NOTE"},
                              {&sk.ln_start, "ln_start", "#SRC_LN_START"},
                              {&sk.ln_body, "ln_body", "#SRC_LN_BODY"},
                              {&sk.ln_end, "ln_end", "#SRC_LN_END"},
                              {&sk.mine, "mine", "#SRC_MINE"},
                              {&sk.auto_note, "auto_note", "#SRC_AUTO_NOTE"},
                              {&sk.auto_ln_start, "auto_ln_start", "#SRC_AUTO_LN_START"},
                              {&sk.auto_ln_body, "auto_ln_body", "#SRC_AUTO_LN_BODY"},
                              {&sk.auto_ln_end, "auto_ln_end", "#SRC_AUTO_LN_END"},
                              {&sk.auto_mine, "auto_mine", "#SRC_AUTO_MINE"}};
    for (const NoteKind& k : notes)
        for (int lane = 0; lane < 20; ++lane)
            add(kNotes, std::string(k.name) + "_" + lane_name(lane), k.what, &(*k.arr)[lane], nullptr);

    static const char* const kJudgeName[6] = {"poor_empty", "poor", "bad", "good", "great", "pgreat"};
    for (int p = 0; p < 2; ++p)
        for (int j = 5; j >= 0; --j) {
            const std::string side = p ? "_2p" : "";
            add(kJudge, std::string("judge_") + kJudgeName[j] + side, "#SRC_NOWJUDGE", &sk.nowjudge[p][j].src, &sk.nowjudge[p][j].dst);
            add(kJudge, std::string("combo_") + kJudgeName[j] + side, "#SRC_NOWCOMBO", &sk.nowcombo[p][j].src, &sk.nowcombo[p][j].dst);
        }

    std::vector<bool> is_frame_image(sk.image_objs.size(), false);
    for (size_t i = 0; i < sk.image_objs.size(); ++i) {
        const SkinObject& o = sk.image_objs[i];
        std::string name;
        Part p = part_of_image(o, judge_bottom, name);
        if (p == kFrame) is_frame_image[i] = true;
        else add(p, name, "#SRC_IMAGE", &o.src, &o.dst);
    }
    for (int i = 0; i < 2; ++i) {
        const std::string side = i ? "_2p" : "";
        add(kGauge, "gauge" + side, "#SRC_GROOVEGAUGE", &sk.gauge[i].src, &sk.gauge[i].dst);
        add(kLane, "judgeline" + side, "#SRC_JUDGELINE", &sk.judgeline[i].src, &sk.judgeline[i].dst);
        add(kLane, "line" + side, "#SRC_LINE", &sk.line[i].src, &sk.line[i].dst);
    }
    for (const SkinObject& o : sk.sliders)
        if (o.src.op[2] == 4 || o.src.op[2] == 5) add(kCover, o.src.op[2] == 5 ? "cover_2p" : "cover", "#SRC_SLIDER", &o.src, &o.dst);

    // 絵を書き出す。同じセットの中で同じ所を切り出すものは 1 回だけ（名前は最初のもの）
    std::map<std::tuple<int, int, int, int, int, int, int, int>, std::vector<std::string>> written;
    std::set<std::string> used;
    for (Element& e : els) {
        const SkinSrc* s = e.src;
        if (s->gr < 0 || s->gr >= static_cast<int>(sk.images.size())) continue;
        const SkinImage& img = sk.images[s->gr];
        // 幅・高さの -1 は画像全体
        const int sw = s->w < 0 ? img.w : s->w, sh = s->h < 0 ? img.h : s->h;
        if (img.rgba.empty() || sw <= 0 || sh <= 0) continue;
        e.fw = sw / s->div_x, e.fh = sh / s->div_y;
        auto key = std::make_tuple(static_cast<int>(e.part), s->gr, s->x, s->y, sw, sh, s->div_x, s->div_y);
        if (auto it = written.find(key); it != written.end()) {
            e.files = it->second;
            continue;
        }
        std::string base = std::string(kPartDir[e.part]) + "/" + e.name;
        for (int n = 2; used.count(base); ++n) base = std::string(kPartDir[e.part]) + "/" + e.name + "_" + std::to_string(n);
        used.insert(base);
        const int frames = std::min(s->graphcount(), 64);
        for (int f = 0; f < frames; ++f) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), frames == 1 ? ".png" : "_%02d.png", f);
            const std::string rel = base + suffix;
            if (save_crop(img, s->x + (f % s->div_x) * e.fw, s->y + (f / s->div_x) * e.fh, e.fw, e.fh, root / fs::u8path(rel)))
                e.files.push_back(rel);
        }
        written[key] = e.files;
    }

    const bool frame_ok = render_frame(sk, is_frame_image, root / "frame" / "frame.png");

    // 一覧（index.html に埋め込む）。番号・タイマー・op は全部残す
    std::string js = "{\"title\":" + json_str(sk.header.title) + ",\"w\":" + std::to_string(sk.width) +
                     ",\"h\":" + std::to_string(sk.height) + ",\"frame\":" + (frame_ok ? "\"frame/frame.png\"" : "null") +
                     ",\"parts\":[";
    for (int p = 0; p < kPartCount; ++p) js += (p ? "," : "") + json_str(kPartDir[p]);
    js += "],\"lanes\":[";
    bool first = true;
    for (int lane = 0; lane < 20; ++lane) {
        const SkinDst& d = sk.note_dst[lane];
        if (d.empty()) continue;
        const SkinDstKey& k = d.keys.front();
        char b[128];
        std::snprintf(b, sizeof(b), "%s[%s,%g,%g,%g,%g]", first ? "" : ",", json_str(lane_name(lane)).c_str(), k.x, k.y, k.w, k.h);
        js += b;
        first = false;
    }
    js += "],\"els\":[";
    for (size_t i = 0; i < els.size(); ++i) {
        const Element& e = els[i];
        char b[512];
        std::snprintf(b, sizeof(b), "%s{\"part\":%d,\"fw\":%d,\"fh\":%d,", i ? "," : "", e.part, e.fw, e.fh);
        js += b;
        js += "\"name\":" + json_str(e.name) + ",\"what\":" + json_str(e.what) + ",\"files\":[";
        for (size_t f = 0; f < e.files.size(); ++f) js += (f ? "," : "") + json_str(e.files[f]);
        js += "]";
        const SkinSrc& s = *e.src;
        std::snprintf(b, sizeof(b),
                      ",\"src\":{\"gr\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"div\":[%d,%d],\"cycle\":%d,\"timer\":%d,"
                      "\"op\":[%d,%d,%d,%d,%d]}",
                      s.gr, s.x, s.y, s.w, s.h, s.div_x, s.div_y, s.cycle, s.timer, s.op[0], s.op[1], s.op[2], s.op[3], s.op[4]);
        js += b;
        if (e.dst && !e.dst->empty()) {
            const SkinDst& d = *e.dst;
            std::snprintf(b, sizeof(b), ",\"dst\":{\"timer\":%d,\"loop\":%d,\"op\":[%d,%d,%d],\"keys\":[", d.timer, d.loop, d.op[0],
                          d.op[1], d.op[2]);
            js += b;
            for (size_t k = 0; k < d.keys.size(); ++k) {
                const SkinDstKey& q = d.keys[k];
                std::snprintf(b, sizeof(b), "%s[%d,%g,%g,%g,%g,%d,%d,%d,%d,%d,%g]", k ? "," : "", q.time, q.x, q.y, q.w, q.h, q.a, q.r,
                              q.g, q.b, q.blend, q.angle);
                js += b;
            }
            js += "]}";
        }
        js += "}";
    }
    js += "]}";

    std::ofstream html(root / "index.html", std::ios::binary);
    if (!html) {
        error = "index.html を書けない";
        return false;
    }
    static const char* const kHead = R"HTML(<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>スキンの部品</title>
<style>
:root{--bg:#16171b;--card:#202228;--line:#33363f;--fg:#e6e6ea;--sub:#9a9cab;--acc:#5fb3ff;--hl:#ff4f7a}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.5 system-ui,"Yu Gothic UI",sans-serif}
header{position:sticky;top:0;z-index:2;background:var(--bg);border-bottom:1px solid var(--line);padding:10px 16px}
h1{font-size:16px;margin:0 0 6px}
nav a{display:inline-block;background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:4px 10px;margin:0 4px 4px 0;text-decoration:none}
main{padding:12px 16px}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin-bottom:14px;scroll-margin-top:90px}
h2{font-size:15px;margin:0 0 8px}
.row{display:flex;gap:10px;align-items:center;padding:4px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}
.lbl{flex:0 0 150px;min-width:0}
.lbl b{color:var(--acc);font-weight:600}
.lbl small{display:block;color:var(--sub);word-break:break-all}
.pics{flex:1;display:flex;flex-wrap:wrap;gap:2px;min-width:0;padding:4px;border-radius:4px;
 background:repeating-conic-gradient(#2c2e35 0 25%,#24262c 0 50%) 0 0/16px 16px}
.pics img{max-width:100%;max-height:200px;image-rendering:pixelated}
.big{width:100%;display:block;border:1px solid var(--line)}
.map{position:relative;width:100%;max-width:640px;background:#0d0e10;border:1px solid var(--line);overflow:hidden}
.map div{position:absolute;border:1px solid var(--hl);background:rgba(255,79,122,.25);font-size:10px;text-align:center;color:#fff}
.sub{color:var(--sub)}
</style></head><body>
<header><h1 id="t"></h1><nav id="nav"></nav></header><main id="m"></main>
<script>
const D=)HTML";
    static const char* const kTail = R"HTML(;
const NAME={notes:"ノーツ",judge:"判定",bomb:"ボム",keybeam:"キービーム",key:"鍵盤",gauge:"ゲージ",lane:"レーン",cover:"レーンカバー",frame:"フレーム"};
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
document.getElementById("t").textContent=D.title+"（"+D.w+"×"+D.h+"）";
const nav=document.getElementById("nav"),m=document.getElementById("m");
D.parts.forEach((key,p)=>{
 const items=D.els.filter(e=>e.part==p);
 if(key!="frame"&&key!="lane"&&!items.length)return;
 const a=document.createElement("a");a.href="#"+key;a.textContent=NAME[key];nav.append(a);
 const sec=document.createElement("section");sec.id=key;
 let h="<h2>"+NAME[key]+(items.length?' <span class="sub">'+items.length+"</span>":"")+"</h2>";
 if(key=="frame")h+=D.frame?'<img class="big" src="'+D.frame+'"><p class="sub">ノーツ・判定・ボム・キービーム・鍵盤・ゲージ・レーンカバー以外を、曲が始まって少し経ったところで描いたもの（数字と文字は除く）</p>':'<p class="sub">描けなかった</p>';
 if(key=="lane"&&D.lanes.length){
  h+='<div class="map" style="aspect-ratio:'+D.w+"/"+D.h+'">'+D.lanes.map(l=>'<div style="left:'+(l[1]/D.w*100)+"%;top:0;width:"+(l[3]/D.w*100)+"%;height:"+((l[2]+l[4])/D.h*100)+'%">'+l[0]+"</div>").join("")+"</div>";
 }
 // 同じ絵を使うものは 1 行にまとめる（ノーツの 1・3・5・7 鍵など）
 const groups=[];const byFile={};
 items.forEach(e=>{const k=e.files.length?e.files.join("|"):"#"+e.name;
  if(byFile[k])byFile[k].push(e);else groups.push(byFile[k]=[e]);});
 groups.forEach(g=>{
  const e=g[0],s=e.src,frames=s.div[0]*s.div[1];
  const names=g.map(x=>x.name).join(" / ");
  let info=esc(e.what)+(frames>1?"<br>"+s.div[0]+"×"+s.div[1]+"コマ"+(s.cycle?" / "+s.cycle+"ms":""):"")+(e.dst?"<br>タイマー "+e.dst.timer+(e.dst.op.some(x=>x)?" op "+e.dst.op.join(","):""):"");
  h+='<div class="row"><div class="lbl"><b>'+esc(names)+"</b><small>"+info+'</small></div><div class="pics">'+
   e.files.map(f=>'<img loading="lazy" src="'+f+'">').join("")+"</div></div>";
 });
 sec.innerHTML=h;m.append(sec);
});
</script></body></html>
)HTML";
    html << kHead << js << kTail;
    std::printf("部品 %zu 個を書き出した: %s\n", els.size(), out_dir.c_str());
    return true;
}

}  // namespace bmj
