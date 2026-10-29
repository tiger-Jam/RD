# MOD の作り方

BMjoy の MOD は、1 つのフォルダに定義ファイル `mod.json` と、必要ならスクリプト（Lua 5.4）を入れたものです。
DLL などのプログラムは読みません。

## 置き場所と選び方

| 場所 | 中身 |
|---|---|
| 実行ファイルの横の `mods/` | 公式の MOD（BMjoy に付いてくる） |
| `LR2files/Mods/ModData/<MOD の id>/` | MOD が書いてよい場所（ここ以外には書けない） |

今は公式の MOD だけを読みます（自分で入れる MOD は読みません）。

設定アプリの「MOD」タブで、使う MOD に印を付けます。組み合わせに問題があるときは、タブの下に警告が出ます。
例：判定を変える MOD が 2 つある、など。

## mod.json

```jsonc
{
  "id": "my-mod",            // 英数字とハイフン。ほかの MOD と重ならないもの
  "name": "わたしの MOD",
  "version": "1",
  "author": "名前",
  "description": "一行の説明",
  "touches": ["visual"],     // 触れる場所（下の表）。宣言していない窓口は呼ばれない
  "script": "main.lua"       // スクリプト（無ければ書かない）
}
```

### 触れる場所（touches）

| 値 | 中身 | 記録 |
|---|---|---|
| `visual` | 画面に描く（`on_draw`） | ふだんの記録のまま |
| `sound` | 音（窓口はまだ無い） | ふだんの記録のまま |
| `control` | 選曲のフォルダを作る（`build_folders`）・フォルダにバーを足す（`folder_bars`） | ふだんの記録のまま |
| `evaluation` | ゲームの状態を読む・カスタムゲージ・GAS | ふだんの記録のまま |
| `judge` | カスタム判定 | **記録を分ける** |
| `chart` | 譜面の変換（窓口はまだ無い） | **記録を分ける** |
| `input` | 入力の処理（窓口はまだ無い） | **記録を分ける** |

`judge`・`chart`・`input` の MOD を使うと、記録は「その MOD の組み合わせ」の記録として別に残ります。
組み合わせは、MOD の id・版・フォルダの中身のハッシュで区別します。選曲画面には、今の組み合わせでのベストが出ます。
MOD を外せば、ふだんのベストに戻ります。LR2 から引き継いだ記録は、MOD を使っていないときだけ出ます。

`gauges`・`lamps` を書けば `evaluation`、`judges` を書けば `judge` が、宣言しなくても足されます。

## カスタムゲージとカスタムランプ（定義だけで作れる）

```jsonc
"gauges": [
  {
    "id": "exhard",
    "name": "EX-HARD",
    "frame": 7,              // スキンの GAUGE のボタンの絵のコマ（無ければ書かない。絵に無いときは名前を文字で出す）
    "initial": 100,          // 始まりの値（%）
    "border": 80,            // survival が false のとき、曲の終わりにこれ以上ならクリア
    "survival": true,        // true：2% を割ったら落ちる（HARD 系）。false：最後まで遊ぶ（GROOVE 系）
    "floor": 2,              // survival が false のとき、これより下がらない
    "delta": {               // 判定ごとの増減（%）
      "pgreat": 0.15,
      "great": 0.06,
      "good": 0,
      "bad": -8,
      "poor": -16,           // 見逃し
      "empty_poor": -8       // 空 POOR
      // { "total": 1.0 } と書くと「1.0 × TOTAL ÷ 総ノーツ数」（GROOVE の増え方）。{ "fixed": 0.1, "total": 0.5 } のように足せる
    },
    "mine": 1.0,             // 地雷のダメージの倍率
    "low_hp": { "below": 30, "damage": 0.6 },  // 残りが 30% 以下なら減る量を 0.6 倍（LR2 の HARD と同じ）
    "colors": 1,             // ゲージの色の数。2 ならクリアの線より上が color、下が color_low（落ちたら終わりのゲージは color だけ）
    "color": "#ffd800",
    "color_low": "#30a0ff",
    "lamp": "exhard"         // クリアしたときのランプ（下の lamps の id、ほかの MOD なら「MOD の id/ランプの id」、本体の FAILED・EASY・CLEAR・HARD）
  }
],
"lamps": [
  {
    "id": "exhard",
    "name": "EX-HARD",
    "color": "#ffd800",      // 選曲画面のランプの色（スキンのランプの絵をこの色に染める）
    "colors": 2,             // 2 なら color と color2 を blink（ms）ごとに入れ替えて点滅させる
    "color2": "#ffffff",
    "blink": 500,
    "rank": 4.5              // 強さ（1 FAILED、2 EASY、3 CLEAR、4 HARD、5 FULL COMBO、6 PERFECT の間の値。設定画面では強い順の一覧で上へ・下へ動かす）。
                             // スキンの絵と、ふだんの記録に残すランプは、すぐ下の本体のランプ（4.5 なら HARD）
  }
]
```

