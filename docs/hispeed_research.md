# ハイスピード調査（LR2 / beatoraja / IIDX / osu!mania）

調査日 2026-10-19。仕様（挙動）だけを書く。コードの表現は写していない。
記号: **[?]** = 出典で確認できていない／推測を含む。

出典の略記:

- `OLR2/` = `/Users/fdk/BMjoy/参考に/OpenLR2-main/LR2/`（LR2 beta3 100201 の逆コンパイル再実装）
- `LR2help/` = `/Users/fdk/BMjoy/参考に/LR2beta3 2/help/`（CP932。行番号は UTF-8 変換後も同じ）
- `LR2hist` = `/Users/fdk/BMjoy/参考に/LR2beta3 2/history.txt`
- `BRJ/` = `/Users/fdk/BMjoy/参考に/beatoraja-master/src/bms/player/beatoraja/`
- `RD/` = `/Users/fdk/BMjoy/src/`

注意: OpenLR2 には原作 LR2 に無い拡張が混ざっている。README と `new_command(*.md)` で拡張と明記されているもの:
**MAINBPM の HS-FIX**（`OpenLR2-main/README.md:35`）、**#SCROLL**（`new_command(bms).md:5`、2026-06 追加）、**LIFT**（`new_command(skin).md` の 421/422・slider 27/28・button 331、2026-08 追加。コード中にも `Extension: OpenLR2 lift` の注記）。
以下「LR2」と書くのは原作の挙動、拡張は「OpenLR2 拡張」と書き分ける。

---

## Part A — LR2 のハイスピード

### A.1 ノーツの画面上の位置

#### 時間軸（読み込み時に作る表）

LR2 は読み込み時に、各オブジェクトに 3 つの時刻を持たせる（`OLR2/LR2_bmsload.cpp:2750-2752`）。

- `realTiming` … 実時間 ms
- `bmsTiming` … 判定・小節線用の「譜面位置」
- `renderTiming` … 描画用の「譜面位置」

譜面位置の単位は **4/4 の 1 小節 = 1920**（1 拍 = 480）。小節長 `#xxx02` は掛けて伸縮する。オブジェクト間の増分（`LR2_bmsload.cpp:2714-2731`）:

- 実時間: Δreal = 240/BPM × 小節長 × Δ小節 × 1000 [ms]（＋ STOP 分）
- **通常（HS-FIX が CONSTANT 以外）**: Δpos = 小節長 × 1920 × Δ小節。描画位置はさらに `#SCROLL` 倍率を掛ける（`:2727`。#SCROLL は OpenLR2 拡張、`:2875-2876`）
- **CONSTANT（またはコースの SCROLL SPEED=CONSTANT）**: Δpos = Δreal × 1.2（`:2717-2721`）。#SCROLL 倍率は掛けない

1.2 という値は BPM 150 の換算と一致する（150 × 1920 / 240000 = 1.2 単位/ms）。つまり **CONSTANT は「曲全体を BPM 150 固定とみなした位置」**になる。

STOP の扱い（`:2671-2711`, `:2868-2871`）:

- STOP 時間 = STOP値/192 × 240000/BPM [ms]（`:2673`）
- 時刻表 `bpmt_data` に「STOP 開始（実時刻 t, 位置 p）」と「STOP 終了（実時刻 t+stop, 位置 p）」の 2 点を入れる
- 増分 Δpos には STOP 時間を含めない（`:2715`）。そのため STOP 中は位置が進まず、**ノーツは止まる**。CONSTANT でも同じ（Δpos に stop 分が入らない）
- `LR2hist:1140`「constant時のストップシーケンスの処理を修正」（071202）

再生中は、実時刻（曲タイマー＋判定タイミング `judgetiming`）を時刻表で線形補間して描画位置に変換する（`RealTimeToRenderTime`, `LR2_bmsload.cpp:1004-1026`。呼び出しは `Scene04_Play.cpp:398`）。

#### 画面上の位置（`OLR2/Scene04_Play.cpp:463`、小節線は `:421`）

```
y_offset[px] = (現在の描画位置 − ノーツの描画位置) × (basespeed/100) × speedmultiplier × hiSpeed / 600
```

- `hiSpeed` は整数で、100 = 1.00 倍
- `basespeed`（基本スクロールスピード）は既定 100（`LR2_configsave.cpp:1081`, `LR2help/8.html:129-131`）
- `speedmultiplier` は HS-FIX から読み込み時に 1 回だけ決まる定数（後述）
- `y_offset` は判定ライン（ノーツの DST の y ＋ スキン調整 `adjust.note_y`）からの相対位置で、上が負（`:499-505`）
- 単位はスキン座標（640×480）の px

具体値: HS 1.00・basespeed 100・倍率 1 のとき、**4/4 の 1 小節 = 1920×100/600 = 320 px（1 拍 80 px）**。

- **レーンの高さは式に入らない。** 速さは px/拍 の絶対値で決まる。レーンが長いスキンほど、ノーツは早く見え始める
- 画面外のノーツの判定には `dst_NOTE[1]` の高さとスキン調整 `size_y` を使う（`:316-322`, `:451`）

#### 結論（A.1 の問いへの答え）

| 問い | 答え | 確度 |
|---|---|---|
| 1 曲の中で BPM が変わると（ソフラン）スクロール速度は変わるか | **変わる**（HS-FIX が CONSTANT 以外なら）。描画位置は拍で進むため、速さ = BPM × 1920/240000 × 倍率 × HS/600 [px/ms] | 高（コードで確認） |
| 速さは BPM に比例するか | **比例する**。式は v[px/ms] = BPM × 0.008 × (base/100) × mult × (HS/600)（OpenLR2 拡張の #SCROLL がある場合はさらに × SCROLL）。小節長は速さに影響しない | 高 |
| HS-FIX MAX/MIN/AVERAGE は何をするか | 読み込み時に **曲ごとの定数倍率** `speedmultiplier = 150 / 基準BPM` を 1 回だけ計算する（`LR2_bmsload.cpp:3314-3330`）。曲中の挙動は OFF と同じで、BPM 変化に比例して速さは変わる。基準 BPM の速さが「HS-FIX 無し・BPM 150」と同じになる | 高（コード＋説明書 `LR2help/4.html:91-96`） |
| START BPM の選択肢はあるか | **無い。** 列挙は OFF / MAXBPM / MINBPM / AVERAGE / CONSTANT（＋ OpenLR2 拡張の MAINBPM）（`OLR2/structure.h:250-256`） | 高 |
| OFF の倍率 | 1.0。曲の BPM がそのまま速さに出る（BPM 300 の曲は BPM 150 の曲の 2 倍速い） | 高 |
| CONSTANT は何をするか | **BPM 変化を完全に無視**し、実時間に比例してスクロールする（BPM 150 相当で一定）。倍率は 1.0（`:3330` の else に落ちる）。#SCROLL も無視。STOP では止まる | 高 |
| CONSTANT のペナルティ | スコア保存しない（`LR2_statplay.cpp:13`）、クリアは EASY 扱い（`:36`、`LR2_bmsload.cpp:3344-3345` は min≠max のとき強制 EASY）。説明書にも「スコアは保存されず、EASY CLEARとなります」（`LR2help/4.html:96-97`） | 高 |

