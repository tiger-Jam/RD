# LR2 スキン ID 対応表と BMjoy の実装状況

LR2 の CSV スキン（`.lr2skin` / `.csv`）とゲーム本体の間の「番号の約束」（タイマー・op・数値・文字列・バーグラフ・スライダー・ボタン、スキン命令）を仕様として一覧にし、BMjoy の実装と突き合わせた監査結果。

- OpenLR2 のコードは写していない。振る舞いは文章と表だけで書いている。
- 選曲画面の細かい挙動（曲リスト、パネル、入力など）は `docs/lr2_select_spec.md` にある。この文書は全シーンの ID 一覧と BMjoy の監査が目的で、選曲の ID の意味は同仕様 §6 と重なるところは短く書いている。
- 調べた日: 2026-10-18。BMjoy は `src/` の現状のコードを読んで判定した（実行しての確認はしていない）。

## 0. 凡例・出典

**出典（優先順）**
1. LR2 付属のスキン資料 `参考に/LR2beta3 2/LR2files/スキン関連ドキュメント/`（timer.txt, dst_option.txt, number.txt, text.txt, bargraph.txt, slider.txt, button.txt, others.txt, customize.txt, skin_history.txt。CP932）。**原典 LR2 が定義する番号はこれを第一とする。**
2. OpenLR2 `参考に/OpenLR2-main/LR2/`（`LR2_skinobject.cpp` の GetOptionFlag_dst / SetObjectValue_Num / _Bargraph / _Slider / _Button、`Scene0*_*.cpp`、`LR2_gameloop.cpp`、`Main.cpp`、`LR2_skinload.cpp`）で意味と起動条件を確認した。OpenLR2 にだけあるものは各章の「拡張」表に分けた。
3. 実スキン `参考に/LR2beta3 2/LR2files/Theme/` の LR2 標準テーマ（`LR2/`）と WMIX_HD（`WMIX_HD/`）。全 CSV を機械的に走査して、どの番号を実際に使っているかを数えた（SRC/DST の timer 欄、DST の op1-3、`#IF`/`#ELSEIF` の条件、`#SRC_NUMBER` の num、`#SRC_TEXT` の st、`#SRC_BARGRAPH` の type、`#SRC_SLIDER` の type、`#SRC_BUTTON` の type）。

**シーン記号**

| 記号 | シーン | procSelecter（OpenLR2） |
|---|---|---|
| S | 選曲 | 2 |
| D | 決定 | 3 |
| P | プレイ（SP 7 鍵の 1P。LR2 標準 `Play/7keys/*.csv`・`Play_half/7.csv`、WMIX `play/csv/AC_1p,AC_2p,wide_1p,wide_2p.csv`） | 4 |
| P他 | プレイの他モード（5/9/10/14 鍵、BATTLE、DP）。使用欄に参考として出すだけで、監査の対象外 | 4 |
| R | リザルト | 5 |
| K | キーコンフィグ | 6 |
| SS | スキンセレクト | 7 |
| CR | コースリザルト | 13 |

（8〜12 は OpenLR2 のイベントモード（Lunaris / Po4）で、原典 LR2 の通常の画面ではない。）

**使用欄**: `L:` は LR2 標準テーマ、`W:` は WMIX_HD。そのテーマで番号を使っているシーンを並べる。`(IF)` は `#IF` 分岐の条件にだけ使われている（読み込み時の評価だけ）。範囲の行は、範囲の中の一部でも使っていれば載せている。

**BMjoy 欄の状態**

| 状態 | 意味 |
|---|---|
| OK | LR2 と同じ値・同じ契機 |
| 固定OK | BMjoy にその機能が無く値は定数だが、その定数が LR2 の既定状態と一致している（例: SP 専用なので BATTLE は常に偽） |
| 誤り | 値・契機が LR2 と違う（備考に理由） |
| 未実装 | 番号を扱っておらず、LR2 と違う値になる（数値なら -1 = 非表示、op なら偽、タイマーなら未起動） |
| 対象外 | DP・BATTLE・IR・コース・トライアル・エフェクタ・リプレイ・ライバルなど、BMjoy に機能自体が無い。入れるときに実装する |
| 画面なし | そのシーン自体が BMjoy に無い |

## 1. シーンの一覧と BMjoy の対応

| シーン | LR2 のスキン種別（`#INFORMATION` の type） | BMjoy | 備考 |
|---|---|---|---|
| 選曲 (S) | 5 | **あり**（`src/app/select_scene.cpp`, `select_context.cpp`） | LR2 スキンで描画。曲バー、パネル、ONMOUSE、readme、マウスカーソルまで実装済み |
| 決定 (D) | 6 | **なし** | `run_play` が読み込み中に決定音だけ鳴らす（`play_session.cpp` の「BMjoyには決定画面が無い」）。LR2 標準・WMIX とも STAGEFILE（gr100）を大きく出す画面 |
| プレイ (P) | 0〜4（7/5/14/10/9 鍵）、12〜14（BATTLE） | **あり（7 鍵 SP の 1P 側だけ）** | ノーツ・判定・ゲージ・BGA（静止画のみ）・小節線・判定ライン |
| リザルト (R) | 7 | **あり**（`run_play` の中） | ゲージグラフ（1P）、スコアグラフ |
| コースリザルト (CR) | 15 | **なし** | コース機能が無い |
| キーコンフィグ (K) | 8 | **なし（独自画面）** | 選曲のボタン13 で独自のキー割り当てウィザード（`run_key_wizard`）を出す。LR2 のキーコンフィグスキンは読まない |
| スキンセレクト (SS) | 9 | **なし** | WMIX は選曲のシステムパネル（パネル2）にスキン選択の部品を置いている（ボタン 170〜190・220〜224、文字列 50/51/100〜114、スライダー 7） |

## 2. ID の一覧と監査

### 2.0 集計

各シーン・種類ごとの、未実装・誤りの **ID の数**（範囲の行は範囲の個数で数える。`#CUSTOMOPTION` の 900〜998 は 1 として数える）。「うちスキン使用」は、そのシーンで LR2 標準か WMIX_HD のどちらかが実際に使っている番号の数。

| シーン | 種類 | 未実装 | うちスキン使用 | 誤り | うちスキン使用 | 対象外 | 画面なし |
|---|---|---|---|---|---|---|---|
| S | タイマー | 19 | 9 | 0 | 0 | 14 | 0 |
| S | オプション(op) | 52 | 40 | 3 | 1 | 84 | 0 |
| S | 数値 | 12 | 10 | 11 | 10 | 71 | 0 |
| S | 文字列 | 26 | 23 | 5 | 4 | 51 | 0 |
| S | バーグラフ | 7 | 0 | 5 | 5 | 0 | 0 |
| S | スライダー | 1 | 1 | 0 | 0 | 18 | 0 |
| S | ボタン | 38 | 38 | 6 | 6 | 51 | 0 |
| D | タイマー | 0 | 0 | 0 | 0 | 0 | 4 |
| D | オプション(op) | 0 | 0 | 0 | 0 | 0 | 21 |
| D | 数値 | 0 | 0 | 0 | 0 | 0 | 5 |
| D | 文字列 | 0 | 0 | 0 | 0 | 0 | 5 |
| P | タイマー | 1 | 0 | 2 | 2 | 53 | 0 |
| P | オプション(op) | 22 | 1 | 1 | 0 | 41 | 0 |
| P | 数値 | 7 | 0 | 8 | 7 | 0 | 0 |
| P | 文字列 | 2 | 1 | 0 | 0 | 0 | 0 |
| P | バーグラフ | 0 | 0 | 0 | 0 | 0 | 0 |
| P | スライダー | 2 | 0 | 0 | 0 | 0 | 0 |
| P | ボタン | 0 | 0 | 1 | 1 | 0 | 0 |
| R | タイマー | 18 | 0 | 1 | 1 | 0 | 0 |
| R | オプション(op) | 11 | 3 | 6 | 4 | 17 | 0 |
| R | 数値 | 8 | 0 | 5 | 5 | 4 | 0 |
| R | 文字列 | 2 | 1 | 0 | 0 | 1 | 0 |
| R | バーグラフ | 0 | 0 | 0 | 0 | 0 | 0 |
| R | ボタン | 0 | 0 | 0 | 0 | 0 | 0 |
| CR | タイマー | 0 | 0 | 0 | 0 | 0 | 1 |
| CR | 文字列 | 0 | 0 | 0 | 0 | 0 | 6 |
| K | オプション(op) | 0 | 0 | 0 | 0 | 0 | 3 |
| K | 文字列 | 0 | 0 | 0 | 0 | 0 | 8 |
| K | ボタン | 0 | 0 | 0 | 0 | 0 | 38 |
| SS | 文字列 | 0 | 0 | 0 | 0 | 0 | 12 |
| SS | スライダー | 0 | 0 | 0 | 0 | 0 | 1 |
| SS | ボタン | 0 | 0 | 0 | 0 | 0 | 26 |

画面ごと無いシーン（D, CR, K, SS）は、表に載っているそのシーン用の番号の数を「画面なし」に入れた。

### 2.1 タイマー

