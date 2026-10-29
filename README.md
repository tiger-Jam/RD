# Revenant Deliria（RD）

LR2の挙動を既定にした、7K+S専用のBMSプレイヤー。仕様は `次世代BMSプレイヤー 仕様書 v0.1.md`。

いまは、選曲画面（難易度表・曲フォルダ）から選んで、LR2のプレイスキンで遊べるところまで。

## ビルド（macOS）

```
brew install cmake ninja sdl3
cmake -S . -B build -G Ninja
cmake --build build
./build/rd_tests
```

## ビルド（Windows）

Visual Studio 2022（C++ のデスクトップ開発）と CMake があればよい。SDL3 はビルドの中でソースを取ってきて一緒にビルドする。

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\rd_tests.exe
build\RelWithDebInfo\RDbody.exe --lr2-root C:\path\to\LR2
```

- HTTP は Windows では WinHTTP を使う（libcurl は不要）
- 日本語のファイル名を扱えるように、マニフェストでコードページを UTF-8 にしている（OpenLR2 と同じ）
- 設定・スコアは `%APPDATA%\RD` に保存される

### Mac から Windows 機でビルド・起動する

`ssh win` で入れる Windows 機があれば、Mac から操作できる（Windows 側は `%USERPROFILE%\BMjoy-beta20261022` を使う）。

```
tools/win.sh sync               # いまの作業ツリーを送ってビルド
tools/win.sh test               # Windows でユニットテスト
tools/win.sh run [引数...]       # Windows のデスクトップで起動（ログイン中の画面に出る）
tools/win.sh log                # 最後に起動したときのログ
```

SSH から直接起動すると画面の無いセッションで動いてしまうので、`run` はタスク スケジューラ経由でログイン中のユーザーの画面に起動する。

### 開発用の口と配る版

`RD_` で始まる環境変数（画面を撮る `RD_SELECT_SHOT`、操作を流す `RD_SELECT_KEYS`、入力を流す `RD_INPUT_REPLAY` など）は、開発用の版（既定）でだけ読む。配る版は `cmake -S . -B build-rel -DRD_DEV=OFF` でビルドし、どれも読まない。入力を作る口が残っていると、記録の不正に使えるため。

## 遊ぶ

LR2 と同じく、設定アプリ `RD`（LR2.exe に相当）と本体 `RDbody`（LR2body.exe に相当）に分かれている。
`RD` で曲フォルダ・難易度表・画面などを設定して「起動」を押すと、黒い画面（Windows はコンソール、Mac はターミナル）つきで本体が立ち上がる。
本体を直接起動すれば、設定を飛ばしてすぐ遊べる。

```
./build/RDbody --add-song-dir ~/BMS --add-table https://stellabms.xyz/sl/table.html   # 最初に1回だけ
./build/RDbody                                                                      # 選曲画面
```

- 選曲画面：↑↓（皿）で選ぶ、Enter（鍵盤 1・3・5・7）で決定、Esc/←（鍵盤 2）で戻る、→（SELECT）で同じフォルダの上の難易度へ、鍵盤 6 でオートで開始、F2でオート、F6でレーンカバー、F1でキーコンフィグ、F5で曲フォルダと難易度表を更新。START（キーボードは Q か Space）を押している間はプレイオプションのパネル（LR2 と同じく 4=GAUGE、5/7=HI-SPEED、6+7=HIDDEN/SUDDEN、5+7=HS-FIX）
  （コントローラは白鍵で決定、黒鍵で戻る、皿で選ぶ）
- 起動時に、裏で曲フォルダの読み直し（変わったファイルだけ）と難易度表の更新をする。難易度表が取れなくても、最後に取れた版を使う
- スコア（ランプ・EX・BP・プレイ回数）は `songs.db` に記録する。自己ベストのゴーストにも使う

譜面を直接指定して遊ぶこともできる：

```
./build/RDbody 譜面.bme [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas] [--hs 100] [--timing ms]
```

| 操作 | キー |
| --- | --- |
| 鍵盤1〜7 | Z S X D C F V |
| 皿 | 左Shift / 左Ctrl / スペース |
| ハイスピード | ↑ ↓（0.10刻み。端で反対の端へ回る）、START/SELECT＋1・3・5鍵で下げる、＋2・4鍵で上げる |
| レーンカバー | ← →、マウスホイール、START/SELECT＋6・7鍵。START か SELECT の2度押しで出し入れ |
| 判定タイミング | Shift＋← →（1ms刻み。RD の拡張。ノーツの見た目の位置だけをずらす） |
| フルスクリーン | F4 |
| 終了 | ESC（リザルトで F5 を押すともう一度） |

コントローラは LR2 の既定の割り当て（鍵盤 B4 B7 B3 B8 B2 B5 左、皿 上下）。

### キーコンフィグ

`./build/RDbody --keyconfig` で設定画面を開く。設定は `~/Library/Application Support/BMjoy/keyconfig.txt` に保存される。

- ↑↓で項目を選び、Enter を押してから割り当てたいキーやボタンを押す（1つの項目に複数割り当てられる）。Backspace で消す。F1 で LR2 の既定に戻す
- ジョイスティックのボタン番号は0から数える（LR2の BUTTON 4 は `joy:button 3`）
- **アナログ皿**：`ANALOG TT` で Enter を押してから皿を回すと、その軸を使う。回している間は押下、止まって `TT RELEASE`（既定100ms）経ったら離す。LR2は皿を方向ボタンとしてしか読まないので、これはRDの拡張
- 画面下の INPUT TEST で、押されているレーンと軸の生の値を確認できる

### スキン

LR2のフォルダ（`LR2files` があるところ）を `--lr2-root` で指定すると、既定で `LR2files/Theme/LR2/Play/play_7.lr2skin` を使う。
別のスキンは `--skin` で .lr2skin を指定する。スキンの中のパス（`LR2files\Theme\...`）は LR2 のフォルダから探す。

```
./build/RDbody 譜面.bme --lr2-root ~/LR2 --skin ~/LR2/LR2files/Theme/WMIX_HD/play/HDPLAY_W.lr2skin --skin-opt 2=1
```

- 起動時にカスタマイズ項目の一覧（`[番号] 項目 = 選択`）を表示する。`--skin-opt 番号=選択肢の番号` で変える
- `#RESOLUTION` の無いスキンは、画面全体を覆う画像の大きさから SD / HD / FHD を推定する。`--skin-res sd|hd|fhd` で指定もできる
- リザルト画面も LR2 のスキンを使う。`--result-skin` で .lr2skin を指定する（既定は `LR2files/Theme/LR2/Result/result.lr2skin`）。
- 選曲画面も LR2 のスキンを使う。設定ファイルの `select_skin` で .lr2skin を指定する（既定は `LR2files/Theme/LR2/Select/select.lr2skin`、`none` で RD の仮の画面）。選曲スキンに渡す値の仕様は `docs/lr2_select_spec.md`
  決定（Enter・鍵盤）を押すたびに グラフ → ランク → 更新 → フェードアウト と進む。フェードアウトの間に白鍵と黒鍵を押していると、同じ譜面をもう一度遊ぶ
- `--no-skin` で仮の画面になる

### 確認用

- `--info`：譜面の情報を表示する
- `--simulate`：画面も音も出さずに自動演奏で判定を回す
- `python3 tools/make_test_song.py testdata/generated/test_song`：キー音つきのテスト曲を作る

## 参考にしたもの

挙動は OpenLR2（LR2の書き直し）のコードに合わせ、コードは書き直している。対応箇所はコメントに `LR2:` として残している。
