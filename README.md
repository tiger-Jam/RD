# BMjoy

LR2の挙動を既定にした、7K+S専用のBMSプレイヤー。仕様は `次世代BMSプレイヤー 仕様書 v0.1.md`。

いまはロードマップの1段階目（譜面を1つ指定して、仮のプレイ画面で遊ぶ）まで。

## ビルド（macOS）

```
brew install cmake ninja sdl3
cmake -S . -B build -G Ninja
cmake --build build
./build/bmj_tests
```

## 遊ぶ

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

### 確認用

- `--info`：譜面の情報を表示する
- `--simulate`：画面も音も出さずに自動演奏で判定を回す
- `python3 tools/make_test_song.py testdata/generated/test_song`：キー音つきのテスト曲を作る

## 参考にしたもの

挙動は OpenLR2（LR2の書き直し）のコードに合わせ、コードは書き直している。対応箇所はコメントに `LR2:` として残している。