資料の約束: 単位は ms（140 だけ拍）。SRC か DST のタイマーが止まっていれば描かない、と資料にはあるが、OpenLR2 も BMjoy も「SRC のタイマーが止まっていればアニメーションの 0 コマ目」を描く（DST だけで表示を決めている）。原典の挙動は未確認 [?]。鍵盤関連は +0 が皿、+1〜+9 が鍵盤 1〜9、+10〜+19 が 2P 側。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 0 | メインタイマー（シーン開始） | 全 | L:S,D,P,P他,R,CR,K,SS / W:S,D,P,P他,R,CR | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 1 | 入力受付開始（#STARTINPUT 経過） | 全 | L:— / W:— | S: OK<br>P: OK<br>R: 未実装 | R: リザルトでは立てていない（入力判定は startinput の値を直接見ている）。スキンは未使用なので実害なし |
| 2 | フェードアウト（#FADEOUT 後に次のシーン） | 全 | L:S,D,P,P他,R,CR,K,SS / W:S,D,P,P他,R,CR | S: 未実装<br>D: 画面なし<br>P: 誤り<br>R: OK | S: LR2 はキーコンフィグ/スキンセレクトへ移るときだけ起動（曲決定時は無し）。BMjoy はボタン13が独自ウィザード、14が無いので立たない。P: LR2 は曲の終わり・ESC・START+SELECT・全ノーツ判定後の START/SELECT で起動し、#FADEOUT 経過でリザルトへ。BMjoy は曲の終わりだけで、ESC は timer2 を立てずに即リザルト（フェードアウト演出が出ない。START+SELECT 終了も無い） |
| 3 | 閉店（プレイ: ゲージ0で強制終了 / 決定: 鍵盤でスキップ） | P,D | L:D,P,P他 / W:D,P,P他 | D: 画面なし<br>P: OK | P: LR2 は HP<2 で起動（GROOVE/EASY は下限2なので実質 HARD 系のみ）。#CLOSE 経過でリザルト。D: 鍵盤・Enter・左クリックで起動し #SKIP(#CLOSE) 経過でプレイへ |
| 4 | 文字入力終了 | S | L:— / W:— | S: OK |  |
| 10 | 曲リスト移動中 | S | L:— / W:— | S: OK | select 仕様 §6.1 |
| 11 | 曲変更（全シーンで timer1 と同時にも起動） | 全 | L:S / W:S,D,CR | S: OK<br>D: 画面なし<br>P: 未実装<br>R: 未実装<br>CR: 画面なし | OpenLR2 Main.cpp では全シーンで timer1 と同時に起動する。WMIX の decide / courseresult が使う。BMjoy の P/R ではスキン未使用 |
| 12-14 | 曲リスト上/下移動・停止（資料で「未」） | S | L:— / W:— | S: 対象外 | LR2 本体でも未実装 |
| 15 | リードミー開始 | S | L:S / W:S | S: OK |  |
| 16 | リードミー終了 | S | L:S / W:S | S: OK |  |
| 17-18 | コースセレクト開始/終了 | S | L:S / W:S | S: 対象外 | コース機能なし |
| 20,30 | （資料外）パネル0 の開/閉 | S | L:S / W:— | S: 対象外 | LR2 標準 select が参照するが、OpenLR2 でも起動しない（パネル0 を開く操作が無い）[?] |
| 21-28 | パネル1-8 起動 | S | L:S / W:S | S: OK | 資料は 21-29 だが、OpenLR2 もパネル9は開けない |
| 29 | パネル9 起動 | S | L:— / W:— | S: 対象外 | 同上 |
| 31-38 | パネル1-8 終了 | S | L:S / W:S | S: OK |  |
| 39 | パネル9 終了 | S | L:— / W:— | S: 対象外 |  |
| 40 | READY（#LOADSTART+#LOADEND 経過かつ読み込み完了） | P | L:P,P他 / W:P,P他 | P: OK |  |
| 41 | プレイ開始（READY から #PLAYSTART 経過） | P | L:— / W:— | P: OK | OpenLR2 は加えて「START/SELECT 等が押されていない」ことも待つ（押しっぱなしだと始まらない）。BMjoy はこの条件なし [?] |
| 42 | ゲージ上昇 1P | P | L:P,P他 / W:P,P他 | P: OK | 偶数%単位で上がったとき。100% に達したら 42 を止めて 44 |
| 43 | ゲージ上昇 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 44 | ゲージ MAX 1P | P | L:P,P他 / W:P,P他 | P: 誤り | LR2 はプレイ開始時（timer41 と同時）に HARD/DEATH/P-ATTACK/G-ATTACK なら起動（初期値100%のため）。BMjoy は初回の old_hp=-1 で何もしないので、HARD 系で開始直後の MAX 演出が出ない。100% 未満に落ちたら停止、は一致 |
| 45 | ゲージ MAX 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 46 | 判定 1P | P | L:P,P他 / W:P,P他 | P: OK | 空POOR を含む全判定で起動 |
| 47 | 判定 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 48 | フルコンボ 1P | P | L:P,P他 / W:P,P他 | P: OK |  |
| 49 | フルコンボ 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 50-57 | ボム 1P（50=皿, 51-57=鍵盤） | P | L:P,P他 / W:P,P他 | P: OK | GREAT 以上。LN は終端で PG/GR のとき |
| 58-59 | ボム 1P 鍵盤8,9 | P | L:P他 / W:— | P: 対象外 | 9鍵 |
| 60-69 | ボム 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 70-77 | LN ホールド 1P | P | L:P,P他 / W:P,P他 | P: OK |  |
| 78-79 | LN ホールド 1P 鍵盤8,9 | P | L:P他 / W:— | P: 対象外 |  |
| 80-89 | LN ホールド 2P | P | L:P他 / W:P他 | P: 対象外 |  |
| 100-107 | キーオン 1P（100=皿） | S,P,R | L:S,P,P他 / W:S,P,P他 | S: OK<br>P: OK<br>R: 未実装 | LR2 は全シーンで timer1 以降に更新。R はスキン未使用 |
| 108-109 | キーオン 1P 鍵盤8,9 | S,P | L:P,P他 / W:— | P: 対象外 | LR2 標準 7鍵スキンが DST timer に 108/109 を書いているが 7 鍵では起動しない（スキンの記述ミスと思われる） |
| 110-117 | キーオン 2P | S,P | L:S,P他 / W:S,P他 | S: 未実装<br>P: 対象外 | LR2 標準/WMIX の select は 2P 側の 111-117 も使う。BMjoy の入力は 1P の 8 レーンだけ |
| 118-119 | キーオン 2P 鍵盤8,9 | S,P | L:— / W:— | P: 対象外 |  |
| 120-127 | キーオフ 1P | S,P,R | L:P,P他 / W:P,P他 | S: OK<br>P: OK<br>R: 未実装 |  |
| 128-129 | キーオフ 1P 鍵盤8,9 | P | L:P他 / W:— | P: 対象外 |  |
| 130-137 | キーオフ 2P | S,P | L:P他 / W:P他 | S: 未実装<br>P: 対象外 | 110-117 と同じ |
| 138-139 | キーオフ 2P 鍵盤8,9 | P | L:— / W:— | P: 対象外 |  |
| 140 | リズム（1拍=1000、小節でリセット） | P | L:P,P他 / W:P,P他 | P: OK | BMjoy は描画拍位置 − 直前の小節線位置 ×1000。LR2 は経過時間×BPM を積算（停止区間の扱いが違う可能性 [?]） |
| 143 | 最終ノート判定 1P | P | L:— / W:P,P他 | P: OK |  |
| 144 | 最終ノート判定 2P | P | L:— / W:P他 | P: 対象外 |  |
| 150 | リザルト グラフ描画開始 | R | L:— / W:— | R: OK | #SRC_GAUGECHART_1P[0] の op3 時刻 |
| 151 | リザルト グラフ描画終了（ランク表示） | R | L:R / W:R | R: OK | op4 時刻、またはキーでスキップ |
| 152 | リザルト ハイスコア更新表示 | R | L:R,CR / W:CR | R: 誤り | LR2 は記録しないプレイ（オート等 isNosave）では 152 を飛ばして直接 timer2。BMjoy はオートでも 152 を立てる（軽微） |
| 170-171 | DB ロード中/完了 | S | L:— / W:— | S: 未実装 | スキン未使用 |
| 172-174 | IR 接続開始/成功/失敗（資料） | S | L:— / W:— | S: 対象外 | OpenLR2 では 172 は「鍵盤6長押し（リプレイ判定）」で資料と食い違う |
| 180-181 | コースエディター開始/終了 | S | L:— / W:— | S: 対象外 |  |

**OpenLR2 にあって資料に無いタイマー**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 5 | コース作成の完了/取消（以後 1000ms 入力無効） | S | L:— / W:— | 未実装（対象外） |
| 142 | プレイ開始（41 と同時に起動。選曲ではプレビュー用 [?]） | P | L:— / W:— | 未実装（対象外） |
| 160 | BGM の遅延検出用（内部） | P | L:— / W:— | 未実装（対象外） |
| 175-176 | IR ランキング表示の切替（退場/入場） | S | L:— / W:— | 未実装（対象外） |
| 177 | IR 自動接続の待機 | S | L:— / W:— | 未実装（対象外） |
| 411-412 | イベントモード（Lunaris/Po4） | P | L:— / W:— | 未実装（対象外） |

OpenLR2 では 500 を超える番号は常に未起動扱い（WMIX の DP スキンが 600 を書いているが、何も起きない）。

### 2.2 オプション（DST の op1〜op3、`#IF`）