基準 BPM の数え方（`LR2_bmsload.cpp:2898-2904`, `:2440-2447`）:

- 数えるのは **通常ノーツと LN の始点**（LN 終点は数えない。LN チャンネルはこの時点で始点だけ通常チャンネルに直される `:2755-2758`）。地雷・不可視は数えない
- AVERAGE = ノーツ 1 つごとのその時点の BPM の平均（ノーツ数で重み付け）
- MAX / MIN は **ヘッダの `#BPM` の値で初期化**してから、ノーツ位置の BPM で更新する（`:2446-2447`）。そのため、ノーツが 1 つも無い区間の BPM は入らないが、**開始 BPM だけは必ず入る**
- MAINBPM（OpenLR2 拡張）= ノーツ数が最多の BPM（`:3315-3325`）。同数のときは `unordered_map` の走査順で決まるので不定 [?]

### A.2 ハイスピードの値・操作・レーンカバー・LIFT

#### 値の範囲と刻み

| 項目 | 既定値 | 出典 |
|---|---|---|
| hs（現在値） | config に無ければ 100（初期化時は 200） | `LR2_configsave.cpp:1055`, `:940` |
| hsmin | 10（= 0.10） | `:1061` |
| hsmax | 900（= 9.00） | `:1062` |
| hsmargin（1 回の変化量） | 10（= 0.10） | `:1063` |
| shuttermargin（レーンカバー 1 回の変化量） | 10 | `:1066` |
| basespeed | 100 | `:1081` |

- 説明書: MIN/MAX/MARGIN はコンフィグで設定可能（`LR2help/8.html:124-127`）。「HSの最大値、最小値、変化量、レーンカバーの変化量を設定できるようになりました」（`LR2hist:743`）
- スキンの HS スライダー（type 2/3）は 10〜990 の範囲で動く（`LR2_skinobject.cpp:2555`, `:2559`）

#### プレイ中の操作（`OLR2/LR2_gameloop.cpp:163-327`、説明書 `LR2help/7.html:44-53`）

- **START か SELECT を押しながら**:
  - 鍵盤 1・3・5（白）で HS −margin、2・4（黒）で +margin（`:234-246`）
  - 6 / 7: レーンカバーが有効で表示中なら、6 でカバーが上がる（値 −margin）、7 で下がる（+margin）。カバーが無ければ 6 で HS +、7 で HS −（`:274-306`）
- キーボードのカーソル上下で HS ±margin（範囲の端で反対側へ回る）、左右でレーンカバー ±shuttermargin（`:476-487`）
- マウスホイールでレーンカバー ±1（`:183-185`）
- START か SELECT を片方だけ 250 ms 以内に 2 回押すと、レーンカバーの表示が一時的に切り替わる（`:385-411`。`LR2help/7.html:53`、`LR2hist:826`「プレイ中のスタート二回押しによるレーンカバーのオンオフ」）
- 値は毎フレーム `hsmin..hsmax`、カバーは 0..100 に丸める（`:330-346`）
- 新しい HS は次のフレームの描画から即座に効く。ノーツの位置は毎フレーム HS から計算するため（A.1 の式）
- **皿（スクラッチ）では HS もカバーも変わらない**（原作 LR2）。START/SELECT＋皿は OpenLR2 拡張の LIFT 用（`:192-232`）
- 開始前の待ち時間でも同じ操作ができる。play シーン（`procSelecter == 4`）の間は時間で制限していない（`:163`, `:187`）。確度は中。原作で曲開始前に入力を受け付けない期間があるかは [?]
- 選曲画面でも HS と HS-FIX を変えられる（`Scene02_Songselect.cpp:1432-1471`）

#### レーンカバー（SUDDEN+）

- 説明書「要はSUD+です」（`LR2help/4.html:162-163`）
- 値 `lanecoverv` は 0〜100（`LR2_gameloop.cpp:330-331`）
- 描画はスキンのスライダー（type 4=1P / 5=2P）が担う。カバーの絵を「値/100 × 可動幅 op2」だけずらす（`LR2_skinobject.cpp:2562-2575`, `SliderByTime :4132-4211`）
- **ゲームはカバー位置を速度や表示時間の計算に一切使わない。** カバーはスキンが上に重ねるただの絵。カバーとノーツが合うかはスキン（可動幅をレーン長に合わせるか）次第
- プレイ中はスライダーをマウスでドラッグして動かすこともできる（`SliderByTime` のドラッグ処理）
- 変更中は番号表示が出る:「レーンカバーの位置(0-100)を数値を表示するようにしました」（`LR2hist:140`）。オプション 270 は「START/SELECT を押していて、カバーが有効」（`LR2_skinobject.cpp:930-936`）

#### 効果オプション HIDDEN / SUDDEN / HID+SUD

- `m_HIDSUD`（`LR2help/4.html:60-63`）
- 1P 側は判定ラインまでの高さの固定 60% 位置で切る（`Scene04_Play.cpp:330-346`）
- 2P 側のコードは lanecoverv を混ぜた式になっていて、1P 側と一致しない（`:352-367`）。原作の挙動か移植の誤りかは [?]
- RD は 1P 側に合わせて判定ラインの位置を使っている（`RD/skin/skin_renderer.cpp` add_notes の注記）

#### LIFT

- **原作 LR2 に LIFT オプションは無い**
- 代わりに、プレイ中にキーボードの 6 を押しながらカーソル上下でノーツ全体（判定位置 `adjust.note_y`）を ±10 px 動かせる（`LR2_gameloop.cpp:461-465`）。`LR2hist:15`「プレイ中にキーボードの6キーを押したときの動作を微変更。某LIFTオプションっぽい挙動になります」
- OpenLR2 拡張の LIFT は 0〜100、ノーツの原点を「lift/100 × レーン高」だけ上げる（`Scene04_Play.cpp:23-45`）

### A.3 緑数字は LR2 にあるか / スキンに渡す数値

- **LR2 には緑数字（表示時間）に相当する値は無い。** 速度は px/拍 で決まり、レーン高・カバー位置をゲームが知らないので、表示時間を計算できない
- スキンに渡す値（`LR2_skinobject.cpp:1648-1660`）:

