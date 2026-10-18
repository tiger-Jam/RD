# BMjoy

LR2の挙動を既定にした、7K+S専用のBMSプレイヤー。仕様は `次世代BMSプレイヤー 仕様書 v0.1.md`。

いまは、選曲画面（難易度表・曲フォルダ）から選んで、LR2のプレイスキンで遊べるところまで。

## ビルド（macOS）

```
brew install cmake ninja sdl3
cmake -S . -B build -G Ninja
cmake --build build
./build/bmj_tests
```

## 遊ぶ

```
./build/bmjoy --add-song-dir ~/BMS --add-table https://stellabms.xyz/sl/table.html   # 最初に1回だけ
./build/bmjoy                                                                      # 選曲画面
```

- 選曲画面：↑↓で選ぶ、Enter/→で決定、Esc/←で戻る、TABでゲージ、F2でオート、F5で曲フォルダと難易度表を更新
  （コントローラは白鍵で決定、黒鍵で戻る、皿で選ぶ）
- 起動時に、裏で曲フォルダの読み直し（変わったファイルだけ）と難易度表の更新をする。難易度表が取れなくても、最後に取れた版を使う
- スコア（ランプ・EX・BP・プレイ回数）は `songs.db` に記録する。自己ベストのゴーストにも使う

譜面を直接指定して遊ぶこともできる：

```
./build/bmjoy 譜面.bme [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas] [--hs 100] [--timing ms]
```

| 操作 | キー |
| --- | --- |
| 鍵盤1〜7 | Z S X D C F V |
| 皿 | 左Shift / 左Ctrl / スペース |
| ハイスピード | ↑ ↓（0.10刻み） |
| 判定タイミング | ← →（1ms刻み。LR2と同じく、ノーツの見た目の位置だけをずらす） |
| フルスクリーン | F4 |
| 終了 | ESC（リザルトで F5 を押すともう一度） |

コントローラは LR2 の既定の割り当て（鍵盤 B4 B7 B3 B8 B2 B5 左、皿 上下）。

### キーコンフィグ

`./build/bmjoy --keyconfig` で設定画面を開く。設定は `~/Library/Application Support/BMjoy/keyconfig.txt` に保存される。

- ↑↓で項目を選び、Enter を押してから割り当てたいキーやボタンを押す（1つの項目に複数割り当てられる）。Backspace で消す。F1 で LR2 の既定に戻す
- ジョイスティックのボタン番号は0から数える（LR2の BUTTON 4 は `joy:button 3`）
- **アナログ皿**：`ANALOG TT` で Enter を押してから皿を回すと、その軸を使う。回している間は押下、止まって `TT RELEASE`（既定100ms）経ったら離す。LR2は皿を方向ボタンとしてしか読まないので、これはBMjoyの拡張
- 画面下の INPUT TEST で、押されているレーンと軸の生の値を確認できる

### スキン

LR2のフォルダ（`LR2files` があるところ）を `--lr2-root` で指定すると、既定で `LR2files/Theme/LR2/Play/play_7.lr2skin` を使う。
別のスキンは `--skin` で .lr2skin を指定する。スキンの中のパス（`LR2files\Theme\...`）は LR2 のフォルダから探す。

```
./build/bmjoy 譜面.bme --lr2-root ~/LR2 --skin ~/LR2/LR2files/Theme/WMIX_HD/play/HDPLAY_W.lr2skin --skin-opt 2=1
```

- 起動時にカスタマイズ項目の一覧（`[番号] 項目 = 選択`）を表示する。`--skin-opt 番号=選択肢の番号` で変える
- `#RESOLUTION` の無いスキンは、画面全体を覆う画像の大きさから SD / HD / FHD を推定する。`--skin-res sd|hd|fhd` で指定もできる
- `--no-skin` で仮の画面になる

### 確認用

- `--info`：譜面の情報を表示する
- `--simulate`：画面も音も出さずに自動演奏で判定を回す
- `python3 tools/make_test_song.py testdata/generated/test_song`：キー音つきのテスト曲を作る

## 参考にしたもの

挙動は OpenLR2（LR2の書き直し）のコードに合わせ、コードは書き直している。対応箇所はコメントに `LR2:` として残している。