約束: 3 つすべて真なら描く。`!n`（負数）は否定。DST の op は**毎フレーム評価**、`#IF` は**スキン読み込み時だけ**評価（OpenLR2 はシーン開始時に 0〜899 を全部評価して表に入れ、900〜999 は `#CUSTOMOPTION`/`#SETOPTION` の値）。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 0 | 常に true | 全 | L:— / W:— | S: OK<br>P: OK<br>R: OK |  |
| 1 | カーソルがフォルダ | S,D | L:S / W:S | S: OK<br>D: 画面なし |  |
| 2 | カーソルが曲 | S,D | L:S / W:S,D | S: OK<br>D: 画面なし | WMIX decide も使う |
| 3 | コース | S | L:S / W:S | S: OK | BMjoy にコースは無いので常に偽 |
| 4 | 新規コース作成/ランダムコース | S | L:S / W:S | S: OK | 同上 |
| 5 | プレイ可能 | S | L:S / W:S | S: OK |  |
| 10 | DP 系 / D-BATTLE / SP-TO-DP | S,P | L:— / W:S | S: 固定OK<br>P: 固定OK<br>R: 固定OK | BMjoy は SP のみなので偽で正しい |
| 11 | BATTLE | S,P,R | L:S,R(IF),CR(IF) / W:S,P(IF),R(IF),CR | S: 固定OK<br>P: 固定OK<br>R: 固定OK | result.lr2skin / WMIX play の #IF 分岐に使われる |
| 12 | DP 系 or BATTLE 系 | S,P | L:S / W:S | S: 固定OK<br>P: 固定OK |  |
| 13 | BATTLE or ゴーストバトル | P,R | L:R / W:P(IF) | P: 固定OK<br>R: 固定OK |  |
| 20 | パネル無し | S | L:— / W:— | S: OK |  |
| 21-29 | パネル n 起動中 | S | L:S / W:S | S: OK |  |
| 30 | BGA 通常サイズ | P | L:P,P他 / W:P,P他 | S: 未実装<br>P: 固定OK | S は常に偽（スキン未使用） |
| 31 | BGA 拡大 | P | L:P,P他 / W:P,P他 | S: 固定OK<br>P: 固定OK |  |
| 32 | オートプレイ OFF | S,P,R | L:P,P他 / W:P,P他 | S: OK<br>P: OK<br>R: OK |  |
| 33 | オートプレイ ON | S,P,R | L:P,P他 / W:P,P他 | S: OK<br>P: OK<br>R: OK |  |
| 34 | ゴースト OFF | S,P | L:— / W:— | S: 固定OK<br>P: 固定OK |  |
| 35-37 | ゴースト A/B/C | S,P | L:P,P他 / W:P,P他 | S: 固定OK<br>P: 固定OK |  |
| 38 | スコアグラフ OFF | S,P | L:— / W:P(IF),P他(IF) | S: 誤り<br>P: OK | BMjoy の P は 39=真（グラフあり）なのに S は 38=真を返す。どちらかに揃える（設定化するなら文字列71・ボタン70も） |
| 39 | スコアグラフ ON | S,P | L:P,P他 / W:P,P他 | S: 誤り<br>P: OK | 同上 |
| 40 | BGA OFF | S,P | L:— / W:P,P他 | S: 固定OK<br>P: 固定OK |  |
| 41 | BGA ON | S,P | L:— / W:P,P他 | S: 固定OK<br>P: 固定OK |  |
| 42 | 1P ノーマル系ゲージ（GROOVE/EASY かつコース外） | P,R | L:— / W:R | S: 未実装<br>P: OK<br>R: OK | S は常に偽（スキン未使用） |
| 43 | 1P 赤ゲージ（HARD 系 or コース） | P,R | L:— / W:R | S: 未実装<br>P: OK<br>R: OK |  |
| 44 | 2P ノーマル系ゲージ | P,R | L:— / W:R | P: 未実装<br>R: 未実装 | LR2 は 2P ゲージ設定（既定 GROOVE）で真。BMjoy は常に偽。WMIX result が使う |
| 45 | 2P 赤ゲージ | P,R | L:— / W:R | P: 未実装<br>R: 固定OK |  |
| 46 | 難易度フィルタ有効（OpenLR2。資料外） | S | L:S(IF) / W:— | S: 固定OK<br>P: 固定OK | beta2 の履歴では 42/43 だった（後にゲージ用に変更）。資料本文には無い |
| 47 | 難易度フィルタ無効（同上） | S | L:— / W:— | S: 固定OK<br>P: 固定OK |  |
| 50 | オフライン | S,R | L:S / W:S,R | S: OK<br>P: OK<br>R: OK |  |
| 51 | オンライン | S,R | L:S,R,CR / W:S,R,CR | S: OK<br>P: OK<br>R: OK |  |
| 52 | EXTRA MODE OFF | S,D | L:S,D(IF) / W:S | S: 固定OK<br>D: 画面なし<br>P: 未実装 | P/R は常に偽（スキン未使用） |
| 53 | EXTRA MODE ON | S,D | L:S,D(IF) / W:S | S: 固定OK<br>D: 画面なし<br>P: 固定OK |  |
| 54 | AUTO SCRATCH 1P OFF | S,P | L:— / W:— | S: 固定OK<br>P: 固定OK | BMjoy には AUTO SCRATCH が無いので常に真 |
| 55 | AUTO SCRATCH 1P ON | S,P | L:P,P他 / W:— | S: 固定OK<br>P: 固定OK | 常に偽（7k モードは AUTO SCRATCH ではないので ASSIST 表示を出さない） |
| 56 | AUTO SCRATCH 2P OFF | S,P | L:— / W:— | S: 固定OK<br>P: 未実装 |  |
| 57 | AUTO SCRATCH 2P ON | S,P | L:P他 / W:— | S: 固定OK<br>P: 固定OK |  |
| 60 | スコア保存不可 | S | L:S / W:S | S: 固定OK | BMjoy はオート以外常に保存 |
| 61 | スコア保存可 | S | L:S / W:S | S: 固定OK |  |
| 62-66 | この設定で付くランプ（なし/EASY/NORMAL/HARD/EXH相当） | S | L:S / W:— | S: OK |  |
| 70-74 | 同フォルダ難易度別レベル ≤ 上限 | S | L:S / W:— | S: 未実装 | LR2 標準 select がレベルバーの光り分けに使う |
| 75-79 | 同フォルダ難易度別レベル > 上限 | S | L:S / W:— | S: 未実装 | 同上 |
| 80 | ロード未完了（timer40 未起動） | S,P,R | L:S,P,P他,R,K / W:P,P他 | S: 誤り<br>P: OK<br>R: 誤り | LR2 は S/R では timer40 が立たないので常に真。LR2 標準の select/result は op80 で回転する CIRCLE 背景を出す。BMjoy は S で偽、R はプレイのタイマーを見るので偽 → CIRCLE が消える |
| 81 | ロード完了 | S,P,R | L:P,P他 / W:P,P他 | S: 固定OK<br>P: OK<br>R: 誤り | 同上（R で真になってしまう） |
| 82 | リプレイ OFF | S,P,R | L:— / W:— | S: 未実装<br>P: 固定OK<br>R: 固定OK | S は常に偽（スキン未使用） |
| 83-84 | リプレイ録画/再生中 | P | L:— / W:— | P: 固定OK<br>R: 固定OK |  |
| 90 | リザルト クリア | R | L:R / W:R,CR | R: OK | #FLIPRESULT 時は 2P 側を見る（BMjoy はフリップ無し） |
| 91 | リザルト ミス | R | L:R / W:R,CR | R: OK |  |
| 100-105 | 自己ベストのランプ NOPLAY..FC | S,P,R | L:S / W:S,P,P他,R | S: OK<br>P: OK<br>R: OK | R は記録後の値。WMIX play は #IF にも使う |
| 110-117 | 自己ベストのランク AAA..F | S | L:S / W:S | S: OK<br>P: 未実装<br>R: 未実装 | P/R は常に偽（スキン未使用） |
| 118-125 | クリア時のオプション履歴: ゲージ | S | L:S / W:S | S: 未実装 | op_history ビット。LR2 標準/WMIX とも 118-123 を使う。BMjoy の DB に履歴が無い |
| 126-133 | 同: ランダム | S | L:S / W:S | S: 未実装 | 126-131 を使用 |
| 134-141 | 同: エフェクト | S | L:S / W:S | S: 未実装 | 134-137 を使用 |
| 142-145 | 同: その他（AUTO SCRATCH/EXTRA/D-BATTLE/SP TO DP） | S | L:S / W:S | S: 未実装 | 143-145 を使用 |
| 146-149 | 同: 予備 | S | L:— / W:— | S: 未実装 |  |
| 150 | 難易度 未設定 | S,D,P,R | L:— / W:D,P(IF),P他(IF),R | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 151-155 | 難易度 BEGINNER..INSANE | S,D,P,R | L:S,D,P,P他 / W:S,D,P,P他,R | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 160 | 7KEYS | S,P,R | L:S / W:S,R(IF) | S: OK<br>P: 固定OK<br>R: 固定OK | S は「曲のとき真」 |
| 161-164 | 5/14/10/9 KEYS | S,P,R | L:S,P,P他 / W:S,P,P他,R | S: 固定OK<br>P: 固定OK<br>R: 固定OK |  |
| 170 | BGA 無し | S | L:— / W:— | S: 未実装<br>P: 未実装 | S は常に真（BGA の有無を見ていない） |
| 171 | BGA 有り | S | L:— / W:— | S: 未実装<br>P: 未実装 |  |
| 172 | LN 無し | S | L:— / W:— | S: OK<br>P: 未実装 |  |
| 173 | LN 有り | S | L:— / W:S | S: OK<br>P: 未実装 |  |
| 174 | 付属テキスト無し | S | L:S / W:— | S: 未実装 | 常に真 |
| 175 | 付属テキスト有り | S | L:S / W:S | S: 未実装 | LR2 標準/WMIX とも readme アイコンに使う。BMjoy はボタン17で txt を開けるのに 175 は常に偽 |
| 176 | BPM 変化無し | S,P,R | L:— / W:S,R | S: OK<br>P: OK<br>R: OK |  |
| 177 | BPM 変化有り | S,P,R | L:S / W:S,P,P他,R | S: OK<br>P: OK<br>R: OK |  |
| 178 | #RANDOM 無し | S | L:— / W:— | S: 未実装 | 常に真 |
| 179 | #RANDOM 有り | S | L:— / W:— | S: 未実装 |  |
| 180-183 | 判定ランク VERYHARD/HARD/NORMAL/EASY | S,P,R | L:S / W:S,R(IF) | S: OK<br>P: OK<br>R: OK |  |
| 185 | レベルが上限内 | S | L:— / W:— | S: OK<br>P: 未実装 | LR2 は 7 鍵で固定 12。BMjoy は設定 levelbar_7（既定 12） |
| 186 | レベルが上限超 | S | L:— / W:— | S: OK<br>P: 未実装 |  |
| 190 | STAGEFILE 無し | S,D | L:D(IF) / W:— | S: OK<br>D: 画面なし<br>P: 未実装 |  |
| 191 | STAGEFILE 有り | S,D | L:D(IF) / W:D | S: OK<br>D: 画面なし<br>P: 未実装 | LR2 標準/WMIX decide が使う |
| 192 | BANNER 無し | S | L:— / W:— | S: OK |  |
| 193 | BANNER 有り | S | L:S / W:S | S: OK |  |
| 194 | BACKBMP 無し | S,P | L:— / W:P,P他 | S: OK<br>P: OK |  |
| 195 | BACKBMP 有り | S,P | L:— / W:P,P他 | S: OK<br>P: OK | ただし P で gr101（BACKBMP 画像）を読んでいないので、WMIX は 195 真でも絵が出ない（§3） |
| 196 | リプレイ無し | S | L:S / W:— | S: 固定OK |  |
| 197 | リプレイ有り | S | L:S / W:S | S: 固定OK |  |
| 200-207 | 1P 現在ランク（判定済みノーツ基準）AAA..F | P | L:P,P他 / W:— | P: OK |  |
| 210-217 | 2P 現在ランク | P | L:— / W:— | P: 対象外 | OpenLR2 の 210 は条件が逆（> 8 で偽）[?] |
| 220-227 | ランク確定（総ノーツ基準、220=AAA, 221=AA以上…累積） | P | L:P,P他 / W:P,P他 | P: OK | 221-226 は「そのランク以上」の累積判定 |
| 230-240 | 1P ゲージ 0-10%..100% | P | L:— / W:— | P: OK | LR2 は HP の実値で判定（表示用の補間値ではない） |
| 241-246 | 1P 判定 PG/GR/GD/BD/POOR/空POOR | P | L:— / W:P,P他 | P: OK |  |
| 247 | 1P POOR BGA 表示時間外（ミスから 1000ms 以上） | P | L:P / W:— | P: OK |  |
| 248 | 1P POOR BGA 表示時間中 | P | L:P / W:— | P: OK |  |
| 250-260 | 2P ゲージ | P | L:— / W:— | P: 対象外 | 資料の「250 2P 100%」は 260 の誤記 |
| 261-266 | 2P 判定 | P | L:— / W:P他 | P: 対象外 |  |
| 267-268 | 2P POOR BGA | P | L:— / W:— | P: 対象外 |  |
| 270 | 1P SUD+ 変更中（START/SELECT 押下中） | P | L:P,P他 / W:P,P他 | P: OK |  |
| 271 | 2P SUD+ 変更中 | P | L:P他 / W:P他 | P: 固定OK |  |
| 280-283 | コースステージ 1-4 | P | L:— / W:P,P他 | P: 対象外 |  |
| 284-288 | コースステージ 5-9（資料で予約） | P | L:— / W:— | P: 対象外 |  |
| 289 | コースステージ FINAL | P | L:— / W:P,P他 | P: 対象外 |  |
| 290-293 | コース / ノンストップ / 段位 / エキスパート | S,D,P,R | L:— / W:D(IF),P(IF),P他(IF),R | S: 対象外<br>D: 画面なし<br>P: 対象外<br>R: 対象外 | BMjoy は常に偽（コース無しなので値は正しい） |
| 300-307 | リザルト 1P ランク AAA..F（総ノーツ基準） | R | L:R,CR / W:R,CR | R: OK | 300 は 220 と同じ |
| 308 | リザルト 1P 0（EX スコア 0） | R | L:R,CR / W:— | R: 未実装 | LR2 標準 result が使う。BMjoy は常に偽 |
| 310-317 | リザルト 2P ランク | R | L:R,CR / W:R,CR | R: 対象外 | LR2 標準 result が使う（ターゲット無しなら 2P は 0 点） |
| 318 | リザルト 2P 0 | R | L:R,CR / W:— | R: 未実装 | ターゲット無しの LR2 では真になる。BMjoy は常に偽 |
| 320-327 | 更新前ランク AAA..F | R | L:R,CR / W:R,CR | R: OK |  |
| 330 | スコア更新 | R | L:— / W:R(IF) | R: OK |  |
| 331 | MAXCOMBO 更新 | R | L:— / W:— | R: OK |  |
| 332 | 最小 BP 更新 | R | L:— / W:R(IF) | R: OK | BMjoy は旧記録が無いとき偽。LR2 は旧 minbp との比較のみ（未記録時の値は未確認 [?]） |
| 333 | トライアル更新 | R | L:R / W:— | R: 対象外 |  |
| 334 | IR 順位更新 | R | L:— / W:— | R: 対象外 |  |
| 335 | ランク更新（beta2 履歴） | R | L:— / W:R(IF) | R: OK |  |
| 340-347 | 更新後ランク AAA..F | R | L:R,CR / W:R,CR | R: OK | OpenLR2 では「今回のランク」そのもの（max(旧,新) ではない）。BMjoy も同じ |
| 350 | リザルトフリップ無効 | R | L:R(IF) / W:— | R: 誤り | LR2 はプレイスキンに #FLIPRESULT が無ければ真（シーン開始時に評価）。BMjoy は #FLIPRESULT/#DISABLEFLIP を読まず常に偽。LR2 標準 result.lr2skin の #IF 分岐に使われる |
| 351 | リザルトフリップ有効 | R | L:R(IF) / W:— | R: 誤り | LR2 標準 7 鍵で PLAYSIDE=RIGHT（7_R*.csv は #FLIPRESULT を書いている）のとき LR2 は真。BMjoy は常に偽 |
| 352-354 | 1P WIN / LOSE / DRAW | R | L:R / W:— | R: 対象外 |  |
| 400-402 | キーコンフィグ 7/9/5 鍵 | K | L:K / W:— | K: 画面なし |  |
| 500-504 | 同フォルダに難易度 d の譜面が無い | S | L:S / W:— | S: OK |  |
| 505-509 | 同フォルダに難易度 d の譜面が有る | S,D | L:S / W:S,D | S: OK<br>D: 画面なし |  |
| 510-514 | 同フォルダに難易度 d が 1 つ | S | L:S / W:— | S: OK |  |
| 515-519 | 同フォルダに難易度 d が複数 | S | L:S / W:— | S: OK |  |
| 520-565 | レベルバー 難易度 d のランプ v（520+d*10+v） | S | L:S / W:— | S: OK | 資料はコピペで全部 beginner と書いてある |
| 570-572 | コースセレクト中 / でない | S | L:— / W:S | S: 固定OK | OpenLR2 の 570-572 は break 抜けあり（select 仕様 §8-10） |
| 580-589 | コース stage 数 n 以上 | S | L:S / W:S,CR | S: 対象外 |  |
| 590-599 | コースセレクト stage n 選択中 | S | L:S / W:— | S: 対象外 |  |
| 600-608 | IR 状態 | S | L:S / W:— | S: 固定OK | 600=真のみ |
| 620 | ランキング表示中ではない | S | L:S / W:— | S: 固定OK |  |
| 621 | ランキング表示中 | S | L:S / W:— | S: 固定OK |  |
| 622 | ゴーストバトルではない | S,P,R | L:— / W:— | S: 固定OK<br>P: 誤り<br>R: 誤り | PlayContext は 622 も偽を返す（真が正しい）。スキン未使用 |
| 623 | ゴーストバトル中 | P,R | L:P他,R(IF),CR(IF) / W:P(IF),R | S: 固定OK<br>P: 固定OK<br>R: 固定OK |  |
| 624 | スコア比較状況ではない | S | L:S / W:— | S: 固定OK |  |
| 625 | スコア比較状況である（ライバル） | S | L:S / W:S | S: 固定OK |  |
| 640-645 | ライバルのランプ | S | L:S / W:— | S: 対象外 |  |
| 650-657 | ライバルのランク | S | L:S / W:S | S: 対象外 |  |
| 700-745 | コース stage s の難易度 v | S | L:S / W:S,CR(IF) | S: 対象外 |  |
| 900-998 | #CUSTOMOPTION / #SETOPTION | 全 | L:S,D(IF),P,P他,R(IF),CR(IF) / W:S(IF),D(IF),P,P他,R,CR(IF) | S: OK<br>P: OK<br>R: 誤り | R: 描画時の判定がプレイスキンの op 表を見ている（PlayContext::skin がプレイスキンのまま）。#IF（読み込み時）は正しい。WMIX result は DST op に 928 を使う |
| 999 | 常に false | 全 | L:S(IF) / W:— | S: OK<br>P: OK<br>R: OK |  |