| 種類 | ID | 値 | `docs/lr2_skin_ids.md` との照合 |
|---|---|---|---|
| NUMBER | 10 | 1P HS（100 = 1.00） | 一致（:324） |
| NUMBER | 11 | 2P HS | 一致（:325） |
| NUMBER | 14 | 1P レーンカバー 0-100 | 一致（:328 "SUD+ 1P"） |
| NUMBER | 15 | 2P レーンカバー | 一致（:329。RD は 1P 値を返す＝誤り、と既に記載） |
| SLIDER | 2 / 3 | HS 1P / 2P（範囲 10〜990） | 一致（:491-492） |
| SLIDER | 4 / 5 | レーンカバー 1P / 2P | 一致（:493-494） |
| BUTTON | 55 | HS-FIX（0..4、OpenLR2 は 5=MAINBPM） | 一致（:542） |
| OPTION | 270 | START/SELECT を押していてカバー（OpenLR2 では LIFT も）有効 | 一致（:263。リフト部分は :310 で未実装） |
| OPTION | 274 / 275 | OpenLR2 拡張: カバー表示中 | 一致（:311） |
| NUMBER | 421 / 422 | OpenLR2 拡張: LIFT 値 | — |

- BPM の表示は別の ID（1815/1818 付近の max/min BPM、1978 付近の現在 BPM など）。緑数字系の ID は無い

### A.4 RD の現状と LR2 の比較

| 項目 | RD | LR2 | 判定 |
|---|---|---|---|
| 位置の式 | `y = (render_beat − note.pos) × 480 × mult × HS × base/100 / 600`（`RD/skin/skin_renderer.cpp:248-252`）。pos は拍単位で STOP 中は進まない（`RD/chart/bms_parser.cpp:241-250`, `chart.cpp:15-19`） | 1920/小節 × HS/600 | **一致**（1 拍 480 単位と同じ） |
| ソフランで速さが変わる | 変わる | 変わる | 一致 |
| HS-FIX MAX/MIN/AVERAGE/MAIN | 150/基準 BPM の定数倍率（`RD/app/play_session.cpp:124-154`） | 同じ | 概ね一致。1 点違う: LR2 の MAX/MIN はヘッダ `#BPM` で初期化する（`LR2_bmsload.cpp:2446-2447`）。RD はノーツ位置の BPM だけを使う。**開始 BPM にノーツが無い譜面で値が変わる**。MAINBPM が同数のときは、RD は小さい BPM、OpenLR2 は不定 |
| CONSTANT | 選べない（`RD/app/select_scene.cpp:195`, `:937`「CONSTANT は未対応」）。値 4 を受け取ると倍率 1.0 で OFF と同じ動き | 実時間比例・BPM 150 相当・#SCROLL 無視・STOP で停止・スコア保存なし | **未実装** |
| #SCROLL | 対応なし | 原作にも無い（OpenLR2 拡張） | 原作とは一致 |
| HS 操作 | START/SELECT＋1・3・5 で −、2・4 で +、6・7 はカバー（無ければ HS）、カーソル上下は端で回る、ホイールでカバー、250 ms 以内の 2 回押しでカバー切替（`RD/app/play_session.cpp:440-505`） | 同じ | 一致 |
| カバーのスライダーをマウスでドラッグ | 不可（`lr2_skin_ids.md:493`） | 可 | 既知の差 |
| 2P のカバー・HS | 1P 値を流用 | 個別 | 既知の差（SP では影響なし） |

---

## Part B — beatoraja（仕様のみ）

### B.1 設定（`BRJ/PlayConfig.java`。鍵盤モードごとに別々に持つ `PlayerConfig.java:411-430`）

| 項目 | 既定 | 範囲・刻み | 出典 |
|---|---|---|---|
| hispeed | 1.0 | 0.01〜20 | `:16-19`, `:251` |
| duration（表示時間 ms。白数字） | 500 | 1〜10000 | `:24-27`, `:252` |
| fixhispeed | **MAINBPM** | 0 OFF / 1 STARTBPM / 2 MAXBPM / 3 MAINBPM / 4 MINBPM | `:43-49` |
| hispeedmargin | 0.25 | 0〜10 | `:54-57` |
| lanecover / enablelanecover | 0.2 / true | 0〜1 | `:62-66` |
| lift / enablelift | 0.1 / false | 0〜1 | `:70-74` |
| hidden / enablehidden | 0.1 / false | 0〜1 | `:78-82` |
| lanecovermarginlow / high | 0.001 / 0.01 | | `:87-91` |
| lanecoverswitchduration | 500 ms | | `:95` |
| hispeedautoadjust | false | | `:97-99` |
| enableConstant / constantFadeinTime | false / 100 ms | −1000〜1000 | `:32-38` |

### B.2 位置の式（`BRJ/play/LaneRenderer.java`）

- 現在時刻 t = 経過時間 ＋ judgetiming（`:300-301`）
- レーン下端 h_l（LIFT 有効なら レーン高 × lift だけ上げる）、上端 h_u（`:324-325`）
- 1 小節あたりの px: R = (h_u − h_l) × HS × speed（`:326`）。**HS 1.0 で 4/4 の 1 小節がちょうどレーン全体（LIFT 後の残り）**になる
- ノーツ位置は、小節位置の差 × 区間の scroll 値 × R を積み上げて求める。STOP 中は進まない（`:390-399`）
- 速さ v = (h_u − h_l) × HS × speed × scroll × BPM / 240000 [px/ms]。**曲中の BPM 変化に比例して速さが変わる**（IIDX と同じ考え方）
- `speed` は #SPEED 系の補間値（`:259-286`）。CONSTANT のときは 1（`:320`）

LR2 との違い: LR2 は px/拍 の絶対値。beatoraja は **レーン高を基準にした相対値**なので、スキンが違っても同じ HS なら表示時間が同じになる。

### B.3 表示時間（duration）と緑数字

- 今の表示時間 region = 240000 / (BPM_now × HS × speed × scroll) [ms]（`:321`）
- `currentduration = round(region × (1 − cover))`（cover はカバー有効時だけ）（`:329-330`）
- **LIFT は表示時間を変えない**（R がレーン残り長に比例するため打ち消し合う）。HIDDEN も計算に入らない
- **緑数字 = duration × 3/5**（整数除算）（`skin/property/IntegerPropertyFactory.java:305-316`）。つまり **1/600 秒単位**（ms × 0.6）
- スキンの数値 ID（`skin/SkinProperty.java:258-283`, `IntegerPropertyFactory.java`）:

| ID | 値 |
|---|---|
| 312 | 白数字（プレイ中は currentduration、選曲は保存値） |
| 313 | 緑数字（312 × 3/5） |
| 1312〜1327 | 現在/MAIN/MIN/MAX BPM での表示時間（白/緑 × カバー ON/OFF の組み合わせ）。`round(240000/BPM/HS × (1−cover) × (0.6))` |
| 310 / 311 | HS の整数部 / 小数 2 桁 |
| 10 | HS × 100（LR2 互換） |
| 14 | cover × 1000 |
| 314 | lift × 1000 |
| 315 | hidden × 1000 |
| 316 | (1 − lift) × cover × 1000 |