- **ゲージの選び方**：選曲画面のゲージの切り替えで、LR2 の 6 種類のあとに、使っている MOD のゲージが並びます。
- **番号の付くゲージ**：MOD に `"gauge_label": "CUSTOM", "gauge_frame": 9` と書くと、ゲージの名前は並びの順に CUSTOM1〜CUSTOM9、絵のコマは 9〜17 になります（公式の「カスタムゲージ」MOD）。
- **見本**：設定画面のゲージ・ランプの一覧の頭に、本体のゲージ・ランプが灰色で並びます（変えられない）。選んで「足す」と、その値から作れます。
  HARD 系の減り方はノーツが少ない譜面ほど大きくなりますが、見本の値は補正のかからない 1000 ノーツ以上のものです。
- **ゲージの色**：2 色のときは 80% 未満が `color_low`、80% 以上が `color`（スキンのゲージの絵の色の境が 80% のため）。遊んでいる間のゲージと、リザルトのゲージのグラフを、スキンの絵を灰色にしてから指定の色に染めて描きます。
- **裏での計算**：使っている MOD のゲージは、どれで遊んでいても全部を裏で計算します。結果は 1 回ごとのプレーの表に残ります。
- **ランプ**：遊んでいるゲージで決めます。カスタムゲージで遊んだときは、次の 2 つを残します。
  - ふだんの記録：すぐ下の本体のランプ
  - カスタムランプの記録（ランプの鍵ごと。同じランプを付けるゲージどうしは同じ記録）：FAILED・クリア・フルコンボ・PERFECT。本体のランプを付けるゲージは、ふだんの記録だけ
- **選曲画面のランプ**：カスタムランプでクリアしていて、その `rank` がふだんのランプより上なら、すぐ下の本体のランプの絵をランプの色に染めて出します。
- **フルコンボ**：どのゲージで遊んでもフルコンボです。
- **コース**：カスタムゲージでは遊べず、LR2 のゲージになります。
- **前の書き方**：ゲージの中に `"lamp": { "name": …, "color": … }` と書いたものも読めます（ゲージと同じ id のランプになります）。

## カスタム判定（定義だけで作れる）

```jsonc
"judges": [
  {
    "id": "late-wide",
    "name": "遅い側が広い",
    "pgreat": [18, 22],      // [早い側, 遅い側]（ms）。1 つの数なら両側とも同じ
    "great": [40, 48],
    "good": [100, 110],
    "bad": [200, 200],
    "empty_poor": 1000,      // 早い側の空 POOR の幅
    "rank_scaled": true      // true なら PGREAT〜GOOD を譜面の #RANK に合わせて伸び縮みさせる（LR2 と同じく、BAD と空 POOR は変えない）
  }
]
```

判定を変える MOD は 1 つだけ使えます。