**OpenLR2 にあって資料に無い op**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 270（リフト部分） | OpenLR2 リフト調整中も真 | P | L:— / W:— | 未実装 |
| 274-275 | 1P/2P レーンカバー有効 | P | L:— / W:— | 未実装 |
| 610-616 | IR ランキング人数 0 / >0 … >5（資料外だが LR2 標準 select が使うので原典にもあると推定。BMjoy では対象外） | S | L:S / W:— | 未実装 |
| 626 | 取り込みスコア表示中 | S | L:— / W:— | 未実装 |
| 800-809 | イベントモード カーソル | — | L:— / W:— | 未実装 |
| 810-829 | イベントモード 画面 | — | L:— / W:— | 未実装 |
| 830 | BMS リソース読み込み完了 | P | L:— / W:— | 未実装 |

### 2.3 数値（`#SRC_NUMBER` の num）

約束: 定義の無い番号は -1（24 コマ以外の数字画像では描かない）。定義があって値が出ない（0 除算など）ときは 0。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 10 | ハイスピード 1P | S,P | L:S,P,P他 / W:S,P,P他 | S: OK<br>P: OK |  |
| 11 | ハイスピード 2P | S,P | L:S,P他 / W:S,P他 | S: 固定OK<br>P: 固定OK | BMjoy は 1P と同じ値 |
| 12 | 判定タイミング | S,P | L:S / W:S,P,P他 | S: OK<br>P: OK |  |
| 13 | デフォルトターゲット % | S | L:S / W:S | S: 固定OK<br>P: 固定OK | 90 固定（ボタン76 が未実装） |
| 14 | SUD+ 1P | S,P | L:P,P他 / W:P,P他 | S: OK<br>P: OK |  |
| 15 | SUD+ 2P | S,P | L:— / W:P他 | S: 誤り<br>P: 誤り | 1P の値を返す。SP スキンは未使用 |
| 20 | FPS | 全 | L:— / W:— | S: 未実装<br>P: OK<br>R: OK |  |
| 21-26 | 年 月 日 時 分 秒 | 全 | L:— / W:S | S: OK<br>P: 未実装<br>R: 未実装 | WMIX select のみ使用 |
| 30-37 | 総プレイ数・クリア・失敗・累計判定数 | S | L:S / W:S | S: 未実装 | LR2 標準/WMIX ともパネル6（プレイヤーステータス）で使う |
| 38-39 | ランニングコンボ / 最大 | S | L:S / W:S | S: 未実装 | 同上 |
| 40 | トライアルレベル | S | L:S / W:S | S: 対象外 | トライアル機能なし |
| 41 | トライアルレベル−1 | S | L:— / W:— | S: 対象外 |  |
| 42 | カーソル曲のレベル（beta2 履歴） | S | L:— / W:— | S: OK<br>P: 未実装 |  |
| 45-49 | 同フォルダ難易度別のレベル | S,D,P,R | L:S / W:S,D,P,P他,R | S: OK<br>D: 画面なし<br>P: 誤り<br>R: 誤り | P/R は今の譜面の難易度の欄だけ値を出し、他は 0（LR2 は同フォルダの譜面のレベル、無ければ -1 で非表示）。WMIX の decide/play/result が使う |
| 50-56 | EQ 0-6 | S | L:S / W:S | S: 対象外 | エフェクタ無し。両スキンともパネル5 で使う |
| 57-59 | 音量 MASTER/KEY/BGM | S | L:S / W:S | S: 対象外 | 同上 |
| 60-65 | FX0-2 パラメータ 1P/2P | S | L:S / W:S | S: 対象外 |  |
| 66 | ピッチ | S | L:S / W:S | S: 対象外 |  |
| 70 | ベストスコア（200000 満点） | S | L:— / W:— | S: 未実装 | スキン未使用 |
| 71-79 | ベスト EX・最大EX・率・ノーツ・コンボ・BP・回数 | S | L:S / W:S | S: OK |  |
| 80-84 | ベストの PG/GR/GD/BD/PR 数 | S | L:S / W:S | S: 誤り | 常に 0（DB に判定内訳が無い）。両スキンが使う |
| 85-89 | 同 割合 % | S | L:S / W:S | S: 誤り | 同上 |
| 90 | 最大 BPM | S,P,R | L:S / W:S,P,P他,R | S: OK<br>P: OK<br>R: OK |  |
| 91 | 最小 BPM | S,P,R | L:S / W:S,P,P他,R | S: OK<br>P: OK<br>R: OK |  |
| 92-94 | IR 順位 / 人数 / クリア率 | S | L:S / W:S | S: 対象外 |  |
| 95 | ライバルとの EX 差 | S | L:S / W:S | S: 対象外 |  |
| 96 | カーソル曲のレベル（beta3 履歴） | S | L:— / W:— | S: OK |  |
| 100 | スコア（200000 満点） | P,R | L:P,P他,R / W:— | P: 誤り<br>R: OK | LR2 は表示用に 500ms かけて数え上げる値（score_print）。BMjoy は即値 |
| 101 | EX スコア | P,R | L:P,P他,R,CR / W:P,P他,R,CR | P: OK<br>R: OK |  |
| 102 | 率（判定済みノーツ基準） | P,R | L:P,P他,R / W:P,P他,R | P: OK<br>R: OK |  |
| 103 | 率 小数2桁 | P,R | L:— / W:R | P: OK<br>R: OK |  |
| 104 | 現在コンボ | P,R | L:— / W:— | P: OK<br>R: OK |  |
| 105 | 最大コンボ | P,R | L:P,P他,R / W:P,P他,R,CR | P: OK<br>R: OK |  |
| 106 | 総ノーツ | P,R | L:R / W:R | P: OK<br>R: OK |  |
| 107 | ゲージ %（偶数） | P,R | L:P,P他 / W:P,P他,R | P: 誤り<br>R: OK | LR2 はゲージの見た目と同じく補間値（変化 1% あたり 10ms）。BMjoy は数値もゲージ画像も即値 |
| 108 | 2P との EX 差 | P,R | L:P,P他 / W:P,P他,R | P: OK<br>R: OK |  |
| 109 | 次のランクまでの差（資料外、OpenLR2 にあり） | P,R | L:— / W:— | P: OK<br>R: OK |  |
| 110-114 | PG/GR/GD/BD/POOR（POOR は空POOR 込み） | P,R | L:P,P他,R,CR / W:P,P他,R,CR | P: OK<br>R: OK |  |
| 115-116 | 総ノーツ基準の率 / 小数2桁 | P,R | L:— / W:CR | P: OK<br>R: OK |  |
| 120-136 | 2P（ターゲット/対戦相手）の同等値 | P,R | L:P他,R,CR / W:P,P他,R,CR | P: 固定OK<br>R: 固定OK | ターゲット無し（LR2 の既定）なら 0 で正しい。ターゲット機能を入れるなら要実装 |
| 150-153 | ハイスコア/ターゲット現在値・差 | P,R | L:P,P他,R / W:P,P他 | P: OK<br>R: OK |  |
| 154 | 次のランクとの差 | P,R | L:R / W:R | P: OK<br>R: OK |  |
| 155-158 | ハイスコア/ターゲット率 と小数2桁 | P,R | L:R / W:— | P: OK<br>R: OK |  |
| 160 | BPM | P | L:P,P他 / W:P,P他 | P: OK |  |
| 161-162 | 経過 分/秒 | P | L:P,P他 / W:— | P: OK |  |
| 163-164 | 残り 分/秒 | P,R | L:— / W:P,P他,R | P: OK<br>R: OK |  |
| 165 | ロード % | P | L:— / W:— | P: OK |  |
| 170-172 | EX 更新前 / 今回 / 差 | R | L:R,CR / W:R,CR | R: OK |  |
| 173-175 | MAXCOMBO 更新前 / 今回 / 差 | R | L:R,CR / W:CR | R: OK |  |
| 176-178 | 最小BP 更新前 / 今回 / 差 | R | L:R,CR / W:R,CR | R: OK |  |
| 179-182 | IR 順位 / 人数 / クリア率 / 更新前順位 | R | L:R,CR / W:R,CR | R: 対象外 | LR2 標準/WMIX result が使う（-1 で非表示） |
| 183-184 | 率 更新前 / 小数2桁 | R | L:— / W:— | R: 未実装 | スキン未使用 |
| 200-201 | IR 総人数 / 総プレイ回数 | S | L:S / W:— | S: 対象外 |  |
| 210-219 | IR クリア別人数・割合 | S | L:S / W:P,P他,R | S: 対象外 | P/R での 201,210-218 は拡張（下記） |
| 220 | IR 自動更新までの残り | S | L:S / W:— | S: 対象外 |  |
| 250-259 | コース stage のレベル | S | L:S / W:S,CR | S: 対象外 | 資料は 250-254 |
| 270-294 | ライバルの 70-94 相当 | S | L:S / W:S | S: 対象外 |  |