- OPTION: 270 カバー変更中、271 カバー ON、272 LIFT ON、273 HIDDEN ON、400 CONSTANT（`SkinProperty.java:857-860`, `:960`）

### B.4 ハイスピ固定（緑数字固定）と操作

- **fixhispeed が OFF 以外のときは、保存するのは duration で、HS は毎回計算する**（`play/BMSPlayer.java:838-843`）
  - HS = 240000 / (基準BPM × duration) × (1 − cover)（`LaneRenderer.java:206-210`）
  - 基準 BPM = START / MAX / MAIN / MIN（`:130-157`）
- **カバーを動かすたびに HS を計算し直す**（`setLanecover` → `resetHispeed(基準BPM)`, `:212-215`）。その結果、**基準 BPM での緑数字が一定のまま、カバーとノーツ速度が一緒に変わる**（＝IIDX の「緑数字固定」に近い振る舞い）。OFF のときは HS が変わらないので、カバーを動かすと表示時間が変わる
- START 2 回押し（500 ms 以内）でカバーの表示を切り替える。このとき HS は計算し直さないので、外すと表示時間が伸びる（`play/ControlInputProcessor.java:162-171`, `LaneRenderer.java:217-219`）
- START＋鍵盤で HS を変える（奇数鍵 −、偶数鍵 +）（`ControlInputProcessor.java:78-95`）
  - OFF のときは ±margin
  - 固定のときは ±(初期 HS × margin)。つまり割合で変わる
  - **固定のとき、この変更は一時的。** 保存されず、次にカバーを動かすと duration から計算し直される（`LaneRenderer.java:237-247`, `BMSPlayer.java:838-843`）
- SELECT＋鍵盤で duration を ±1 ms、SELECT＋皿で 50 ms ごとに ±1（`ControlInputProcessor.java:97-119`）
- START＋皿でカバーを動かす（`:246-269`）
  - 50 ms ごとに 0.001
  - 500 ms 以上回し続けると 0.01 刻み
  - アナログ皿は回した量 × 0.001
  - その他: カーソル上下 ∓0.01、ホイール（`:136-154`）
- カバーが OFF で LIFT か HIDDEN が ON なら、同じ操作でそちらを動かす（`:231-239`）
- **hispeedautoadjust**（説明は「皿チョン」）: カバー・LIFT・HIDDEN を動かしたときだけ、**今の BPM** を基準に計算し直す（`LaneRenderer.java:241-243`）。BPM が変わっただけでは何もしない。OFF のときは無効
- 入力はどの状態でも処理する（開始前の待ち時間を含む）（`BMSPlayer.java:802-805`）。HS の操作はプレイ・練習のときだけで、オート・リプレイでは効かない（`:157-160`）

### B.5 CONSTANT（beatoraja）

- LR2 の CONSTANT とは**別物**
- 位置の計算は BPM 比例のまま（speed=1 にするだけ）
- 「今 + duration」より先のノーツを描かない時間による打ち切りで、境界に fade（constantFadeinTime）を付ける（`LaneRenderer.java:355-388`, `:472-500`）
- 速さを本当に一定にするのは、別の譜面書き換えオプション **Scroll Mode = REMOVE**（`pattern/ScrollSpeedModifier.java:36-50`）
  - 全区間を開始 BPM にし、STOP と scroll を消す
  - LIGHT_ASSIST 扱いになる

---

## Part C — beatmania IIDX と osu!mania（Web 調査）


注意:

- KONAMI 公式（p.eagate.573.jp）は調査日に全ページが HTTP 503 で読めなかった。IIDX の内容は BEMANI wiki・ニュース記事・プレイヤーの解説による
- 緑数字の定数などは、プレイヤーが観測から推定した値で、版や筐体によって違う
- 版の名前: **IIDX ZINRAI は 34 作目（2026-09-16 稼働）**（https://bemaniwiki.com/?beatmania+IIDX+34+ZINRAI ）

### C.1 緑数字・白数字

- 定義:「緑数字とは、譜面がレーンの上部から現れて判定ラインに達するまでの表示時間を表した数値」（https://rhychris.hatenablog.com/entry/2021/12/12/202238 ）。大きいほど表示時間が長い（＝遅い）
- **単位: 60fps での表示フレーム数 × 10。すなわち 1/600 秒**
  - 「緑数字 = [60fpsにおける表示フレーム数] × 10」「緑数字30 が 50ms に対応」（https://cheru.hatenadiary.com/entry/2023/12/17/121000 ）
  - 表示時間[ms] = 緑数字 × 5/3。緑 300 ≈ 500 ms（「30=50ms」からの換算）
  - beatoraja の緑数字（ms × 3/5、B.3）もこの定義と一致する
- 式（いずれも推定値）:
  - `緑数字 = 定数 ÷ HS ÷ BPM × (見えている px ÷ 723)`。レーンは 723 px、定数は筐体（32 LM）で 174728・INFINITAS で 174758。値は 0〜9999 の整数、見える範囲が 1px 以下なら 0（https://note.com/masaoblue/n/n02ecb7426a76 、2025-08、作者の観測推定）
  - 旧式: `緑数字 = 174000 / Speed`、`Speed = BPM × HS × 1000/(1000 − SUD+位置)`（http://my.iesaba.com/posts/iidx-fhs-memo/ 、2015）
  - 「緑数字=175300÷速度」。LM 化・フル HD 化で 174000 では合わなくなった、とする（https://note.com/nceahleudatg/n/nddf851370b47 、2023）
  - **現行の正確な定数は [?]**。どの式も「表示時間 ∝ 見えているレーン長 /（BPM × HS）」という形は同じ
- 白数字 = レーンカバーの量
  - 「白数字はリフトとサドプラを合計したもの」（https://note.com/nceahleudatg/n/nddf851370b47 ）
  - 表示値 = px × 999/723。1 px 刻み。SUD+ は 42〜999、LIFT は 0〜830、合計は最大 1000（https://note.com/masaoblue/n/n02ecb7426a76 。推定）
  - 2015 年の資料では「Sudden+は50から1,000の間で設定可能」（http://my.iesaba.com/posts/iidx-fhs-memo/ ）
- SUD+ と LIFT はどちらも見えている長さを縮めるので、緑数字を小さくする（上の式による）。HIDDEN+ が緑数字に入るかは [?]
- どの BPM の値か: **今の BPM の値**。ソフランでは「BPMに応じて急に緑数字が変化します」。選曲画面では譜面の上限・下限を出す（https://rhychris.hatenablog.com/entry/2021/12/12/202238 ）

### C.2 フローティングハイスピード（FHS）

- **導入は IIDX 20 tricoro**（SPADA ではない）
  - "tricoro introduces the Floating Hi-Speed, in which adjustments are done by one-hundredth decimal intervals"（https://remywiki.com/What_is_beatmania_IIDX ）
  - SPADA からは、HS をプレイ画面でしか変えられなくなった（同ページ）