設定に `{ "type": "judges" }` を持つ MOD（公式の「カスタム判定」）は、判定を起動の設定画面の詳細で作り、遊ぶ判定を 1 つ選びます
（`judge_use`。選ばなければ判定は変わらず、記録も分けません）。記録は、選んだ判定の幅の値ごとに分けて残します。
幅を変えれば別の記録になり、戻せば前の記録が出ます。名前を変えても記録は同じです。
設定画面の一覧の頭には、本体の判定（#RANK どおり・VERY HARD・HARD・NORMAL・EASY）が灰色で並びます。選んで「足す」と、その幅から作れます。
選曲画面では、スキンのボタン 900（BMjoy で足したもの）を押すと、遊ぶ判定を OFF → JUDGE1 → … → JUDGE9 → OFF の順に替えます
（JUDGE の番号は設定画面の一覧の順。判定は 9 個まで）。W-MIX には `tools/skin/wmix_gauge_frames.py` で設定パネルの左上に欄を足せます。

## GAS

`"gas": true` と書くと、選曲画面の GAUGE に「GAS」が増えます（MOD を入れただけでは効きません）。
GAS を選ぶと、`gas_order` の先頭のゲージで始め、尽きたら次のゲージに替えて続けます。曲の終わりにクリアの線に届いていなければ、
クリアできた一番上のゲージになります。コースでは使いません。

```json
"gas": true,
"gas_order": ["HARD", "GROOVE", "EASY"]
```

本体のゲージは `GROOVE` `HARD` `HAZARD` `EASY` `P-ATTACK` `G-ATTACK`、カスタムゲージは「MOD の id/ゲージの id」で書きます。
公式の「GAS」MOD がこれです。

## メトロノーム（定義だけで作れる）

```json
"metronome": { "accent": "accent.wav", "beat": "beat.wav", "volume": 400 }
```

遊んでいる間、譜面の拍に合わせて音を鳴らします。小節の頭は `accent`、ほかの拍は `beat`（どちらも MOD のフォルダの中の音）。

- 拍は BPM と STOP から数えます（CONSTANT で速さを変えても BPM どおり）。小節の頭は小節線の時刻で、3/4 の小節なら 3 拍鳴ります。小節線の無い譜面は 4 拍ごと。
- 曲の BGM と同じ時計で鳴らすので、FREQ で速さを変えても拍からずれません（音の高さも曲と一緒に変わります）。
- 30 ms より詰まった拍（極端に速い BPM）は飛ばします。
- `volume` は音量（%。0〜500）。公式の音は 500% で割れない大きさに作ってあるので、400% くらいがふつう。設定に `{ "type": "metronome" }` を持てば、起動の設定画面の詳細で変えられます。
- `metronome` を書けば `sound` が足されます。プレーには影響しないので、記録は分けません。

## 部品の色（定義だけで作れる）

プレースキンの部品の絵に、色のフィルターをかけます（スキン選択でほかのスキンから持ってきた部品にも当たります）。スキンを読み込んだとき、当てる部品の絵だけを、色を変えた写しに差し替えます
（同じ画像の中のほかの部品は元のまま）。上から順に当てます。

```json
"skin_colors": [
  { "parts": ["note"], "lanes": ["white", "blue"], "mode": "map", "color": "#23d100" },
  { "parts": ["bomb", "hold"], "lanes": ["scratch"], "mode": "hue", "hue": 120 },
  { "parts": ["judge", "combo"], "judges": ["pgreat"], "mode": "range", "from": 210, "width": 35, "hue": 150 }
]
```