**資料外・拡張の数値**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 201,210-218（P/R） | FAST/SLOW 拡張（ずれ ms・FAST/SLOW・FAST 数・SLOW 数・CB・残り/判定済みノーツ）。「LR2 with Fast/Slow」由来。WMIX が play で 210、result で 212/214 を使う | P,R | L:— / W:— | BMjoy 実装済み（201,210,212,214,216-218）。211,213 は 2P |
| 295, 418 | LR2OOL のランダム配置表示 | P | L:— / W:— | 未実装 |
| 421, 422 | OpenLR2 リフト量 | P | L:— / W:— | 実装（LIFT の量 %） |
| 312 | 見えている時間（ms。beatoraja と同じ番号） | P | L:— / W:— | BMjoy の拡張 |
| 313 | 緑数字（1/600 秒。beatoraja と同じ番号） | P | L:— / W:— | BMjoy の拡張 |
| 314 | LIFT の量（%。beatoraja と同じ番号） | P | L:— / W:— | BMjoy の拡張 |
| 300-382 の一部 | LR2 標準 select が参照するが資料・OpenLR2 とも未定義（IR ランキング表用と推定 [?]） | S | L:— / W:— | OpenLR2 では -1（非表示）。BMjoy も -1 |

### 2.4 文字列（`#SRC_TEXT` の st）