- 切り替え: START を押しながら EFFECT（http://my.iesaba.com/posts/iidx-fhs-memo/ ）
- 動き方（https://note.com/masaoblue/n/n02ecb7426a76 。観測による解説）:
  - FHS を ON にしたときの緑数字を覚えておく
  - カバーを動かすと「HS = 開始時 HS × 今見えている px / 開始時に見えていた px」で計算し直す
  - HS の範囲は 0.50〜10.00。緑の下限は 10
  - 0.50〜10.0 の範囲は 26 Rootage のページにもある（https://bemaniwiki.com/?beatmania+IIDX+26+Rootage ）
- 刻み:
  - START＋鍵盤で 0.5 ずつ
  - カバーが無いとき、START＋皿で 0.01 刻み（http://my.iesaba.com/posts/iidx-fhs-memo/ ）
  - 皿の刻みを約 1/300 とする資料もある。どちらが正しいかは [?]
- **BPM 変化時**: 「FHS設定は曲開始時のBPMで計算される」（http://my.iesaba.com/posts/iidx-fhs-memo/ ）。曲中は HS 倍率が固定で、スクロールは BPM に比例する。**緑数字が開始時の値になるのは開始 BPM の区間だけ**
- クラシック HS（CHS）:
  - 1.00, 1.50, 2.00, 2.25 … 4.00 の 11 段階。2.00 までは 0.5 刻み、以降は 0.25 刻み（https://note.com/masaoblue/n/n02ecb7426a76 , https://note.com/nephilim_ks/n/n065d0a4cd618 ）
  - tricoro 当時は "x0.50 to x4.00 in 0.25 increments"（remywiki）
- ノーマル HS（26 Rootage〜）:
  - 緑数字基準の 10 段階。「HS1…緑数字1200…1段階上げる事に緑数字が100小さくなる」
  - 「開始時のBPMに応じて…HS倍率が自動的に調整」「演奏中はハイスピード倍率はそのまま」（https://bemaniwiki.com/?beatmania+IIDX+26+Rootage ）
  - 27 で 20 段階になった（remywiki）
- 基準 BPM の選択（MAX/MIN/MAIN 等）は IIDX には無いらしい [?]。資料はどれも開始 BPM が基準
- **定速（CONSTANT）は製品版 IIDX に無い**（ZINRAI 時点）
  - 「ノーツ表示時間固定オプション「LANE COVER ADJUST」「SPEED ADJUST」の追加は製品版で見送りとなった」（ロケテスト。https://bemaniwiki.com/?beatmania+IIDX+34+ZINRAI ）
  - その 2 つの中身は [?]

### C.3 ギアチェンジ

#### 皿チョン（tricoro 以降の FHS）

- FHS で SUD+ を出した状態で START を押しながら皿に触れる。カバーが動き、緑数字が FHS 開始時の値に戻るように HS が計算し直される
  - 「初めに設定したノーツの表示時間(緑数字)になるようにHSが調整される」（http://my.iesaba.com/posts/iidx-fhs-memo/ ）
  - "will correct the Hi-SPEED setting to match the original green number"（https://remywiki.com/Beatmania_IIDX_20_tricoro ）
- つまり、**ソフランで BPM が変わったあと、カバーを少し動かせば HS が「今の BPM で元の緑数字」になるように合わせ直される**。白数字（カバー位置）は動かした分だけ変わる
  - 「今の BPM」で合わせ直すと読めるのは資料の記述からの推論。厳密な式は [?]
- SUD+ を出していれば START＋皿で SUD+ が、LIFT だけなら LIFT が動く（https://note.com/masaoblue/n/n02ecb7426a76 ）

#### その他の操作

- START＋鍵盤: HS を直接変える（FHS では 0.5 ずつ）
- FHS 中の START＋EFFECT: いちばん近い CHS の値になる（https://note.com/nephilim_ks/n/n065d0a4cd618 ）
- 33 まで:「BPM変化後にスタートボタンを4回押せばFHS開始時の緑数字に戻る」。白数字は動かない（https://note.com/masaoblue/n/n02ecb7426a76 ）

#### 34 ZINRAI（2026-09-16）の新オプション「譜面表示エリアの固定」（https://bemaniwiki.com/?beatmania+IIDX+34+ZINRAI ）

- 「SUD+およびLIFTの位置が完全に固定され、動かすことも外すこともできなくなる。」
- 「FHS使用時はSTARTボタンを2回押すと緑数字を楽曲開始時(もしくはFHSに切り替えた時点)の値に変更できる。」 **→ カバーを動かさずに HS だけで緑数字を戻すギアチェンジ**
- 「一方、いわゆる皿チョンによる緑数字変更はできなくなる。」
- 「楽曲開始前(GET READYが出るまで)と終了後(最終ノーツ後)に限りSUD+位置を調整することができる。」
- ニュース記事:「フローティングハイスピードを使用している場合、スタートボタンを2回押すとリセットできます。」（https://dtimes.jp/post-1381051/ ）
- 同時に追加されたもの:
  - 「BPM変化予告」（変化前 >> 変化後。速くなるときは青、遅くなるときは赤）
  - 「表示エリア情報常時表示」（白・緑数字をカバー上に常に出す）
- Ver.3（2026-09-30）で「レーンカバー・ハイスピード設定の複数の意図しない挙動」を修正。FHS で緑数字が少しずれる件を含む
- 未確認 [?]:
  - このオプションが ON のときに START＋鍵盤で HS を変えられるか
  - オプション OFF でも START 2 回押しが効くか（dtimes の書き方は曖昧で、bemaniwiki はオプションの中に書いている）

#### 開始前の待ち時間

- 31 EPOLIS 公式ヘルプ（検索結果の抜粋でのみ確認）:「プレー中(または曲開始前)に、STARTボタンを押しながら、ターンテーブルを回すことで、隠す範囲を調整できます」。START を押している間は白・緑数字を表示する（https://p.eagate.573.jp/game/2dx/31/howto/play/option.html ）
- カバーが無い FHS なら START＋皿で HS を細かく変えられる（C.2）
- 待ち時間だけに特有の LIFT・HS 操作があるかは [?]

### C.4 osu!mania

#### osu!stable（https://osu.ppy.sh/wiki/en/Game_mode/osu%21mania 。wiki 側でも要書き直しの印あり）

- スクロール速度は 1〜40。Ctrl/Shift＋`+`/`-`、F3/F4 で変える
- 「Scale osu!mania scroll speed with BPM」が ON のとき（旧方式・既定）: "scale the scroll speed relative to the current BPM played"
- OFF のとき（Fixed）: "enforce a constant scroll speed"
- ただし、どちらも "will still be affected by speed up/down by BPM changes"
- Fixed の正確な式は [?]
- 譜面作りの決まり: "normalising the scroll speed of all timing sections to the most used one in the beatmap"（SV で正規化する。https://osu.ppy.sh/wiki/en/Ranking_criteria/osu%21mania ）