| 項目 | 中身 |
|---|---|
| `parts` | `note`・`ln`（始点・中・終点）・`mine`・`bomb`・`hold`（LN を押している間のボム）・`beam`（キービーム）・`key`（鍵盤）・`judge`（判定の文字）・`combo`（コンボの数字）・`judgeline`・`line`（小節線）・`gauge`・`frame`（そのほかの絵と数字） |
| `lanes` | `scratch`・`white`（1・3・5・7 鍵）・`blue`（2・4・6 鍵）か、鍵盤の番号。無ければ全部。ノーツ・ボム・キービーム・鍵盤だけに効く |
| `judges` | `pgreat`・`great`・`good`・`bad`・`poor`・`empty_poor`。無ければ全部。`judge` と `combo` だけに効く |
| `skins` | その部品が元々どのスキンのものか（`.lr2skin`。実行ファイルのフォルダからの相対か絶対）。スキン選択でほかのスキンへ持っていった部品にも効き、ほかのスキンから持ってきた部品には効かない。無ければ全部のスキン |
| `mode` | `hue`：色相をずらす（`hue` 度、`sat` 彩度 %、`lit` 明るさ -50〜50）。白・黒・灰色は変わらない<br>`range`：`from` 度の色に近い所（`width` 度まで）だけ色相をずらす（`hue`・`sat`）<br>`map`：明るさを「暗い所 → `color` → 白」に置き換える（`mix` 強さ %、`white` いちばん明るい所の白さ %。0 にすると白い所も `color` になる）<br>`mul`：灰色にしてから `color` を掛ける（`mix`） |

- 部品はタイマーで見分けます（ボムは 50〜69、LN のボムは 70〜89、キービームと鍵盤は 100〜139 で、判定ラインより上で光るものがキービーム）。
- 選曲画面のスキンの試し見にも効きます。`skin_colors` を書けば `visual` が足されます。記録は分けません。
- 設定に `{ "type": "skin_colors", "name": "部品の色" }` を持てば、起動の設定画面の詳細で、部品・レーン・判定・方式・色を変えられます（`settings.json` に残り、`mod.json` の `skin_colors` より優先）。

## 詳細の設定（settings）

起動の設定画面の MOD のタブで、MOD を選んで「詳細」を押すと変えられる項目です。今は次の 5 つです。2 つ以上あれば、詳細の上のボタンで切り替えます。

| type | 変えるもの |
|---|---|
| `gauge_order` | `gas_order`（GAS のゲージの順番） |
| `gauges` | `gauges`（カスタムゲージの一覧。足す・直す・消す） |
| `lamps` | `lamps`（カスタムランプの一覧。足す・直す・消す） |
| `judges` | `judges`（カスタム判定の一覧。足す・直す・消す）と `judge_use`（遊ぶ判定） |
| `metronome` | `metronome_volume`（メトロノームの音量） |

```json
"settings": [{ "type": "gauges", "name": "ゲージ" }, { "type": "lamps", "name": "ランプ" }]
```

`gauges` の設定を持つ MOD では、本体のゲージ（GROOVE〜G-ATTACK）の色も変えられます（`standard_gauges`。「スキンの絵のまま・1 色・2 色」）。
`lamps` の設定を持つ MOD では、本体のランプの色（`standard_lamps`）と、使っているほかの MOD のランプ（EX-HARD など）の色（`lamp_colors`）も変えられます。
どれも、その MOD を使っているときだけ効きます。

変えた値は `LR2files/Mods/ModData/<MOD の id>/settings.json` に残り、mod.json の値の代わりに使われます。設定画面で変えると、その場で書きます。
mod.json に書いた値は、最初の中身です。

## スクリプト

スクリプトには、決まった名前の関数を書きます。本体は、その名前の関数だけを呼びます。
渡す値はゲームの状態の写しなので、書き換えてもゲームには効きません。

| 関数 | いつ | 宣言が要るもの |
|---|---|---|
| `on_play_start(info)` | 曲が始まる前 | なし |
| `on_judge(e)` | 判定が出たとき・地雷を踏んだとき | なし |
| `on_play_end(r)` | リザルトに入るとき | なし |
| `on_draw(s)` | プレー中、毎フレーム | `visual` |
| `build_folders(charts)` | 選曲の一覧を作るとき | `control` |
| `folder_bars(folder)` | フォルダを開いたとき | `control` |
| `pick_chart(req)` | `folder_bars` のバーを決定したとき | `control` |