LR2 の文字列表はシーンをまたいで残る（選曲で入れた値がリザルトでもそのまま見える）。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 1 | ターゲット名 / ライバル名 / ランキングのカーソル行 | S,P | L:S,P,P他 / W:S | S: 固定OK<br>P: 未実装<br>R: 未実装 | S は "NO TARGET"。LR2 標準 7 鍵プレイスキンが使う（LR2 はプレイ開始時にターゲット名を入れる） |
| 2 | プレイヤー名 | S,P | L:S,P他 / W:— | S: 未実装<br>P: 未実装 | LR2 標準 select が使う |
| 10 | タイトル | 全 | L:S,D,P,P他 / W:S,D,P,P他,R | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 11 | サブタイトル | 全 | L:S,D,P,P他 / W:S,D,P,P他 | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 12 | タイトル+サブタイトル | 全 | L:R / W:P,P他,R | S: OK<br>P: OK<br>R: OK |  |
| 13 | ジャンル | 全 | L:S,D,P,P他 / W:S,D,P,P他 | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 14 | アーティスト | 全 | L:S,D,P,P他 / W:S,D,P,P他,R | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 15 | サブアーティスト | 全 | L:S,D,P,P他 / W:P,P他 | S: OK<br>D: 画面なし<br>P: OK<br>R: OK |  |
| 16 | 検索タグ | S | L:— / W:— | S: 未実装 |  |
| 17 | プレイレベル（文字列） | S | L:— / W:— | S: OK |  |
| 18 | 難度カテゴリ（文字列） | S | L:— / W:— | S: OK |  |
| 19 | exlevel（OpenLR2） | S | L:— / W:— | S: 未実装 |  |
| 20 | 編集用タイトル / リザルトの IR 送信メッセージ（beta2 履歴） | S,R | L:S,R,CR / W:S,CR | S: 未実装<br>R: 対象外 | S はパネル4（タグエディター）。R は IR |
| 21-28 | 編集用 サブタイトル..難度 | S | L:S / W:S | S: 未実装 | タグエディター（両スキン） |
| 29 | 編集用 exlevel（OpenLR2） | S,R | L:S / W:S,R | S: 未実装<br>R: 未実装 | WMIX result も使う（LR2 の文字列表はシーンをまたいで残る） |
| 30 | 検索語 / フォルダ名 | S | L:S / W:S | S: OK |  |
| 40-47 | キーコンフィグ スロット0-7 | K | L:K / W:— | K: 画面なし |  |
| 50 | スキン名 | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし | WMIX はシステムパネル内にスキン選択がある |
| 51 | スキン作者 | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし |  |
| 60 | プレイモード（鍵盤数フィルタ） | S | L:S / W:— | S: 誤り | LR2 は optionstr.csv の #MODE の名前。BMjoy は独自（beatoraja）並びの名前 |
| 61 | ソート | S | L:— / W:— | S: 誤り | 同上（#SORT）。OpenLR2 は鍵盤数フィルタ値で引いていてこれもバグ（select 仕様 §8-3） |
| 62 | 難易度フィルタ | S | L:— / W:— | S: OK |  |
| 63 | ランダム 1P | S | L:S / W:— | S: OK |  |
| 64 | ランダム 2P | S | L:S / W:— | S: 固定OK |  |
| 65 | ゲージ 1P | S | L:S / W:— | S: OK |  |
| 66 | ゲージ 2P | S | L:S / W:— | S: 誤り | 1P の値を出す |
| 67 | アシスト 1P | S | L:S / W:— | S: OK | BMjoy は 7k モード（7K+S / 7K） |
| 68 | アシスト 2P | S | L:S / W:— | S: 固定OK |  |
| 69 | バトル | S | L:S / W:— | S: 固定OK |  |
| 70 | フリップ | S | L:S / W:— | S: 固定OK |  |
| 71 | スコアグラフ | S | L:S / W:S | S: 誤り | "OFF" 固定だがプレイでは op39（グラフあり） |
| 72 | ゴースト | S | L:S / W:S | S: 固定OK |  |
| 73 | シャッター（レーンカバー） | S | L:S / W:S | S: OK |  |
| 74 | HS-FIX | S | L:S / W:— | S: OK | 5=MAINBPM は OpenLR2 拡張 |
| 75 | BGA サイズ | S | L:S / W:S | S: 固定OK |  |
| 76 | BGA 描画 | S | L:S / W:S | S: 固定OK |  |
| 77 | 画面色数 | S | L:— / W:— | S: 未実装 | スキン未使用 |
| 78 | VSYNC | S | L:— / W:— | S: OK |  |
| 79 | スクリーンモード | S | L:S / W:S | S: OK |  |
| 80 | ジャッジ自動調整 | S | L:S / W:S | S: 固定OK |  |
| 81 | リプレイ保存方式 | S | L:S / W:S | S: 固定OK |  |
| 82-83 | トライアル 1/2 行目 | S | L:S / W:S | S: 対象外 | 両スキンが使う |
| 84 | エフェクト 1P | S | L:S / W:— | S: OK |  |
| 85 | エフェクト 2P | S | L:S / W:— | S: 誤り | 1P の値を出す |
| 100-104 | スキンカスタマイズ カテゴリ名（5 行分） | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし | 資料は 100-109。WMIX はシステムパネル内で使う |
| 110-114 | スキンカスタマイズ 項目名 | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし |  |
| 120-129 | ランキング プレイヤー名 | S | L:S / W:— | S: 対象外 | LR2 標準 select が 120-124 を使う。OpenLR2 も未実装 |
| 130-134 | クリア文字列（資料は 130-131 仮） | S | L:S / W:— | S: 対象外 | 同上 |
| 150-154 | コース stage ごとの曲タイトル | S,CR | L:S / W:S,CR | S: 対象外<br>CR: 画面なし |  |
| 160-164 | コース stage ごとのサブタイトル | S | L:S / W:— | S: 対象外 |  |
| 170 | コース名 | S,CR | L:S / W:S,CR | S: 対象外<br>CR: 画面なし |  |
| 171-183 | コースオプション（つなぎ・ソフラン・ゲージ・オプション禁止・IR） | S | L:S / W:S | S: 対象外 |  |
| 190-199 | ランダムコースオプション | S | L:S / W:S | S: 対象外 |  |

**拡張**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 300-302 | フルスクリーンフィルタ / ストレッチ / リフト | S | L:— / W:— | 未実装 |

### 2.5 バーグラフ（`#SRC_BARGRAPH` の op1=type, op2=向き 0 横 / 1 縦）

約束: 値 ≤ 0 なら描かない。値 ≥ 最大なら最大。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 1 | 曲の進行 | P | L:— / W:— | P: OK |  |
| 2 | 曲のロード | P | L:P,P他 / W:— | P: OK |  |
| 3 | カーソル曲のレベル（7鍵は 12 が最大） | S | L:— / W:— | S: OK | LR2 は固定 12。BMjoy は設定 levelbar_7 |
| 5-9 | 同フォルダ難易度別レベル | S | L:S / W:— | S: 誤り | LR2 はカーソル変更（timer11）から 100ms かけて前の値から伸び縮みする。BMjoy は即値（軽微） |
| 10 | 1P EX スコア | P | L:P,P他 / W:P,P他 | P: OK |  |
| 11 | 1P EX 予測値 | P | L:— / W:— | P: OK |  |
| 12 | ハイスコア現在値 | P | L:P,P他 / W:P,P他 | P: OK |  |
| 13 | ハイスコア最終値 | P | L:P,P他 / W:P,P他 | P: OK |  |
| 14 | ターゲット現在値（BATTLE 時は 2P） | P | L:P,P他 / W:P,P他 | P: OK |  |
| 15 | ターゲット最終値 | P | L:P,P他 / W:P,P他 | P: OK |  |
| 20-25 | 1P PG/GR/GD/BD/PR/MAXCOMBO ÷ ノーツ | R | L:— / W:— | R: OK |  |
| 26 | 1P スコア ÷ 200000 | R | L:— / W:— | R: OK | OpenLR2 は最大値 20000（7/14鍵）で、ほぼ常に満タンになる [?]。資料どおり 200000 の BMjoy を正とする |
| 27 | 1P EX ÷ (ノーツ×2) | R | L:— / W:— | R: OK |  |
| 30-37 | 2P（ターゲット）同等 | R | L:— / W:— | R: 固定OK | ターゲット無しなら値 0 → 描かない、で一致 |
| 40-46 | ベストの判定内訳・スコア | S | L:— / W:— | S: 未実装 | スキン未使用 |
| 47 | ベストの EX ÷ 最大 | S | L:— / W:S | S: OK |  |

**拡張**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 48-49, 58-59 | 1P/2P SLOW・FAST の割合（LR2 with Fast/Slow 由来） | P,R | L:— / W:— | 未実装（両スキン未使用） |

### 2.6 スライダー（`#SRC_SLIDER` の op1=向き, op2=可動幅, op3=type, op4=操作無効）

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 1 | 曲リスト位置 | S | L:S / W:S | S: OK |  |
| 2 | ハイスピード 1P | S,P | L:— / W:— | S: OK<br>P: OK | 表示のみ |
| 3 | ハイスピード 2P | S,P | L:— / W:— | S: 固定OK<br>P: 未実装 |  |
| 4 | SUD+ 1P（プレイ中は巨大スライダー） | S,P | L:P,P他 / W:P,P他 | S: OK<br>P: OK | LR2 はプレイ中もマウスでドラッグして SUD+ を変えられる。BMjoy は表示だけ（ホイール・キーでは変えられる） |
| 5 | SUD+ 2P | S,P | L:P他 / W:P他 | S: 固定OK<br>P: 未実装 |  |
| 6 | 曲の進行度 | P | L:P,P他 / W:P,P他 | P: OK |  |
| 7 | スキンカスタマイズのスクロール | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし | WMIX のシステムパネル |
| 8 | IR ランキングのスクロール | S | L:S / W:— | S: 対象外 |  |
| 10-16 | EQ 0-6 | S | L:S / W:S | S: 対象外 | 両スキンが使う（エフェクタ無し） |
| 17-19 | 音量 | S | L:S / W:S | S: 対象外 |  |
| 20-25 | FX パラメータ | S | L:S / W:S | S: 対象外 |  |
| 26 | ピッチ | S | L:S / W:S | S: 対象外 |  |

**拡張**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 27-28 | OpenLR2 リフト | P | L:— / W:— | 未実装 |

### 2.7 ボタン（`#SRC_BUTTON` の op1=type, op2=click, op3=panel, op4=plusonly）

表示コマ = 現在値。BMjoy でクリックが効くのは選曲画面だけ（LR2 も実質選曲・キーコンフィグ・スキンセレクトのみ）。