#### osu!lazer（ppy/osu master のソース、2026-10-15 時点）

- `ScrollSpeed` は既定 8.0、範囲 1.0〜40.0、0.1 刻み
- 表示は「{0}ms (speed {1:N1})」で、**時間で見せる**
- 表示時間 = 11485 ms / scroll speed
- 出典: `osu.Game.Rulesets.Mania/Configuration/ManiaRulesetConfigManager.cs`, `UI/DrawableManiaRuleset.cs`, `osu.Game/Localisation/RulesetSettingsStrings.cs`
- 「BPM に合わせる」切り替えは無い
- BPM の扱い:
  - mania は `RelativeScaleBeatLengths = true`。譜面でいちばん多い拍の長さを基準にする
  - 区間ごとの速さ = SV × 基準の拍の長さ / その区間の拍の長さ
  - 方式は既定で Sequential（`osu.Game/Rulesets/UI/Scrolling/DrawableScrollingRuleset.cs`, `osu.Game/Rulesets/Timing/MultiplierControlPoint.cs`）
  - したがって **表示 ms は「最頻 BPM・SV 1.0 での表示時間」**。他の BPM・SV の区間では、BPM と SV に比例して速さが変わる
  - beatoraja の MAINBPM 固定に近い、というのはソースの読み取りによる解釈
- レート系の mod では、time range に tempo × frequency を掛ける。そのため実時間の表示時間は変わらない。スキンの判定位置に合わせて見える長さも調整する（`updateTimeRange`）
- 「Constant Speed」mod（"No more tricky speed changes!"）は `ScrollVisualisationMethod.Constant` に切り替える。**BPM も SV も無視する**（`osu.Game.Rulesets.Mania/Mods/ManiaModConstantSpeed.cs`）

---

## 比較のまとめ

| | LR2 | beatoraja | IIDX | osu!lazer |
|---|---|---|---|---|
| 速度の単位 | px/拍（スキン座標の絶対値） | レーン長/小節 | 緑数字（1/600 s） | 表示 ms |
| 曲中の BPM 変化 | 比例して変わる | 比例して変わる | 比例して変わる | 比例（SV も） |
| 基準 BPM | OFF/MAX/MIN/AVG（＋MAIN 拡張）。読み込み時の定数倍率 | START/MAX/MAIN/MIN から duration で HS を計算 | 開始 BPM（FHS・ノーマル HS） | 最頻 BPM |
| 完全な定速 | CONSTANT（BPM 150 相当。STOP では止まる。スコア保存なし） | Scroll Mode REMOVE（譜面を書き換え。LIGHT_ASSIST） | 無し | Constant Speed mod |
| カバーと速度の連動 | 無し（カバーは絵） | 固定時は、カバーを動かすと HS を計算し直す | FHS の皿チョン | — |
| 今の BPM での合わせ直し | 無し | hispeedautoadjust（カバー操作時） | 皿チョン／START 4 回（〜33）／START 2 回（34 の固定オプション） | 無し |

---

## RD の設計への示唆

ここに挙げるのは選択肢で、決めたことではない。

1. **速度の内部表現**
   - (a) LR2 互換: px/拍 × HS
   - (b) 表示時間（ms、または 1/600 s の緑数字）を基準にする
   - (c) 両方を持ち、LR2 スキン用の ID 10/14 は (a) の値で出す
   - LR2 互換の挙動（A.4 で一致を確認済み）を残すかどうかが分かれ目。LR2 と違う挙動は「拡張」として明示することになる
2. **表示時間の分母**
   - レーンの長さ（判定ラインから上端まで、カバー・LIFT を引いた長さ）をどこから取るか
   - LR2 スキンにはレーン長の定義が無い。候補はノーツ DST の y、またはカバー用スライダーの可動幅 op2（OpenLR2 拡張 LIFT はこれを使う）
3. **基準 BPM の選び方**
   - 候補: 開始 / 最大 / 最小 / 平均 / 最頻（MAIN） / 今の BPM
   - LR2 の MAX/MIN はヘッダ BPM を含む（A.1）
4. **BPM に連動するか**
   - (i) BPM 比例（LR2・IIDX・beatoraja の既定）
   - (ii) 実時間で一定（LR2 CONSTANT、osu! の Constant Speed）
   - (ii) で STOP を止めるか、#SCROLL を無視するか、スコアを保存するかも選ぶ
5. **緑数字固定の範囲**
   - 「基準 BPM での表示時間」を固定するのか（beatoraja・IIDX FHS）
   - 「今の BPM」で合わせ直す操作を用意するのか（皿チョン、IIDX 34 の START 2 回）
6. **ギアチェンジの操作**
   - カバーを動かして HS を合わせ直す（皿チョン型）
   - カバーを動かさず HS だけ戻す（START 2 回型）
   - START＋鍵盤の HS 刻み（固定量 / 割合 / CHS の段階）
   - いずれも LR2 の START 2 回押し（カバー表示の切替）と操作がぶつかる
7. **カバーの単位**: LR2 の 0〜100 ／ beatoraja の 0〜1 ／ IIDX の白数字 0〜1000
8. **LIFT**: 原作 LR2 には無い。入れる場合は表示時間に含めるかを決める（beatoraja は含めない。IIDX は白数字に合算）
9. **開始前の待ち時間の操作**: カバー・LIFT・HS を調整できる期間と、曲中に制限するか（IIDX 34 の固定オプションは開始前と終了後だけ）
10. **スキンに出す値**
    - LR2 には緑数字の ID が無い
    - beatoraja の 312/313/1312〜1327 を拡張として出す方法がある
    - メモリ「画面の文字に注釈を出さない」に従い、出し方はスキン側に任せる
11. **osu!mania 譜面**
    - SV（区間の速度倍率）を #SCROLL 相当として扱う
    - 基準を最頻 BPM にするか（lazer 方式）
    - Constant 相当を用意するか

---

## D. フローティング時の操作（追加調査 2026-10-19）

C.2・C.3 の続き。IIDX のプレー中・開始前の操作を、入力ごとに詳しく調べた。
KONAMI 公式ヘルプ（p.eagate.573.jp）は今回は読めた（C の冒頭の「503 で読めない」は解消）。

版の名前と稼働日:

- 31 EPOLIS: 2023-10-18（https://www.konami.com/amusement/corporate/ja/topics/20231019/ ）
- 32 Pinky Crush: 2024 年 [? 日付は未確認]（https://www.konami.com/arcadegames/video/bm2dx/ ）
- 33 Sparkle Shower: 2025-09-17（https://dic.pixiv.net/a/Sparkle_Shower 。公式では未確認 [?]）
- 34 ZINRAI: 2026-09-16（https://bemaniwiki.com/?beatmania+IIDX+34+ZINRAI ）