### info（on_play_start）

| 値 | 中身 |
|---|---|
| `sha256` `md5` `path` | 譜面の識別とファイル |
| `title` `subtitle` `artist` `genre` | 譜面の文字 |
| `playlevel` `notes` `total` `rank` `keys` `length_ms` `bpm_min` `bpm_max` | 譜面の数字（`notes` は判定するノーツの数） |
| `gauge` | 遊ぶゲージ（`"GROOVE"` `"HARD"` `"HAZARD"` `"EASY"` `"P-ATTACK"` `"G-ATTACK"`、またはカスタムゲージの `"MOD の id/ゲージの id"`） |
| `random` | 配置のオプション（0 OFF、1 MIRROR、2 RANDOM、3 S-RANDOM、4 SCATTER、5 CONVERGE） |
| `autoplay` `replay` `course` | オートプレイ・リプレイの再生・コースのステージか |
| `view_w` `view_h` | 画面（スキン）の大きさ |
| `windows` | 判定の幅。`windows.pgreat[1]` が早い側、`[2]` が遅い側（ms）。`great` `good` `bad` も同じ |

### e（on_judge）

| 値 | 中身 |
|---|---|
| `time_ms` | 曲の時刻 |
| `lane` | 0 皿、1〜7 鍵盤 |
| `judge` | 0 空POOR、1 POOR、2 BAD、3 GOOD、4 GREAT、5 PGREAT（`bmj.JUDGE.PGREAT` なども使える） |
| `name` | `"PGREAT"` など。地雷なら `"MINE"` |
| `offset_ms` | 押した時刻 − ノーツの時刻（正が遅い）。見逃しなど、ずれが無いときは `nil` |
| `fast` `mine` `ln_end` | 早かったか・地雷か・LN の終点か |
| `combo` `exscore` `hp` | そのフレームの判定を全部数えたあとの値 |

### r（on_play_end）

| 値 | 中身 |
|---|---|
| `clear` | ランプ（1 FAILED … 6 PERFECT） |
| `exscore` `bp` `max_combo` `total_notes` | 結果の数字 |
| `fast` `slow` `quit_early` | 速い・遅いの数、途中でやめたか |
| `judges` | `judges.PGREAT` など。`POOR` には空 POOR を含めない（空 POOR は `EMPTY_POOR`） |
| `gauges` | 裏で計算した全部のゲージ（`gauges.HARD`、`gauges["exhard/exhard"]` など）。途中で落ちたときは、落ちた時点の値 |

### s（on_draw）

`now_ms`（画面の時刻）、`song_ms`（曲の時刻）、`combo`、`exscore`、`hp`、`view_w`、`view_h`。

### charts（build_folders）

持っている譜面の配列です。1 つの譜面には次の値が入っています。

- 譜面の値：`sha256` `md5` `path` `folder` `title` `subtitle` `artist` `genre` `playlevel` `difficulty` `notes` `ln` `rank` `total` `keys` `bpm_min` `bpm_max` `bpm_main` `length_ms` `scratch` `added`
- 記録の値：`clear` `exscore` `bp` `playcount`（記録が無ければ `clear` は 0）

戻り値はフォルダの配列です。

```lua
return {
  { title = "フォルダの名前", charts = { "sha256", ... }, folders = { { title = "下のフォルダ", charts = { ... } } } },
}
```

譜面は `{ sha256 = "…", label = "72.4%" }` とも書けます。`label` は一覧の曲名の前に出ます。
フォルダに `keep_order = true` を付けると、選曲の並べ替えの設定にかかわらず、`charts` の順のまま並びます。

### folder・req（folder_bars・pick_chart）

フォルダを開くと、一番上にフォルダの名前のバーが出ます。`folder_bars` が返したバーは、その下に並びます。