| ID | 意味 | 使うシーン | 使用（L=LR2標準 / W=WMIX_HD） | BMjoy | 備考 |
|---|---|---|---|---|---|
| 1-8 | パネル 1-8 | S | L:S / W:S | S: OK |  |
| 9 | パネル 9 | S | L:— / W:— | S: 対象外 | OpenLR2 も 1-8 のみ |
| 10 | 難易度フィルタ | S | L:S / W:S | S: OK |  |
| 11 | 鍵盤数フィルタ | S | L:S / W:S | S: 誤り | 値の並びが BMjoy 独自（表示コマだけ LR2 の並びに読み替え）。クリックの順序・範囲が LR2 と違う |
| 12 | ソート | S | L:S / W:S | S: 誤り | 同上。LR2 は 0..4（OFF/TITLE/LEVEL/FOLDER/CLEAR）でループ |
| 13 | キーコンフィグへ | S | L:S / W:S | S: 誤り | LR2 は timer2 → #FADEOUT 後にキーコンフィグ画面（スキン）。BMjoy は独自ウィザード |
| 14 | スキンセレクトへ | S | L:S / W:S | S: 未実装 | 両スキンが使う |
| 15 | プレイ開始 | S | L:— / W:— | S: OK |  |
| 16 | オートプレイ開始 | S | L:S / W:S | S: OK |  |
| 17 | テキストビュー | S | L:S / W:S | S: OK |  |
| 18 | タグのリセット | S | L:S / W:S | S: 未実装 | タグエディター無し |
| 19 | リプレイ開始 | S | L:S / W:S | S: 対象外 |  |
| 20-22 | FX0-2 種類 | S,P | L:S / W:S,P,P他 | S: 対象外<br>P: 固定OK | WMIX play も 20 を表示に使う（0=OFF で正しい） |
| 23-28 | FX ON/OFF・対象 | S | L:S / W:S | S: 対象外 |  |
| 29-30 | EQ / EQ プリセット | S | L:S / W:S | S: 対象外 |  |
| 31 | 音量調整 ON/OFF | S | L:— / W:— | S: 対象外 |  |
| 32-33 | ピッチ ON/OFF / 種類 | S | L:S / W:S | S: 対象外 |  |
| 40 | ゲージ 1P | S,P,R | L:S / W:S,R | S: OK<br>P: OK<br>R: OK |  |
| 41 | ゲージ 2P | S,R | L:S / W:S,R | S: 誤り<br>R: 固定OK | S は 1P の値を表示 |
| 42 | ランダム 1P | S | L:S / W:S | S: OK |  |
| 43 | ランダム 2P | S | L:S / W:S | S: 未実装 | 両スキンが使う |
| 44 | アシスト 1P | S | L:S / W:S | S: OK | BMjoy は 7k モード（0 = 7K+S 1 = 7K）。W-MIX のコマは tools/skin/wmix_gauge_frames.py |
| 45 | アシスト 2P | S | L:S / W:S | S: 未実装 |  |
| 46 | シャッター（レーンカバー） | S | L:S / W:S | S: OK |  |
| 50 | HID/SUD 1P | S | L:S / W:S | S: OK |  |
| 51 | HID/SUD 2P | S | L:S / W:S | S: 誤り | 1P の値を表示 |
| 54 | DP FLIP | S | L:S / W:S | S: 対象外 |  |
| 55 | HS-FIX | S,P | L:S / W:S,P,P他 | S: 誤り<br>P: 誤り | S: LR2 は 0..4（CONSTANT 含む）。BMjoy は CONSTANT を飛ばし 5=MAINBPM（OpenLR2 拡張）を入れている。P: PlayContext が常に 0 を返す（WMIX play が表示に使う） |
| 56 | BATTLE | S | L:S / W:S | S: 対象外 |  |
| 57 | HS 1P ± | S | L:S / W:S | S: OK |  |
| 58 | HS 2P ± | S | L:S / W:S | S: 未実装 | 表示のみ |
| 70 | SCOREGRAPH | S | L:S / W:S | S: 未実装 | 両スキン |
| 71 | GHOST POSITION | S | L:S / W:S | S: 対象外 |  |
| 72 | BGA off/on/auto | S | L:S / W:S | S: 未実装 | 表示 1 固定、クリック無効 |
| 73 | BGA SIZE | S | L:S / W:S | S: 未実装 |  |
| 74 | JUDGE TIMING | S | L:S / W:S | S: OK |  |
| 75 | JUDGE AUTO ADJUST | S | L:S / W:S | S: 未実装 |  |
| 76 | DEFAULT TARGET RATE | S | L:S / W:S | S: 未実装 | 表示値 90 固定、クリック無効 |
| 77 | TARGET | S | L:S / W:S | S: 未実装 |  |
| 80 | SCREEN MODE | S | L:S / W:S | S: OK |  |
| 81 | COLOR MODE | S | L:— / W:— | S: 対象外 |  |
| 82 | VSYNC | S | L:— / W:— | S: OK | 表示のみ |
| 83 | SAVE REPLAY | S | L:S / W:S | S: 対象外 |  |
| 90 | お気に入り（off/favorite/ignore） | S | L:— / W:S | S: 未実装 | WMIX が使う |
| 91-96 | 難易度直接指定 | S | L:S / W:S | S: OK |  |
| 101-113 | キーコンフィグ 1P | K | L:K / W:— | K: 画面なし |  |
| 121-133 | キーコンフィグ 2P | K | L:K / W:— | K: 画面なし |  |
| 140-143 | 鍵盤変更 / 鍵盤数切替 | K | L:K / W:— | K: 画面なし |  |
| 150-157 | キーコンフィグ スロット | K | L:K / W:— | K: 画面なし |  |
| 170-184 | スキンセレクト 種別 | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし | WMIX はシステムパネル内で使う |
| 190 | スキン切り替え | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし |  |
| 200-209 | ヘルプ表示 | S | L:S / W:S | S: OK |  |
| 210 | IR ページ | S | L:S / W:S | S: 対象外 |  |
| 220-229 | スキンカスタマイズ切替 | S,SS | L:SS / W:S | S: 未実装<br>SS: 画面なし |  |
| 230-234 | コースセレクト決定/取消/編集/削除 | S | L:S / W:S | S: 対象外 |  |
| 240-253 | コースオプション | S | L:S / W:S | S: 対象外 |  |
| 260-269 | ランダムコースオプション | S | L:S / W:S | S: 対象外 |  |

**拡張**

| ID | 意味 | シーン | 使用 | BMjoy |
|---|---|---|---|---|
| 55 の値 5 | HS-FIX MAINBPM | S | L:— / W:— | BMjoy は入れている（要判断） |
| 80 の値 2 | ボーダーレス | S | L:— / W:— | 未実装 |
| 331 | リフト ON/OFF | S | L:— / W:— | 未実装 |
| 400-401 | スケーリングフィルタ / ストレッチ | S | L:— / W:— | 未実装 |

## 3. スキン命令（ヘッダ命令・SRC/DST・特殊画像）

`src/skin/lr2_skin.cpp` が解釈する命令と、解釈しない命令。使用欄は LR2 標準 / WMIX_HD のどのシーンで使っているか。

### 3.1 解釈しない命令

| 命令 | 意味（LR2） | 使用 | BMjoy への影響 |
|---|---|---|---|
| `#FLIPRESULT` | プレイスキンに書くと、リザルトで 1P と 2P のスコアデータを入れ替える（右側プレイ用）。op350 偽 / 351 真 | **L: P（PLAYSIDE=RIGHT のときの `7keys/7_RL*.csv`, `7_RR*.csv`、`Play_half/7.csv`）**, R（#IF 912 の中）, CR / W: CR | 読まない。LR2 標準 7 鍵で PLAYSIDE を RIGHT にすると、LR2 はリザルトで自分のスコアを 2P 側に移し、result.lr2skin が `#ELSEIF,901,351` で `result_sc_r.csv` を選ぶ。BMjoy は op350/351 が両方偽なので `#ELSE` の result_normal.csv になる（§2.2） |
| `#DISABLEFLIP` | `#FLIPRESULT` を打ち消す。op350 真 / 351 偽 | L: R, CR / W: R, CR | 同上 |
| `#SRC_GAUGECHART_2P` / `#DST_GAUGECHART_2P` | 2P のゲージ推移グラフ | L: R / W: R | SP では LR2 も描かない（BATTLE・フリップ・ゴーストバトル時のみ）。対象外 |
| `#SCENETIME` | 決定画面の長さ（経過でプレイへ） | L: D / W: D | 決定画面が無い |
| `#IGNOREINPUT` | 決定画面で、この時間まで ESC 以外の入力を無視（WMIX のコメントより） | W: D | OpenLR2 も解釈しない。原典の挙動は WMIX のコメントだけが根拠 [?] |
| `#SAMPLEBMS` | スキンセレクトのプレビュー用 BMS | L: SS | スキンセレクトが無い |
| `#TEXTMERGIN` | ヘルプ表示の余白 | 未使用 | readme のヘルプ表示で既定 10,10 を足す処理が無い（影響は小） |
| `#FLIPSIDE` | BATTLE で左右反転 | 未使用 | 対象外 |
| `#CUSTOMFOLDER` | （OpenLR2 にある）フォルダ単位の `#CUSTOMFILE` | 未使用 | — |
| `#NOBGA`, `#HORIZONTAL`, `#SRC/DST_MASK`, `#SRC/DST_THUMBNAIL`, `#EVENT_*`, `#RESOLUTION`, `#INFORMATION` の幅・高さ | OpenLR2 の拡張・イベントモード・スキンセレクト用 | 未使用 | — |

### 3.2 解釈するが、扱いが足りないもの

| 命令 | 状態 | 使用 |
|---|---|---|
| `#IMAGE`（動画） | mpg/avi/wma/ogv は読まない（空の画像になる） | — |
| `#SCRATCHSIDE` | 値は読むが使っていない（LR2 では 7→5 鍵の使用レーンと皿の位置に使う。7 鍵 SP では影響は小さい [?]） | L: P / W: P |
| `#SRC_NOTE` などのレーン 8〜19 | 読むが描かない（1P の 0〜7 だけ描く） | P他 |
| `#SRC/DST_NOWJUDGE_2P`, `NOWCOMBO_2P`, `GROOVEGAUGE`[1], `LINE`[1], `JUDGELINE`[1] | 読むが描かない | P他 |
| `#DST_IMAGE` の op4=2（2P 皿の回転） | 回転しない（op4=1 だけ対応） | L: P他 / W: P他 |
| `#SRC_BAR_MY_LAMP`, `BAR_RIVAL_LAMP`, `BAR_RIVAL`, `BAR_RANK`, `BAR_STAR`, `#DST_BAR_STAGEFILE` | 読むが描かない（ライバル・IR 無し） | L/W: S |
| DST の blend 10（反転 src） | 描画で既定のαになる | L: D（12 箇所）, S（1 箇所） |
| 特殊画像 gr100（STAGEFILE） | プレイ・リザルトで読み込んでいない | **W: R（2 箇所）**, L/W: D |
| 特殊画像 gr101（BACKBMP） | プレイで読み込んでいない | **W: P（AC_1p.csv だけで 24 箇所。op194/195 で出し分け）** |
| 特殊画像 gr102（BANNER） | 選曲で `#RELOADBANNER` 時のみ読む（select 仕様どおり） | L/W: S |
| 特殊画像 gr110/111（黒・白 1 ドット） | 対応済み | — |