主な出典（以下、略記を使う）:

- 【公式HS32】【公式HS33】【公式HS34】 = https://p.eagate.573.jp/game/2dx/{32,33,34}/howto/play/option_speed.html （3 つとも同じ文面）
- 【公式OP34】 = https://p.eagate.573.jp/game/2dx/34/howto/play/option.html （33 も同じ文面）
- 【公式BPM34】 = https://p.eagate.573.jp/game/2dx/34/howto/new/bpm_option.html
- 【公式tri】 = https://p.eagate.573.jp/game/2dx/20/p/howtoplay/option2.html （20 tricoro）
- 【masao】 = https://note.com/masaoblue/n/n02ecb7426a76 （2025-08、32 LM と INFINITAS での観測。推定を含む）
- 【iesaba】 = http://my.iesaba.com/posts/iidx-fhs-memo/ （2015）
- 【remy】 = https://remywiki.com/What_is_beatmania_IIDX ／ https://remywiki.com/Beatmania_IIDX_20_tricoro

### D.1 HI-SPEED ウィンドウの開き方とモード切替

- プレー画面で START を押している間、HI-SPEED ウィンドウが開く。HS の操作は START を押している間だけ（【公式HS34】）
- SPADA 以降、HS はプレー画面でしか変えられない（【remy】。tricoro からとする公式の記述もある【公式tri】）
- START を押しながら EFFECT で FHS に切り替わる。もう一度 START＋EFFECT で通常（ノーマル）または CHS に戻る（【公式HS34】、https://iidx.org/beginner/floating ）
  - FHS → CHS に戻すと、そのときの FHS 値にいちばん近い CHS の段になる（https://note.com/nephilim_ks/n/n065d0a4cd618 ）
  - 【remy】は「EFFECT またはテンキー [0]」で Classic / Rootage（ノーマル）/ Floating を切り替えると書く。テンキーの件は [?]
- ノーマル HS と CHS のどちらを「通常の HS」にするかは、詳細オプション「クラシックハイスピード」で選ぶ（【公式HS34】【公式OP34】）
- ビギナーオプションでは FHS は使えない（【公式HS34】）
- VEFX は HS の操作には使わない。START を押してオプション窓を開いた状態で VEFX を押すと詳細オプションへ移る（【公式OP34】）。プレー中の HS ウィンドウで VEFX が何かをするかは [?]

### D.2 FHS の操作（IIDX 32〜34 の公式ヘルプ。文面は tricoro から変わっていない）

公式の文面（【公式HS34】、【公式tri】も同文）:

- SUDDEN+ あり:「鍵盤でハイスピードを0.5ずつ変更、ターンテーブルでノーツ表示時間を固定したままSUDDEN+の長さを変更することができます。」
- SUDDEN+ なし:「鍵盤でハイスピードを0.5ずつ変更、ターンテーブルでハイスピードを細かく調整することができます。」

入力ごとの整理:

| 入力（START を押しながら） | SUD+ が出ている | SUD+ も LIFT も出ていない | 出典 |
|---|---|---|---|
| 白鍵（1・3・5・7） | HS −0.50 | HS −0.50 | 【公式】は「0.5ずつ」のみ。向きは【masao】の例 |
| 黒鍵（2・4・6） | HS +0.50 | HS +0.50 | 同上 |
| 皿 | SUD+ を動かす。緑数字が FHS 開始時の値になるように HS を計算し直す（＝皿チョン） | HS を細かく変える。0.01 刻み（【remy】【iesaba】）／約 1/300 刻み（【masao】の推定） | 【公式HS34】 |
| EFFECT | FHS ⇔ 通常/CHS の切り替え | 同左 | 【公式HS34】 |

- **白鍵・黒鍵の違いは「向き」だけ。** 鍵盤ごとに刻みが違う、という記述はどの出典にも無い。白鍵・黒鍵どれを押しても ±0.50 [? 鍵ごとに違わないことを明言した出典は無い。違うという出典も無い]
- 通常 HS（ノーマル）でも向きは同じ:「黒鍵盤を押すとHI-SPEEDが1段階上がり、白鍵盤を押すと1段階下がります。」（【公式HS34】）。**FHS で変わるのは、鍵盤が「1 段階」から「±0.50」になることと、皿の役割**
- **鍵盤の ±0.50 は「今の値からの相対」。** 0.25 刻みの格子には揃えない。例: 2.25 で白鍵 3 回 → 0.75（【masao】）
- 範囲: 0.50〜10.00（【masao】、【remy】tricoro、26 Rootage の bemaniwiki）
- **端では止まる（折り返さない）。** 端を越える操作では端の値になり、その後は端から ±0.50 で動く。元の端数には戻らない
  - 「そこから更に白鍵盤を押す → 下限の0.50になる」「この状態で黒鍵盤を押すと1.00、1.50、…というように変化し、元の0.75や2.25に戻すことはできない」（【masao】）
  - 計算結果が範囲を超えるときは緑数字の固定をやめ、HS を端の値にして緑数字を計算し直す（【masao】）
- 鍵盤で HS を変えたとき、固定する緑数字（基準）も新しい値に更新されるか: 【masao】は「START を押しながら鍵盤操作した時」を再計算のタイミングに挙げている。基準の緑数字が新しい HS のものに置き換わると読めるが、明言は無い [?]
- 皿の刻み（SUD+ なし）の 0.01 と 1/300 の食い違い:【masao】は「白数字が表示されず、厳密な確認は難しいため推測値」と断っている。HS の表示は小数 2 桁（tricoro のイベント条件「5.73」【remy】）。どちらが正しいかは [?]
- 皿の速さ（回し続けたときの加速）・アナログ量の扱いは、どの出典にも無い [?]
- LIFT だけが出ているとき（SUD+ なし）: START＋皿で LIFT が動く（C.3、【masao】）。このとき HS をどう計算し直すかは [?]。【公式】の 2 分類（SUD+ あり／なし）には LIFT の場合が書かれていない

### D.3 START 2 回押し・4 回押し（カバーの出し入れと緑数字）

- START を素早く 2 回押すとレーンカバーが消える。もう一度 2 回押すと出る（【公式OP34】、33 も同文）。「素早く」の時間は [?]
- FHS のときは、カバーを出し入れした瞬間に HS を計算し直すことがある（【masao】）:
  - LIFT が出ているとき: SUD+ を外す直前に、SUD+ ありの状態で開始時の緑数字になるように HS を計算し直す
  - LIFT が出ていないとき: SUD+ を外すときは HS をそのままにし、SUD+ を再び出したときに元の緑数字になるように HS を計算し直す
  - 計算の式は皿チョンと同じ
- その結果、33 までは「BPM変化後にスタートボタンを4回押せばFHS開始時の緑数字に戻る」（＝外して出す）。白数字は動かない（【masao】）
- 34 の「譜面表示エリアの固定」が ON のとき、START 2 回押しの意味が変わる（D.6）