- `folder`：`title`（フォルダの名前）、`charts`（中の譜面の数）。同じフォルダでは 1 回だけ呼ばれる。
- 戻り値：バーの配列 `{ { key = "random", title = "?????", roulette = true }, ... }`（8 本まで）。`key` は `pick_chart` に渡る。
  `roulette = true` にすると、カーソルを置いている間、曲名の枠に一覧の曲名が次々に出る。

バーを決定すると `pick_chart(req)` が呼ばれます。

- `req`：`key`（バーの key）、`folder`（フォルダの名前）、`charts`（今の一覧に見えている譜面。鍵盤数・難易度の絞り込みのあと。値は `build_folders` の charts と同じ）、`shown`（`roulette` のバーのとき、決定した瞬間に曲名の枠に出ていた譜面の sha256）。
- 戻り値：遊ぶ譜面の `sha256`。カーソルがその譜面に移り、そのまま決定画面へ進みます。`nil` なら何もしません。

見本：`mods/random-select`（ランダムセレクト）。

### bmj の表

| 関数 | 中身 |
|---|---|
| `bmj.log(...)` / `print(...)` | 黒い画面に出す |
| `bmj.write_file(名前, 文字)` | `LR2files/Mods/ModData/<id>/名前` に書く。書けたら `true`、書けなければ `nil, 理由`。名前には `/` で下のフォルダを付けられる。`..` や絶対パスは使えない |
| `bmj.append_file(名前, 文字)` | 後ろに足す |
| `bmj.read_file(名前)` | 自分が書いたファイルを読む（無ければ `nil`） |
| `bmj.read_mod_file(名前)` | MOD のフォルダの中のファイルを読む（設定や画像の情報など） |
| `bmj.time()` | 今の時刻（UNIX 秒） |
| `bmj.date(書式, 時刻)` | 日時の文字（書式は C の strftime。既定 `"%Y-%m-%d %H:%M:%S"`） |
| `bmj.draw_rect(x, y, w, h, 色, 不透明度)` | 四角を描く（`on_draw` の中だけ）。色は `0xRRGGBB`、不透明度は 0〜255 |
| `bmj.draw_text(x, y, 文字, 大きさ, 色, 不透明度, 揃え)` | 文字を描く。揃えは 0 左、1 中央、2 右 |
| `bmj.id` | この MOD の id |

### 使えるもの・制限

- **使えるライブラリ**：Lua の base・table・string・math・utf8・coroutine。
- **外してある関数**：`io`・`os`・`require`・`dofile`・`loadfile`・`load` は無い。
- **命令数の上限**：1 回の呼び出しで動かせる量に上限がある（`build_folders` だけは多め）。超えるとその MOD はそのプレーのあいだ止まり、黒い画面に理由が出る。
- **メモリ**：1 つの MOD が使えるのは 256MB まで。
- **呼ばれ方**：MOD ごとに別の Lua の環境で動く。プレーのたびに作り直すので、プレーをまたいで残したいものはファイルに書く。

## 公式の MOD

| MOD | 中身 |
|---|---|
| GAS | GAUGE で GAS を選ぶと、ゲージが尽きたら決めた順番の次のゲージに替えて続ける。順番は詳細で変える |
| EX-HARD | GAUGE に EX-HARD を足す（ランプも EX-HARD） |
| ASSIST EASY | GAUGE に ASSIST EASY を足す（ランプも ASSIST EASY） |
| カスタムゲージ | 自分でゲージ（CUSTOM1〜9）とランプを作る。詳細で足す・直す・消す |
| カスタム判定 | 自分で判定の幅を作り、詳細で選んだ判定で遊ぶ。記録は判定の幅ごとに分かれる |
| メトロノーム | 譜面の拍に合わせてメトロノームの音を鳴らす。音量は詳細で変える |
| 判定のずれのバー | 押したずれを横のバーに印で出す。置き場所は `main.lua` の頭で変える |
| 判定の記録を書き出す | 1 回のプレーの判定を全部 CSV に書き出す |
| 推定難易度のフォルダ | ノーツの密度から難易度をざっくり推定して、段階ごとのフォルダに分ける |