### 3.3 解釈する命令（問題なし）

`#INFORMATION`, `#CUSTOMOPTION`, `#CUSTOMFILE`, `#ENDOFHEADER`, `#INCLUDE`, `#IF`/`#ELSEIF`/`#ELSE`/`#ENDIF`, `#SETOPTION`, `#IMAGE`, `#LR2FONT`, `#FONT`, `#TRANSCOLOR`（綴り違いの `#TRANSCLOLR` も）, `#STARTINPUT`（3 値）, `#LOADSTART`, `#LOADEND`, `#PLAYSTART`, `#FADEOUT`, `#CLOSE`, `#SKIP`, `#SRC/DST_IMAGE`, `NUMBER`, `TEXT`, `BARGRAPH`, `SLIDER`, `BUTTON`, `BGA`, `NOTE`, `MINE`, `LN_START/END/BODY`, `AUTO_*`, `LINE`, `JUDGELINE`, `GROOVEGAUGE`, `NOWJUDGE_1P`, `NOWCOMBO_1P`, `GAUGECHART_1P`, `SCORECHART`, `BAR_BODY`, `BAR_BODY_ON/OFF`, `BAR_CENTER`, `BAR_AVAILABLE`, `BAR_TITLE`, `BAR_FLASH`, `BAR_LEVEL`, `BAR_LAMP`, `ONMOUSE`, `MOUSECURSOR`, `README`, `#HELPFILE`, `#RELOADBANNER`。

## 4. 優先して直すもの（SP 7 鍵の選曲・決定・プレイ・リザルトを LR2 標準と WMIX_HD で正しくする）

「実スキンが実際に使っていて、いま見た目か挙動が LR2 と違うもの」を上に置いた。

### A. 少ない手間で見た目が直るもの（バグ修正に近い）

1. **リザルトの op 900〜999 がプレイスキンの値を見ている**（`render/skin_play.cpp` の `option()` の default 節が `PlayContext::skin`＝プレイスキンの `op[]` を引く）。リザルト描画の間は `ctx.skin` をリザルトスキンにする。WMIX result が DST op 928 を使う。
2. **op 80/81（ロード未完了/完了）**: LR2 では選曲・リザルトで timer40 が立たないので op80 が常に真。BMjoy は選曲で偽、リザルトではプレイのタイマーを見て偽。LR2 標準の select/result の回転する CIRCLE 背景が消えている。
3. **op 350/351**: `#FLIPRESULT`/`#DISABLEFLIP` を読み、プレイスキンに `#FLIPRESULT` が無ければ 350 真・351 偽にする（LR2 標準 result.lr2skin の分岐）。`#FLIPRESULT` があるとき（LR2 標準の右側プレイ）の 1P/2P データの入れ替えは C に回す。
4. ~~op 54/55（AUTO SCRATCH 1P）をプレイでも返す~~：AUTO SCRATCH を無くした（7k モードに置き換え）ので、常に OFF を返す。
5. **ボタン 55（HS-FIX）をプレイで設定値にする**（WMIX play の HS-FIX 表示が常に OFF）。
6. **timer 44 をプレイ開始時に HARD 系ゲージなら立てる**。
7. **数値 45〜49 のプレイ・リザルト**: 選曲と同じ「同フォルダの難易度別レベル」を渡す（WMIX の play/result が使う。いまは他の難易度が 0 と出る）。
8. **op 308 / 318（EX 0）**、**op 622**（真を返す）、**op 38/39 と文字列 71 の不整合**を揃える。
9. **timer 152 をオートプレイ（記録しないプレイ）では飛ばして timer2 へ**。

### B. 決定画面（D）を作る

LR2 標準・WMIX とも決定画面スキンがあり、ここが無いと曲決定からプレイまでの演出がまるごと抜ける。必要なもの:
- シーン: timer 0 / 1 / 11、#STARTINPUT 経過後に鍵盤・Enter・左クリックで timer3（`#SKIP` 経過でプレイへ）、ESC・右クリック・START+SELECT で timer2（`#FADEOUT` 経過で選曲へ戻る）、`#SCENETIME` 経過でプレイへ。`#IGNOREINPUT` は WMIX のコメントどおりに入れるか判断が要る [?]。
- 特殊画像 gr100（STAGEFILE）。
- op: 2, 52/53（#IF）, 150〜155, 190/191, 290, 505〜509, 900 番台。数値 45〜49。文字列 10, 11, 13, 14, 15。DST blend 10（LR2 標準 decide が 12 箇所で使う）。
- 曲の読み込みは決定画面の裏で始める（LR2 はプレイ画面の #LOADSTART から読み込むが、決定音・STAGEFILE 表示は決定画面）。

### C. プレイ・リザルトの残り

1. **gr101（BACKBMP）をプレイで、gr100（STAGEFILE）をリザルトで読む**（WMIX play は op195 のときに BACKBMP を全面に出す設計）。
2. **timer2 のフェードアウト**: ESC・START+SELECT・全ノーツ後の START/SELECT でも timer2 → `#FADEOUT` 経過でリザルト（いまは即リザルトでフェードアウト演出が出ない）。
3. **数値 100 / 107 とゲージ画像の補間**（スコアは 500ms で数え上げ、ゲージは 1% あたり 10ms）。
4. **文字列 1（ターゲット名）**（LR2 標準 7 鍵プレイスキンが使う）。ターゲット機能を入れるなら 120〜136、バーグラフ 30〜37、op 310〜318 も一緒に。
5. **リザルトのフリップ**（`#FLIPRESULT`）: LR2 標準 7 鍵の PLAYSIDE=RIGHT では、リザルトで自分のスコアを 2P 側（数値 120〜136、op 310〜318、バーグラフ 30〜37、`#GAUGECHART_2P`）に移し、op 90/91・330〜347・数値 171〜178 は 2P 側を見る。これをしないと右側用のリザルト（result_sc_r.csv）を選んでも中身が合わない。
6. スライダー 4（SUD+）のマウスドラッグ。

### D. 選曲画面（WMIX / LR2 標準のパネルの中身）

1. **op 118〜145（クリア時のオプション履歴）**: 両スキンとも曲情報欄に出す。DB にクリア時のゲージ・ランダム・エフェクト・アシストを記録する必要がある。
2. **数値 80〜89（ベストの判定内訳）**: DB に PG〜PR の数を記録する（いまは常に 0）。
3. **数値 30〜39（プレイヤー累計）**: パネル6 用。DB から集計できる。
4. **op 70〜79（レベルバーの上限超え）**、**op 174/175（付属テキストの有無）**、**op 170/171・178/179**。
5. **ボタン 11/12 と文字列 60/61 を LR2 の並びにする**（いまは BMjoy 独自の並び。メモリの「LR2 挙動の維持」に反する）。ボタン 55 の MAINBPM（OpenLR2 拡張）と CONSTANT の扱いも要判断。
6. 2P 側の値を 1P で代用している表示（数値 15、文字列 66/85、ボタン 41/51）は、2P の設定を持たないなら LR2 の既定値（GROOVE, OFF）を返す。
7. 2P 側の鍵盤タイマー 110〜117 / 130〜137（2P 側コントローラーで選曲したときの光）。
8. ボタン 70/72/73/75/77 と文字列（スコアグラフ・BGA・ターゲット等）は、機能を入れるかどうかの判断が先。タグエディター（文字列 20〜29、ボタン 18）、お気に入り（ボタン 90）、スキンセレクト関連（WMIX のパネル2）も同じ。

### E. 当面不要（機能自体が無い）

DP・BATTLE・2P、IR・ランキング、ライバル、コース、トライアル、エフェクタ（EQ/FX/ピッチ/音量）、リプレイ、ゴースト、キーコンフィグ画面・スキンセレクト画面のスキン化。

## 5. 不確かな点

1. 資料の「SRC のタイマーが止まっていれば描かない」と、OpenLR2・BMjoy の「0 コマ目を描く」の違い（§2.1 冒頭）。
2. timer 41 で「START/SELECT が押されていないこと」を待つ条件（OpenLR2 では `inputID` も見ている。鍵盤の番号付けが確認できていない）。
3. timer 140 の積算方法の違い（STOP シーケンス中の進み方）。
4. timer 172〜174 の資料（IR 接続）と OpenLR2（鍵盤6長押し）の食い違い。
5. バーグラフ 26 の最大値（資料 200000 / OpenLR2 20000）。
6. op 332 で旧記録が無いときの最小 BP の値。
7. op 340〜347 が「今回のランク」か「更新後（旧と新の良い方）」か。OpenLR2 は今回のランク。
8. op 46/47（難易度フィルタ）は資料に無く、beta2 の履歴では 42/43 だった。OpenLR2 の番号を採っている。
9. `#IGNOREINPUT` の意味は WMIX のコメントだけが根拠。
10. LR2 標準 select が使う数値 300〜382 の一部、文字列 120〜134 は資料・OpenLR2 とも定義が無いか未実装（IR ランキング表用と推定）。
11. WMIX_HD は「LR2 with Fast/Slow」系の拡張番号（数値 210・212・214）を使っている。原典 LR2 では IR の数値なので、原典どおりにすると WMIX の FAST/SLOW 表示は出ない。BMjoy は拡張として実装済み。