### D.4 SUDDEN+ / HIDDEN+ / LIFT の調整

- SUDDEN+:「プレー中(または曲開始前)に、STARTボタンを押しながら、ターンテーブルを回すことで、隠す範囲を調整できます」（【公式OP34】）
- LIFT:「プレー中(またはプレー待ちのとき)にSTARTボタンを押しながらターンテーブルを回すことで、判定ラインを上下に移動させることができます」（【公式OP34】）
- SUD+ と LIFT が両方出ているとき、皿で動くのは SUD+（C.3、【masao】）。LIFT に切り替える操作があるかは [?]
- HIDDEN+ の調整は「START＋皿を対応する向きに回す」（【remy】）。SUD+ と HID+ が両方あるとき、どちらが動くか・両方動くかは [?]
- 皿の向き（右回し／左回しのどちらでカバーが下がるか）は、どの出典にも無い [?]
- 刻み: 白数字は 1 px 刻み（約 1.38）。SUD+ 42〜999、LIFT 0〜830（C.1、【masao】の推定）

### D.5 表示

- START を押している間、レーンカバーの位置を白い数字で、ノーツの表示時間を緑の数字で出す（【公式OP34】）
- HI-SPEED ウィンドウに HS の値を出す。tricoro から実際の倍率で表示する（【公式tri】【公式HS34】）
- プレー開始前は、速度のプレビューで確かめられる（【公式HS34】）。プレビューの見た目は [?]
- 34 の「表示エリア情報常時表示」を ON にすると、白数字・緑数字をレーンカバー（と LIFT）の上に常に出す（【公式BPM34】、bemaniwiki 34）
- START を離した後もしばらく出ているか（残る時間）は [?]

### D.6 緑数字・HS を固定するオプション

- **ノーマル HS（26 Rootage〜）**: 開始 BPM で緑数字が決まる段階式。曲中は倍率そのまま（C.2）。「緑数字固定」という名前のオプションは公式ヘルプに無い
- **FHS 自体**が「カバーを動かしても緑数字を保つ」仕組み。公式は「ノーツ表示時間を固定したままSUDDEN+の長さを変更」と書く
- **34 ZINRAI「譜面表示エリアの固定」**（詳細オプション。【公式BPM34】、bemaniwiki 34）:
  - 「プレー中、レーンカバーやLIFTの位置が完全に固定されます。」
  - 「フローティングハイスピードを使用している場合は、STARTボタンを素早く2回押すことで、レーンカバーの位置を変えることなくノーツ表示時間(緑数字) を初期設定に戻すことができます。」
    - bemaniwiki は「初期設定」を「楽曲開始時(もしくはFHSに切り替えた時点)の値」と書く
  - START 2 回押しでカバーを消す操作はできなくなる（カバーを「外すこともできなくなる」、bemaniwiki）
  - 皿チョンはできない（bemaniwiki）
  - SUD+ の位置は、曲の開始前（GET READY まで）と終了後（最後のノーツの後）だけ動かせる（bemaniwiki）
  - **ON のとき START＋鍵盤で HS を変えられるかは [?]**。どの出典にも書かれていない
- ロケテストで試された「LANE COVER ADJUST」「SPEED ADJUST」（ノーツ表示時間固定）は製品版に入らなかった（bemaniwiki 34）
- 34 第 3 版（2026-09-30）で、FHS でカバーを動かすと緑数字が少しずれる不具合を直した（bemaniwiki 34）

### D.7 クラシック HS（CHS）・tricoro 以前

- 操作: START を押しながら、黒鍵で 1 段上げ、白鍵で 1 段下げる（【remy】:「holding the START button during gameplay and hitting either a black or white key」）
  - 曲中に変えられるようになったのは DistorteD（AC）から。.5 の段は HAPPY SKY（AC）から（【remy】）
  - tricoro 以前は、モード選択・選曲画面でも変えられた（【公式tri】）
  - tricoro 以前の段（HS1〜4、0.5 刻み）の正確な並びと、鍵盤ごとの割り当ては [?]
- 現行の CHS の段: 1.00, 1.50, 2.00, 2.25, 2.50, 2.75, 3.00, 3.25, 3.50, 3.75, 4.00 の 11 段（【masao】、nephilim）
  - tricoro 当時は「x0.50 to x4.00 in 0.25 increments」（【remy】）。0.50・0.75 の段が今もあるかは [?]
- 端で止まるか、折り返すかは [?]
- CHS で皿が HS を変えるかは [?]。皿は SUD+/LIFT を動かす（D.4）

### D.8 INFINITAS・CS

- INFINITAS の緑数字の定数は AC と少し違う（174758 と 174728、【masao】）。操作の違いを書いた出典は見つからなかった [?]
- INFINITAS の画面は SPADA が元（【remy】）。34 の「譜面表示エリアの固定」などが INFINITAS にあるかは [?]

### D.9 beatoraja の対応（仕様のみ。B.4 の補足）

`BRJ/play/ControlInputProcessor.java:62-120`, `play/LaneRenderer.java:237-247`, `PlayConfig.java`

- 鍵の割り当て（7 鍵）: 奇数鍵（白鍵）= −、偶数鍵（黒鍵）= +、皿の 2 方向 = カバー ±。**ハイスピ固定のモードでも割り当ては同じ**。変わるのは刻みだけ
  - 固定 OFF: ±hispeedmargin（既定 0.25）
  - 固定 ON: ±(基準 HS × margin)。B.4 のとおり一時的
- 範囲: 0 < HS < 20 を外れる変更は**無視する**（端の値に寄せない。IIDX は端の値になる）（`LaneRenderer.java:244`）
- START＋皿: カバー（LIFT・HIDDEN）を 50 ms ごとに 0.001、500 ms 以上回し続けると 0.01（B.4）。IIDX と違い、カバーが無いときに HS の微調整にはならない
- SELECT＋鍵盤: duration ±1 ms、SELECT＋皿: duration ±1（50 ms ごと）
- START 2 回押し（500 ms 以内）: カバー表示の切替（B.4）
- START＋SELECT を短く押す: カバーが OFF で LIFT と HIDDEN が両方 ON のとき、皿で動かす対象を LIFT／HIDDEN で切り替える（`:186-193`）。長く押すと終了
- EFFECT に当たる「固定モードの切り替え」はプレー中には無い（設定画面で選ぶ）

### D.10 未確認のまとめ

- FHS の皿の刻み（0.01 か 1/300 か）と、回す速さによる加速
- 鍵盤で HS を変えたとき、固定する緑数字の基準が更新されるか
- LIFT だけのとき・HID+ があるときの皿の挙動と HS の計算
- 34「譜面表示エリアの固定」ON での START＋鍵盤
- START 2 回押しの受付時間
- CHS の端での挙動、tricoro 以前の段
- INFINITAS・CS の操作差
