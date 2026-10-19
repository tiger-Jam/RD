# MOD の作り方

BMjoy の MOD は、1 つのフォルダに定義ファイル `mod.json` と、必要ならスクリプト（Lua 5.4）を入れたものです。
DLL などのプログラムは読みません。

## 置き場所と選び方

| 場所 | 中身 |
|---|---|
| 実行ファイルの横の `mods/` | 公式の MOD（BMjoy に付いてくる） |
| 設定フォルダの `mods/` | 自分で入れる MOD（Windows は `%APPDATA%\BMjoy\mods\`、macOS は `~/Library/Application Support/BMjoy/mods/`） |
| 設定フォルダの `moddata/<MOD の id>/` | MOD が書いてよい場所（ここ以外には書けない） |

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
| `control` | 選曲のフォルダを作る（`build_folders`） | ふだんの記録のまま |
| `evaluation` | ゲームの状態を読む・カスタムゲージ・GAS | ふだんの記録のまま |
| `judge` | カスタム判定 | **記録を分ける** |
| `chart` | 譜面の変換（窓口はまだ無い） | **記録を分ける** |
| `input` | 入力の処理（窓口はまだ無い） | **記録を分ける** |

`judge`・`chart`・`input` の MOD を使うと、記録は「その MOD の組み合わせ」の記録として別に残ります。
組み合わせは、MOD の id・版・フォルダの中身のハッシュで区別します。選曲画面には、今の組み合わせでのベストが出ます。
MOD を外せば、ふだんのベストに戻ります。LR2 から引き継いだ記録は、MOD を使っていないときだけ出ます。

`gauges` を書けば `evaluation`、`judges` を書けば `judge` が、宣言しなくても足されます。

## カスタムゲージ（定義だけで作れる）

```jsonc
"gauges": [
  {
    "id": "exhard",
    "name": "EX-HARD",
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
    "lamp": {
      "name": "EX-HARD",
      "color": "#ffd800",
      "rank": 4.5,           // ランプの並び（1 FAILED、2 EASY、3 CLEAR、4 HARD、5 FULL COMBO）
      "as": 4                // スキンや一覧にはこの番号のランプで見せる（スキンは 0〜5 しか知らない）
    }
  }
]
```

- **ゲージの選び方**：選曲画面のゲージの切り替えで、LR2 の 6 種類のあとに、使っている MOD のゲージが並びます。
- **裏での計算**：使っている MOD のゲージは、どれで遊んでいても全部を裏で計算します。結果は 1 回ごとのプレーの表に残ります。
- **ランプ**：遊んでいるゲージで決めます。カスタムゲージで遊んだときは、次の 2 つを残します。
  - ふだんの記録：`as` の番号のランプ
  - そのゲージのランプ：FAILED・クリア・フルコンボ・PERFECT
- **フルコンボ**：どのゲージで遊んでもフルコンボです。
- **コース**：カスタムゲージでは遊べず、LR2 のゲージになります。

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

## GAS

`"gas": true` と書くと、GAS が効きます。遊んでいるゲージが落ちたら、裏で計算している一段下のゲージに替えて続けます。
公式の「GAS」MOD がこれです。

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

### bmj の表

| 関数 | 中身 |
|---|---|
| `bmj.log(...)` / `print(...)` | 黒い画面に出す |
| `bmj.write_file(名前, 文字)` | `moddata/<id>/名前` に書く。書けたら `true`、書けなければ `nil, 理由`。名前には `/` で下のフォルダを付けられる。`..` や絶対パスは使えない |
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
| GAS | ゲージが尽きたら一段下のゲージに替えて続ける |
| EX-HARD | HARD より減りが大きく、残りが少なくても減りが緩まないゲージ |
| ASSIST EASY | EASY より減りが小さく、60% でクリアになるゲージ |
| 判定の見本 | 遅い側を広げたカスタム判定（書き方の見本） |
| 判定のずれのバー | 押したずれを横のバーに印で出す。置き場所は `main.lua` の頭で変える |
| 判定の記録を書き出す | 1 回のプレーの判定を全部 CSV に書き出す |
| 推定難易度のフォルダ | ノーツの密度から難易度をざっくり推定して、段階ごとのフォルダに分ける |
