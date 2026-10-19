# osu!mania（.osu）と bmson の取り込み仕様（調査）

調査日 2026-10-19。挙動だけを書く。コードの表現は写していない。
記号: **[?]** = 出典で確認できていない／推測を含む。**[差]** = 出典どうしで食い違う。

関係する BMjoy 側: 仕様書 2章（対応範囲）・5章（譜面モデルとインポーター）・6章「ハイスピ・LIFT・緑数字」、
`/Users/fdk/BMjoy/src/chart/chart.h`、`/Users/fdk/BMjoy/src/chart/bms_parser.h`。
スクロールの比較は `docs/hispeed_research.md` の C.4 にもある（本書はその続き）。

## 出典の略記

| 略記 | 実体 |
|---|---|
| `WK-fmt` | osu! wiki「.osu (file format)」 https://osu.ppy.sh/wiki/en/Client/File_formats/osu_(file_format) 。行番号は原稿 `ppy/osu-wiki@e0eec9d` の `wiki/Client/File_formats/osu_(file_format)/en.md` |
| `WK-judge` | osu! wiki「osu!mania judgement system」 https://osu.ppy.sh/wiki/en/Gameplay/Judgement/osu!mania （原稿 `wiki/Gameplay/Judgement/osu!mania/en.md`） |
| `WK-mania` | osu! wiki「osu!mania」 https://osu.ppy.sh/wiki/en/Game_mode/osu!mania （原稿 `wiki/Game_mode/osu!mania/en.md`） |
| `WK-opt` | osu! wiki「Options」 https://osu.ppy.sh/wiki/en/Client/Options （原稿 `wiki/Client/Options/en.md`） |
| `WK-sbaudio` | osu! wiki「Storyboard audio samples」 https://osu.ppy.sh/wiki/en/Storyboard/Scripting/Audio |
| `LZ/` | osu!lazer のソース `https://github.com/ppy/osu/blob/676f3b01f28841ebd97ab583e178f2d7b99fe48c/`（2026-10-19 の master） |
| `BMSON` | bmson 仕様 1.0.0-beta https://bmson-spec.readthedocs.io/en/master/doc/index.html （原稿 `bemusic/bmson-spec@668c433` `doc/index.rst`）。節名で引く |
| `BMU/` | bmson 仕様の作者側の参照実装（Bemuse） `https://github.com/bemusic/bmson/blob/129b92871f008fcccfa4cf999bab6dd114eeb437/` |
| `JBP` | beatoraja の bmson 読み込み `https://github.com/exch-bms2/jbms-parser/blob/8108cd54a19275810e1f5d2c9586f008eb6db95e/src/bms/model/BMSONDecoder.java` 。同梱の `/Users/fdk/BMjoy/参考に/beatoraja-master/lib/jbms-parser.jar` を逆アセンブルし、本書で引いた読み込み処理（ヘッダ・タイミング・ノーツ・`c`・LN）が同じであることを確かめた（GitHub 版には入力検査の処理が追加されている） |
| `BRJ/` | `/Users/fdk/BMjoy/参考に/beatoraja-master/src/bms/player/beatoraja/` |
| `BMJ/` | `/Users/fdk/BMjoy/src/` |

「stable」は旧クライアント osu!(stable)、「lazer」は現行の osu!(lazer)。stable のソースは公開されていないので、stable の挙動は wiki と、lazer のソースに書かれた「stable 互換のため」という注記から読む。

---

## Part 1 — osu! .osu ファイル（osu!mania, `Mode: 3`）

### 1.1 ファイル全体

- 1行目が `osu file format vN`（stable の最新は v14、lazer は v128）。`WK-fmt:7`。lazer はこの行から数値を取り出して版とする（`LZ/osu.Game/Beatmaps/Formats/LegacyBeatmapDecoder.cs:55`）。
- 以降は `[General]` などの節。`[General]` `[Editor]` `[Metadata]` `[Difficulty]` は `key: value`、`[Events]` `[TimingPoints]` `[HitObjects]` はカンマ区切り。`WK-fmt:11-20`。
- 空行と、`//` で始まる行は読み飛ばす。行の途中の `//` 以降もコメントとして捨てるが、**`[Metadata]` だけは捨てない**（曲名に `//` が入りうるため）。行末の空白は落とす。`LZ/osu.Game/Beatmaps/Formats/LegacyDecoder.cs:50-58, 81`
- `key: value` は最初の `:` で 2 つに分け、両側の空白を落とす（`LegacyDecoder.cs:155-157`）。
- 文字コードは UTF-8 として読む [?]（lazer は UTF-8 前提。BOM 付きもある）。
- 解釈できない行は、その行だけ捨てて続ける（`LegacyDecoder.cs:67-78`）。

### 1.2 使う項目

#### [General]

| 項目 | 意味 | 既定 | 出典 |
|---|---|---|---|
| `AudioFilename` | 音源ファイル（譜面フォルダからの相対） | — | `WK-fmt:28` |
| `AudioLeadIn` | 音源が鳴り始める前に置く無音の時間（ms） | 0 | `WK-fmt:29`、`LegacyBeatmapDecoder.cs:256-257` |
| `PreviewTime` | 選曲画面のプレビュー開始位置（ms）。-1 は未指定。v5 未満は +24ms する（1.5） | -1 | `WK-fmt:31`、`LegacyBeatmapDecoder.cs:260-262` |
| `Mode` | 0=osu!、1=taiko、2=catch、**3=mania** | 0 | `WK-fmt:35` |
| `SampleSet` | 既定のサンプルセット（Normal/Soft/Drum） | Normal | `WK-fmt:33` |
| `SpecialStyle` | 1 なら mania の「N+1」配置（8K で特別なキーを 1 つ持つ配置） | 0 | `WK-fmt:44` |

- `Mode` が無ければ 0（osu!）。**`Mode: 3` 以外は取り込まない**（仕様書 5章）。

#### [Metadata]

`Title`（ローマ字）、`TitleUnicode`（原語）、`Artist`、`ArtistUnicode`、`Creator`（譜面作者）、`Version`（難易度名）、`Source`、`Tags`（空白区切り）、`BeatmapID`（難易度ごとの ID）、`BeatmapSetID`（曲ごとの ID）。`WK-fmt:62-73`

#### [Difficulty]

| 項目 | mania での意味 | 範囲 | 出典 |
|---|---|---|---|
| `CircleSize` | **キー数** | mania は 1〜18 に丸め込む（他モードは 0〜10） | `LegacyBeatmapDecoder.cs:43, 122-125` |
| `OverallDifficulty` | 判定幅（1.7） | 0〜10 | `LegacyBeatmapDecoder.cs:127` |
| `HPDrainRate` | ライフの増減量（1.8） | 0〜10 | `LegacyBeatmapDecoder.cs:120` |
| `SliderMultiplier` | mania のスクロールには効かない（1.4.2） | 0.4〜3.6 | `LegacyBeatmapDecoder.cs:130` |

- キー数は `CircleSize` を四捨五入した整数（1 未満は 1）。mania 用の譜面ならこの値をそのまま使う（`LZ/osu.Game.Rulesets.Mania/Beatmaps/ManiaBeatmapConverter.cs:75-81`）。
- 11 キー以上（mania 用譜面で 10 を超えるとき）は、半分ずつの 2 段（Dual stage）にする（`ManiaBeatmapConverter.cs:67-73`、1段の上限 10 は `LZ/osu.Game.Rulesets.Mania/ManiaRuleset.cs:54`）。BMjoy では対象外。

#### [Events]

書式は `種類,開始時刻,引数…`。種類は名前でも番号でもよい。`WK-fmt:88-92`。番号は 0=Background、1=Video、2=Break、3=Colour、4=Sprite、5=Sample、6=Animation（`LZ/osu.Game/Beatmaps/Legacy/LegacyEventType.cs`）。

| 行 | 書式 | 意味 | 出典 |
|---|---|---|---|
| 背景画像 | `0,0,"ファイル",xOffset,yOffset` | 背景の静止画。引用符は無くてもよい。オフセットは osu!px で画面中央から | `WK-fmt:94-99`、`LegacyBeatmapDecoder.cs:470-472` |
| 動画 | `Video,開始時刻,"ファイル",x,y`（`1` でも可） | 背景動画を指定時刻（ms）から流す。**拡張子が動画でなければ背景画像として扱う**（古い譜面の誤記対策） | `WK-fmt:101-107`、`LegacyBeatmapDecoder.cs:457-468` |
| 休憩 | `2,開始,終了` | 休憩区間（mania では演出だけ） | `WK-fmt:109-115` |
| 効果音 | `Sample,時刻,レイヤ,"ファイル",音量` | 指定時刻に音を鳴らす（1.3.3）。音量 1〜100、省略時 100 | `WK-sbaudio`、`LZ/osu.Game/Beatmaps/Formats/LegacyStoryboardDecoder.cs:186-194` |

- 背景画像の行が無く、ストーリーボードのスプライトだけがある古い譜面では、最初のスプライトの画像を背景とする（`LegacyBeatmapDecoder.cs:446-455`）。
- ストーリーボードは `.osu` の `[Events]` と、同じフォルダの `.osb`（全難易度で共有）の両方に書ける。`WK-fmt:117-125`

#### [TimingPoints]

書式: `time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects`（`WK-fmt:131-142`）。

| 欄 | 意味 |
|---|---|
| `time` | 区間の開始（ms、小数可）。次のタイミングポイントまでが 1 区間 |
| `beatLength` | 非継承点（uninherited=1）: 1拍の長さ（ms）。継承点（uninherited=0）: 負の値で、SV 倍率 = 100 / (−beatLength) |
| `meter` | 1小節の拍数（継承点では無視）。`0` で始まる値は 4 とみなす（`LegacyBeatmapDecoder.cs:496-498`） |
| `sampleSet` | 0=譜面の既定、1=normal、2=soft、3=drum |
| `sampleIndex` | カスタムサンプルの番号。0 は osu! 標準のヒット音 |
| `volume` | ヒット音の音量（%） |
| `uninherited` | 1=非継承（BPM を決める）、0=継承（SV 等だけ） |
| `effects` | ビット0=kiai、ビット3=最初の小節線を出さない（taiko・mania） |

lazer の読み方（`LegacyBeatmapDecoder.cs:484-603`）:

- 欄が足りないときの既定: meter=4、sampleSet=[General] の値、sampleIndex=0、volume=100、**uninherited=1**、effects=0（`:496-524`）。
- 倍率 = beatLength が負なら 100/(−beatLength)、そうでなければ 1（`:494`）。**mania と taiko ではこの倍率をスクロール速度の倍率（EffectControlPoint の ScrollSpeed）にする**（`:557-559`）。
- 非継承点は「BPM」と「倍率 1」を同時に置く。**同じ時刻に継承点もあれば、継承点の倍率が勝つ**（同時刻の点はまとめてから、継承点由来のものを優先して登録する。`:576-603`）。
- 値の範囲: 拍の長さは 6〜60000 ms（BPM 1〜10000）に丸め込まれる（`LZ/osu.Game/Beatmaps/ControlPoints/TimingControlPoint.cs:65-69`）。スクロール速度の倍率は **0.01〜10**（`LZ/osu.Game/Beatmaps/ControlPoints/EffectControlPoint.cs:22-26`）。参考までに、osu! 標準モードのスライダー速度は 0.1〜10（`DifficultyControlPoint.cs:24-28`）。依頼にあった「0.1〜10」はこちらの値で、mania のスクロール速度の倍率は 0.01〜10。
- 非継承点の beatLength が NaN なら、その行は捨てる（`:532-533`）。
- タイミングポイントは時刻順に並んでいるはず（`WK-fmt:129`）。

#### [HitObjects]

書式: `x,y,time,type,hitSound,objectParams,hitSample`（`WK-fmt:183-190`）。

- `x` → 列。**列 = floor(x × キー数 / 512)** を 0〜キー数−1 に丸め込む（`WK-fmt:306`）。lazer は「512/キー数」で割って切り捨てる（`LZ/osu.Game.Rulesets.Mania/Beatmaps/Patterns/Legacy/LegacyPatternGenerator.cs:43-53`、mania 用譜面は `PassThroughPatternGenerator.cs:29` から特別扱いなしで呼ぶ）。x は 0〜512 に丸め込み、stable の版（v14 以下）では整数に切り捨ててから使う（`LZ/osu.Game/Rulesets/Objects/Legacy/ConvertHitObjectParser.cs:57-62`）。x が整数なら、どちらの式でも同じ列になる。
- `y` は mania では使わない（`WK-fmt:363`）。
- `time` は ms（stable は整数、lazer の版は小数もある）。
- `type` のビット: 0=丸（mania では通常ノーツ）、1=スライダー、2=新コンボ、3=スピナー、4〜6=コンボ色スキップ、**7=mania のホールド**（`WK-fmt:196-203`）。lazer は **ビット0 → 1 → 3 → 7 の順に調べて最初に当たったもの**にする（`ConvertHitObjectParser.cs:79-181`）。つまりビット0と7が両方立っていれば通常ノーツになる。どれにも当たらなければその行を捨てる（`:183-184`）。
- ホールド: `x,y,time,type,hitSound,endTime:hitSample`。objectParams の先頭が終端時刻（ms）で、その後ろに `:` 区切りで hitSample が続く（`WK-fmt:301-307`）。終端が始点より前なら始点に揃える（長さ 0 のホールドになる）（`ConvertHitObjectParser.cs:167-181`）。
- mania 用譜面にスライダーやスピナーがあると、lazer はホールドとして扱う（スライダーだと押している間の滑り音を鳴らす）。正規の譜面にはまず無い（`WK-fmt:365-366`、`PassThroughPatternGenerator.cs` の HoldNote 生成部）[?: スピナーの扱いは converter の SpinnerPatternGenerator を通る。`ManiaBeatmapConverter.cs:205-211`]。
- 同時刻のノーツは、ファイル内の順番を保ったまま時刻で並べ直す（手で編集された譜面に順序の乱れがある。`LegacyBeatmapDecoder.cs:99-103`）。

### 1.3 音（ヒット音・キー音・BGM）

#### 1.3.1 ヒット音の指定

- `hitSound` のビット: 0=normal、1=whistle、2=finish、3=clap。どれも立っていなければ normal（`WK-fmt:205-216`）。
- `hitSample` = `normalSet:additionSet:index:volume:filename`。省略時は `0:0:0:0:`（`WK-fmt:220-230`）。
  - `normalSet` / `additionSet`: 0=指定なし（normal 音はタイミングポイントのセット、追加音は normal 音のセットに従う）、1=normal、2=soft、3=drum（`WK-fmt:232-239`）
  - `index`: 0 ならタイミングポイントの sampleIndex を使う
  - `volume`: 0 ならタイミングポイントの音量を使う
  - **`filename`: 鳴らすファイルを直接指定する（譜面フォルダ内）**
- ファイル名の組み立て（filename が無いとき）: `<セット>-hit<音>[<index>].wav`。セットは normal/soft/drum、音は normal/whistle/finish/clap。index は 0 と 1 のときは付けない。探す場所は「譜面フォルダ（index が 0 でないとき）→ スキン → osu! 標準」の順（`WK-fmt:241-251`）。
- **filename があるとき**:
  - wiki: 追加音（whistle/finish/clap）は鳴らさず、そのファイルだけを鳴らす（`WK-fmt:253`）
  - lazer: filename は normal 音の代わりになり、**追加音のビットが立っていれば追加音も重ねて鳴らす**（`ConvertHitObjectParser.cs:543-569`）。**[差]**
  - lazer はファイルを「書かれた名前のまま → 拡張子を外した名前」の順で探す（`ConvertHitObjectParser.cs:685-700`。拡張子を外した名前から何を探すかはサンプルの読み込み側に任される [?]）
- 音量の優先順位: hitSample の volume（0 以外）→ タイミングポイントの volume。lazer はホールド以外のノーツでは「ノーツの終了時刻 + 5ms」の時点のタイミングポイントを使う（`LegacyBeatmapDecoder.cs:38, 184-189`）。

#### 1.3.2 mania で音が鳴る時（キー音のモデル）

- **キーを押した瞬間に、その列で次にまだ判定されていないノーツの音を鳴らす**。ノーツに当たったかどうかとは関係なく、空押しでも次のノーツの音が鳴る（`LZ/osu.Game.Rulesets.Mania/UI/Column.cs:186-192` → `LZ/osu.Game/Rulesets/UI/GameplaySampleTriggerSource.cs:63-75, 104-120`）。列にもう未判定のノーツが無ければ最後のノーツの音を鳴らす（`:115-118`）。
  - これは BMS の「押したキーの音＝そのレーンの次のノーツの音」と同じ考え方。BMjoy の判定部もそうなっている（`BMJ/game/judge.cpp:33-35`）。
- ホールドは**始点の音だけ**を鳴らす。終端には音が無い（mania の既定。`LZ/osu.Game.Rulesets.Mania/Objects/HoldNote.cs:126-138`）。
- 通常の mania 譜面は「音源 1 本 + 標準ヒット音」。BMS から変換した「キー音譜面」は、各ノーツの `filename` にキー音を、伴奏を `Sample` 行（1.3.3）に書く [?: 慣習。仕様として書かれたものは見つけていない]。

#### 1.3.3 BGM

- `AudioFilename` の音源は、譜面の時刻 0 で鳴り始める（全オブジェクトの時刻は「音源の先頭からの ms」。`WK-fmt:91, 133, 186`）。
- `AudioLeadIn` は、音源が鳴り始める前に置く待ち時間（ms）。譜面の時刻はずれない（`WK-fmt:29`）。
- ストーリーボードの `Sample` 行は、指定時刻に鳴らす効果音。レイヤは 0=Background、1=Fail、2=Pass、3=Foreground。**Fail / Pass レイヤの音は、その時点でプレイヤーが失敗中／合格中のときだけ鳴る**。重なり順の意味は無く、全部混ぜて鳴らす（`WK-sbaudio`、レイヤ番号 `LZ/osu.Game/Beatmaps/Legacy/LegacyStoryLayer.cs`）。
- lazer は `Sample` 行の時刻に v5 未満の +24ms を足していない（`LegacyStoryboardDecoder.cs:186-194` に補正が無い）。stable も同じかは [?]。

### 1.4 時刻とスクロール

#### 1.4.1 時刻の補正

- **ファイル版が v5 未満（v4 以下）なら、全タイミングに +24ms を足す**（「昔のクライアント側のずれ直し」）。対象はヒットオブジェクトの時刻とホールドの終端、タイミングポイント、休憩、PreviewTime（−1 以外）。`LegacyBeatmapDecoder.cs:27-30, 75, 89, 262, 475-476, 488, 613-617`、`ConvertHitObjectParser.cs:64, 160, 180`。仕様書 5章の記述と一致する。
- 版の行が無いファイルは、最新の版として読む（`LegacyBeatmapDecoder.cs:56, 66`）。

#### 1.4.2 lazer のスクロール位置の計算

1. タイミングポイントを集め、時刻ごとに「その時点で有効な BPM の点」と「その時点で有効な SV の点」の組を作る（`LZ/osu.Game/Rulesets/UI/Scrolling/DrawableScrollingRuleset.cs:120-149`）。
2. **最後のノーツより後の点は捨てる**。同じ時刻の点は最後のもの 1 つにまとめる（`:151-156`）。点が 1 つも無ければ倍率 1 の点を 1 つ置く（`:160-161`）。
3. 各区間の速さの倍率 = **SV × 基準の拍の長さ ÷ その区間の拍の長さ**（`LZ/osu.Game/Rulesets/Timing/MultiplierControlPoint.cs:24`）。mania では SliderMultiplier の影響を打ち消している（`DrawableScrollingRuleset.cs:111-118` で割り、`LZ/osu.Game.Rulesets.Mania/UI/DrawableManiaRuleset.cs:90-94` で掛け戻す）。
4. **基準の拍の長さ ＝ 譜面で一番長く使われている拍の長さ**（mania は `RelativeScaleBeatLengths = true`。`DrawableManiaRuleset.cs:53`、`DrawableScrollingRuleset.cs:111-114`）。求め方（`LZ/osu.Game/Beatmaps/Beatmap.cs:80-120`）:
   - 非継承点ごとに「次の非継承点まで」の長さを足し上げる。最後の点は最後のノーツの時刻まで
   - **最初の点は時刻 0 から始まったものとして数える**（stable 互換、と注記がある）
   - 最後のノーツより後の点は長さ 0
   - 拍の長さは 1/1000 ms に丸めてから同じものをまとめる
   - 一番長いものを選ぶ。全部 0 なら 1000ms（BPM 60）。結果は譜面中の最小〜最大の拍の長さに丸め込む
5. 方式（ScrollVisualisationMethod）。**mania の既定は Sequential**（`DrawableScrollingRuleset.cs:172`。mania 側で変えていない）:
   - **Sequential**: 位置 ＝ 時刻を倍率で積分したもの。区間ごとに「区間の長さ × 倍率」を足して、区間の始まりの位置の表を作り、その間は線形にたどる。最初の点より前は最初の点の倍率で延ばす（`LZ/osu.Game/Rulesets/UI/Scrolling/Algorithms/SequentialScrollAlgorithm.cs:65-74, 92-96, 111-131`）。BMS の「時刻→位置」の区分線形と同じ形。
   - **Overlapping**: ノーツごとに、そのノーツの時刻の倍率だけで（今の時刻との差 × 倍率）位置を決める。速さの違うノーツ同士は追い越しうる（`OverlappingScrollAlgorithm.cs:36-37`）。taiko 型。mania では使っていない
   - **Constant**: 倍率を全部無視して、時刻の差だけで位置を決める（`ConstantScrollAlgorithm.cs:21-22`）。「Constant Speed」mod でこれになる（`LZ/osu.Game.Rulesets.Mania/Mods/ManiaModConstantSpeed.cs:30`）。停止という概念が無いので「止まる」こともない
6. 画面上の位置 ＝（ノーツの位置 − 今の位置）÷ TimeRange × レーンの長さ（Sequential）。

まとめ: **lazer の mania では、BPM の変化は「最頻 BPM に対する比」で、SV はそのまま倍率で、両方を掛けたものが速さになる**。BMS の BPM 変化 + #SCROLL と同じ構造で、STOP だけが無い。

#### 1.4.3 「スクロール速度」（設定値）の意味

- lazer の設定値は 1〜40（0.1 刻み、既定 8）（`LZ/osu.Game.Rulesets.Mania/Configuration/ManiaRulesetConfigManager.cs:24`）。
- **表示時間 TimeRange = 11485 ms ÷ 設定値**（`DrawableManiaRuleset.cs:38-43, 183`）。これは「倍率 1（最頻 BPM・SV 1）のノーツが、レーンの端から判定位置に届くまでの時間」。スキンの判定位置に合わせてこの時間を伸縮し、レート系 mod（DT/HT）のときは曲の速さを掛けて実時間を保つ（`:167-176`）。
- BMjoy の緑数字（1/600 秒）に直すと **緑数字 ≒ TimeRange[ms] × 0.6**（レーンを全部見せたとき）。設定値 8 → 1436ms → 緑数字 約 861。

#### 1.4.4 stable の扱い

- 設定「Scale osu!mania scroll speed with BPM」:
  - ON（BPM scaling）: 譜面の BPM に応じて速さが変わる。BPM 100 の譜面は遅く、200 の譜面は速い（`WK-mania:49`）
  - OFF（Fixed）: 譜面によらず同じ速さで流れるように基準をそろえる（`WK-mania:53`）
  - **どちらでも、曲の途中の BPM 変化では速さが変わる**（`WK-mania:55`）
  - 既定値: `WK-opt:188` は「Disabled（OFF）」、`WK-mania:49` は BPM scaling を「current default」と書いている。**[差]**
- Fixed のときの基準は、lazer の「最頻の拍の長さ（最初の点は 0 から数える）」が stable 互換と注記されている（`Beatmap.cs:98-99`）ので、stable も最頻 BPM を基準にしていると読める [?: stable の式は直接は確認できない]。
- ランク基準でも「SV を譜面で一番使われている BPM に合わせて正規化する」ことになっている（`docs/hispeed_research.md` C.4 の引用）。

### 1.5 小節線

lazer の mania の小節線（`LZ/osu.Game/Rulesets/Objects/BarLineGenerator.cs`）:

- 非継承点ごとに、その点から「拍の長さ × meter」おきに引く。次の非継承点で打ち切る。最後の点は「最後のノーツ + 1ms + 1小節」まで。
- 引き始めは「0 と最初のノーツの早いほう」より前にはしない（その時刻に揃うように点からの小節数を進める）（`:46`）。
- effects のビット3 が立った点は、最初の 1 本を飛ばす（`:68-71`）。
- 小節の頭（meter 拍ごと）を太線にする。

### 1.6 ホールドの判定（参考）

- stable: ホールドは**始点と終点を合わせて 1 つの判定**。条件は「始点のずれ ≤ 窓 × 係数」かつ「始点のずれ + 離すずれ ≤ 窓 × 係数」（PERFECT 1.2/2.4、GREAT 1.1/2.2、GOOD・OK 1/2）。途中で離すと MEH 以下（`WK-judge:41-58`）。
- lazer（既定）: 始点と終点を別々に判定する。終点は**判定窓を 1.5 倍に広げる**（離すずれを 1.5 で割って判定する）（`LZ/osu.Game.Rulesets.Mania/Objects/TailNote.cs:19`、`Objects/Drawables/DrawableHoldNoteTail.cs:59-60`）。ScoreV2 の stable も同じ考え方（`WK-judge:62-68`）。
- 仕様書 5章では、取り込み時の LN モードの既定を CN にすることになっている（終端判定があるため）。

### 1.7 判定窓（OD から）

判定の表（片側の幅、ms）。「OD」は OverallDifficulty。

| 判定 | stable（mania 用譜面） | lazer 既定 | lazer の値 OD0 / OD5 / OD10 |
|---|---|---|---|
| PERFECT（300g, MAX） | **16（固定）** | OD≤5: 22.4 − 0.6·OD、OD≥5: 24.9 − 1.1·OD | 22.4 / 19.4 / 13.9 |
| GREAT（300） | 64 − 3·OD | 64 − 3·OD | 64 / 49 / 34 |
| GOOD（200） | 97 − 3·OD | 97 − 3·OD | 97 / 82 / 67 |
| OK（100） | 127 − 3·OD | 127 − 3·OD | 127 / 112 / 97 |
| MEH（50） | 151 − 3·OD | 151 − 3·OD | 151 / 136 / 121 |
| MISS | 188 − 3·OD | 188 − 3·OD | 188 / 173 / 158 |

- 出典: stable `WK-judge:9-20`。lazer `LZ/osu.Game.Rulesets.Mania/Scoring/ManiaHitWindows.cs:12-17, 152-160`、補間の式 `LZ/osu.Game/Beatmaps/IBeatmapDifficultyInfo.cs:57-65`（OD 5 を境にした 2 本の直線）。
- lazer の Classic mod では stable と同じ値になる（PERFECT 16、他は 34 + 3·(10−OD) など。`ManiaHitWindows.cs:140-150`、mod の適用は `ManiaRuleset.cs:492-495`）。
- 端数: lazer は「切り捨てた整数 + 0.5」を窓にする（`ManiaHitWindows.cs:133-159`）。stable も「ずれは四捨五入、窓は切り捨て」で、最大 0.5ms の差が出る（`WK-judge:20`）。
- ずれが MISS の窓より外なら、押しても何も起きない（BMS の空 POOR に当たるものは無い）。stable は OK の窓を過ぎると見逃し MISS（遅い MEH は出ない）（`WK-judge:39`）。lazer は MEH の窓を過ぎると MISS（`LZ/osu.Game/Rulesets/Scoring/HitWindows.cs:109`、`LZ/osu.Game.Rulesets.Mania/Objects/Drawables/DrawableNote.cs:85-100`）。**[差]**
- osu! 標準モードからの変換譜面（BMjoy の対象外）は別の固定値になる（`WK-judge:22-31`）。
- DT/HT などのレート系 mod では、mania の判定窓は実時間で変わらない（`WK-judge:33`、`ManiaHitWindows.cs:21-37`）。

### 1.8 ライフ（HP）

- mania では時間経過によるライフの減少は無い。ノーツの判定だけで増減する（`WK-mania:65`、lazer は減少率を 0 にしている `LZ/osu.Game.Rulesets.Mania/Scoring/ManiaHealthProcessor.cs:18-23`）。
- lazer の増減量（HP = HPDrainRate。最大を 1 とした量）（`ManiaHealthProcessor.cs:30-63`）:
  - MISS: −(HP+1)×0.0075。ホールドの始点・終点の MISS はその半分
  - MEH: −(HP+1)×0.0016
  - OK: 0
  - GOOD: (0.004 − HP×0.0004) × 補正
  - GREAT: (0.005 − HP×0.0005) × 補正
  - PERFECT: (0.0055 − HP×0.0005) × 補正
  - 補正（HpMultiplierNormal）は stable 互換の計算から出る値（`LegacyDrainingHealthProcessor`、未読）[?]
- BMS の TOTAL に当たるものは無い。仕様書 5章では BMS の TOTAL 未定義時と同じ既定式で補う。

---

## Part 2 — bmson

### 2.1 版

- `version` は文字列（現行 "1.0.0"。比較は Semantic Versioning）。**`version` が無いものは旧形式（0.21）**で、仕様は「拒否する」か「0.21 として読む」のどちらかを求める。`version` が null ならエラーを出す。`BMSON`「Top Level Object」
- 1.0.0 での 0.21 からの変更（`BMSON`「Changelog」）:
  - camelCase をすべて snake_case に: `soundChannel`→`sound_channels`、`judgeRank`→`judge_rank`、`initBPM`→`init_bpm`、`bgaHeader`→`bga_header`、`bgaNotes`→`bga_events`、`layerNotes`→`layer_events`、`poorNotes`→`poor_events`、`ID`→`id`
  - `bpmNotes`→`bpm_events`（値 `v`→`bpm`）、`stopNotes`→`stop_events`（値 `v`→`duration`）
  - `BarLine.k` を削除
  - 時間の単位を pulse に統一
  - `total` を相対値に変更
  - 追加: `version`、`subtitle`、`subartists`、`mode_hint`、`chart_name`、`back_image`、`eyecatch_image`、`banner_image`、`preview_music`、`resolution`
- 0.21 の読み方（Bemuse の実装）: 1拍 = 240（`y / 240`）固定。`initBPM`、`bpmNotes{y, v}`、`stopNotes{y, v}`（v は pulse として 240 で割って拍にする）、`soundChannel`（`BMU/src/legacy.js:10-30`、`BMU/src/index.js:103-105, 148-152`）。0.21 の `total` が絶対値だったのかは [?]（Changelog の「相対値に変更」からの推測）。
- beatoraja は `version` を見ずに 1.0 の名前で読む（`JBP` の型定義 `bms/model/bmson/Bmson.java`）。つまり 0.21 のファイルはほぼ空の譜面になる [?: 実ファイルで未確認]。

### 2.2 情報（info）

| 項目 | 意味 | 既定 | 出典 |
|---|---|---|---|
| `title` | 曲名。区切り記号で分けなくてよい | — | `BMSON`「title」 |
| `subtitle` | 副題。`\n` で複数行 | "" | 同 |
| `artist` | 主な作者 | — | 同 |
| `subartists` | `"key:value"` の配列。key は music/vocal/chart/image/movie/other（省略時 other）。前後の空白は落とす | [] | 同 |
| `genre` | ジャンル | — | 同 |
| `mode_hint` | 配置。`beat-7k` など（2.4） | "beat-7k" | 同 |
| `chart_name` | 難易度名（HYPER など） | "" | 同 |
| `level` | レベル（0 以上の整数） | — | 同 |
| `init_bpm` | 開始 BPM。**無いと致命的エラー** | — | 同 |
| `judge_rank` | 判定幅の相対値（2.7） | 100 | 同 |
| `total` | ゲージ増加の相対値（2.8） | 100 | 同 |
| `back_image` | プレイ中の背景静止画 | — | 同 |
| `eyecatch_image` | 読み込み中の画像 | — | 同 |
| `title_image` | 開始前に出す画像（OADX+ の #BACKBMP 相当）。**仕様の説明にはあるが、冒頭の型定義と beatoraja には無い** | — | 同 |
| `banner_image` | バナー（15:4、通常 600×160） | — | 同 |
| `preview_music` | プレビュー用の音。無ければ sound_channels から作ってよい | — | 同 |
| `resolution` | 1拍（4分音符）の pulse 数。0・null・未定義なら既定値、負なら絶対値 | 240 | 同 |

beatoraja の読み替え（`JBP`）: 副題は `subtitle` に ` [chart_name]` を付けたもの（`:74-78`）、`subartists` はカンマで連結（`:81-85`）、`eyecatch_image` → STAGEFILE、`back_image` → BACKBMP、`banner_image` → BANNER（`:131-133`）。`resolution` が 0 以下なら 240（`:139`。負の値の絶対値は取らない）。

beatoraja の独自拡張（仕様に無い。`JBP` の型定義 `bms/model/bmson/*.java`）:

- `info.ln_type`（1=LN、2=CN、3=HCN。譜面全体の LN の種類）（`:114-116`）
- `scroll_events`（`{y, rate}`。BMS の #SCROLL 相当）
- `mine_channels`（`{name, notes[{x, y, damage}]}`。地雷）
- `key_channels`（`{name, notes[{x, y}]}`。見えないノーツ＝空押しで鳴る音）
- ノーツの `t`（そのノーツの LN の種類 1〜3）と `up`（LN 終端の音の指定）
- `bga.bga_sequence`、BGA イベントの `id_set` `condition` `interval`（アニメーションと条件付きレイヤ）

### 2.3 時間

- 単位は 3 つ: 秒（t）、拍（b。BPM と STOP で長さが変わる）、pulse（y）。1拍 = `resolution` pulse。`BMSON`「Time Units」「Beat Resolution」
- 拍子・小節の概念は無く、小節線の位置だけを持つ（2.6）。
- `bpm_events`: `{y, bpm}`。開始は `init_bpm`。**同じ y に複数あれば最後のもの**（`BMSON`「Timing」）。
- `stop_events`: `{y, duration}`。duration は **pulse**。その間スクロールと曲が止まる。**同じ y に複数あれば足し合わせる**（`BMSON`「Timing」）。
- 同じ y の処理順: ノーツ・BGA → BPM → STOP。STOP の秒数は、同じ y の BPM 変化を反映したあとの BPM で計算する（`BMSON`「Order of Processing」）。
- **STOP と同じ y のノーツは、停止の始まりの時刻に判定する**（停止明けではない）。BGM ノーツなら、音を鳴らしてから止まる（同）。BMS と同じ。
- 秒への換算: Δt = 60 × Δy ÷ (resolution × BPM)。STOP は 60 × duration ÷ (resolution × その時点の BPM)。

beatoraja の挙動（`JBP`）:

- BPM・STOP・SCROLL を y の順にまとめて処理し、同じ y なら SCROLL → BPM → STOP の順（`:150-176`）。
- BPM が 0 以下のイベントは捨てる（警告）。STOP の duration が負なら捨てる（警告）（`:157-174`）。
- **同じ y の STOP は足し合わせず、最後のもので上書きする**（`TimeLine` の停止時間の設定が代入。jar を逆アセンブルして確認）。仕様と **[差]**。
- 時刻の計算は「直前のイベントの時刻 + 直前の停止時間 + Δy から求めた時間」（`:588-606`）。

### 2.4 レーン（x）と mode_hint

- `x` = 0 または null → BGM ノーツ（演奏しない）。1, 2, 3… → 演奏するノーツ（`BMSON`「notes :: Note[]」）。
- **`beat-7k`: x = 1〜7 が鍵盤、8 が皿**（`BMSON`「Dimensions」、付録の表）。
- 付録の表（`BMSON`「Canonical List of Mode Hints」）: `beat-5k` は 1〜5 が鍵盤で 8 が皿（6, 7 は空き）。`beat-10k` / `beat-14k` は 2P 側を 11〜18 に置く（`beat-14k` は 11〜17 が鍵盤、18 が皿）。`popn-5k` は 1〜5、`popn-9k` は 1〜9。左右対称の汎用キーボードは `generic-nkeys`（左から順）。
- 独自の配置を作るときは `mode_hint` を変え、`version` は変えない（同 「mode_hint」）。
- beatoraja: `mode_hint` が知らない値なら警告を出して `beat-7k` として読む（`JBP:107-113`）。範囲外の x は BGM ノーツ扱い（`:209-212`）。5K と 10K は x の 6・7（と 16・17）を BGM に落とす（`:117-125`）。
- Bemuse: x = 1〜7 を 1〜7 鍵、8 を皿にし、それ以外はレーン無し（`BMU/src/index.js:160-172`）。

### 2.5 ノーツと音（sound_channels）

#### 2.5.1 基本

- `sound_channels` は音ファイルごとの配列。各要素は `name`（ファイル名）と `notes`（そのファイルを使うノーツ）。`BMSON`「Sound Channels」
- ノーツ `{x, y, l, c}`:
  - `l` = 0 → 通常ノーツ。`l` > 0 → **LN（y から y + l まで）**
  - `c` = 継続フラグ（2.5.2）
- ファイル名（`BMSON`「name :: DOMString」）:
  - 拡張子は省略できる。省略されていたら、使える形式（wav, ogg, m4a…）を探す
  - 拡張子付きで見つからない・再生できないときも、拡張子を外して探し直す
  - サブフォルダ可。区切りは `\` と `/` の両方を受け付けて正規化する
  - **絶対パス、親フォルダへの参照（`..`）、ヌル文字は拒否しなければならない**
  - 推奨形式は WAV と、OGG Vorbis か MP4 AAC。MP3 は先頭に無音が入るので非推奨
- 別々の sound channel が同じファイルを指してもよい。その場合は別の音として同時に鳴らせる（BMS で同じファイルを複数の #WAV に割り当てたのと同じ）（`BMSON`「Playback Behavior」）。

#### 2.5.2 `c`（継続）と音の切り出し

**考え方**: 1 本の長い音（ステム）を、ノーツの位置で切り分けて使う。`c = false` のノーツでは音を頭から鳴らし直し、`c = true` のノーツでは「頭から鳴らし直した位置から、今までに経過した時間」の所から続きを鳴らす。

仕様の手順（`BMSON`「Slicing Algorithm (Roughly)」）:

1. その sound channel の全ノーツの y を集め、重複を除く
2. y を秒に直す（BPM と STOP を反映した時刻）
3. `c` の無いノーツの所で音を頭に戻す
4. 2 の時刻を切れ目として音を切り分ける
5. 各ノーツには、そのノーツの時刻から始まる切れ端を割り当てる（同じ y のノーツは同じ切れ端）

例（BPM 120、`BMSON`「Slicing Example」）: y=240（c なし）→ 音の 0〜0.25 秒、y=360（c あり）→ 0.25〜1.0 秒、y=720（c なし）→ 0〜0.25 秒、y=840（c あり）→ 0.25〜1.0 秒、y=1200（c あり）→ 1.0〜2.0 秒、y=1680（c あり、BGM）→ 2.0 秒〜ファイルの終わり。

- 切れ端の開始位置 ＝ 直近の「頭に戻した y」からの経過時間（秒）
- 切れ端の終わり ＝ 次の切れ目まで。最後の切れ端はファイルの終わりまで

境界の扱い（`BMSON`「Edge Cases」「Playback Behavior」）:

- 同じ切れ端が演奏ノーツと BGM ノーツの両方に割り当てられたら、**BGM ノーツは捨てる**
- 同じ y のノーツで `c` が混ざっていたら、`c` は無いものとみなす（頭に戻す）
- 1 つの切れ端の同時発音数は 1。同じ切れ端を持つノーツ（同じ y）を同時に叩いても、音が大きくなってはいけない
- 同じ channel の別の切れ端どうしは同時に鳴らしてよい
- 各切れ端は BMS の #WAV 1 つと同じように扱ってよい
- （参考・任意）BGM にしか使われない連続した切れ端は、次が `c = true` ならつなげてよい

**[差] 次が「頭に戻す」ノーツのときの切れ端の終わり**:

- 仕様の例は、切れ端を次の切れ目で切っている（y=360 の切れ端は y=720 の所で終わる）
- Bemuse（仕様の作者側の実装）は、次の切れ目が「頭に戻す」ノーツなら終わりを決めず、**ファイルの終わりまで鳴らす**（`BMU/src/utils.js:43-61`）
- beatoraja も、次のノーツが `c = true` のときだけ長さを決め、そうでなければ長さ 0（＝最後まで鳴らす）にする（`JBP:200-207`）

beatoraja の細部（`JBP:186-314`）:

- channel 内のノーツを y で並べ、各ノーツについて「y がこれより大きい最初のノーツ」を次のノーツとする
- 開始位置は channel ごとに持つ累計で、`c = false` のノーツで 0 に戻し、各ノーツの処理後に「このノーツの長さ」を足す
- 長さ ＝ 次のノーツが `c = true` なら（次の時刻 − この時刻）、そうでなければ 0
- 同じ y に複数のノーツがあると、それぞれの処理後に長さを足すので、開始位置が二重に進む（コードからの読み取り）[?: 実際の譜面で確かめていない]。仕様の「同じ y は同じ切れ端」とは合わない
- `c` の判定はノーツごと（仕様の「混ざっていたら無し」は実装していない）
- 時刻の差には、間にある STOP の時間も入る [?: STOP 中に音源がどう進むのが正しいかは、仕様に書かれていない]

#### 2.5.3 LN と重なり

- `l > 0` が LN（仕様）。LN の終端に音は無い（仕様に終端の音の定めは無い）。
- **重なったノーツ**（`BMSON`「Layered Notes」）: 別々の sound channel のノーツが同じ (x, y) にあれば、1 つのノーツにまとめ、叩いたら全部の切れ端を鳴らす。長さ（l）が違うのはエラー（警告してよい）。
- beatoraja（`JBP:213-310`）:
  - LN の中（始点より後〜終点まで）に同じレーンのノーツがあれば、そのノーツを BGM に落とす（警告）
  - 同じ位置の LN は、終点も同じなら重ねて 1 本にする。違えば後のものを捨てる（警告）
  - 同じ位置の通常ノーツは重ねる
  - `up = true` のノーツは LN 終端の音として、終端が同じ位置の LN に付ける（後から来た LN にも付くように控えておく）
  - 各 LN の種類は、ノーツの `t`（1〜3）があればそれ、無ければ `info.ln_type`、それも無ければ設定値

#### 2.5.4 地雷と見えないノーツ（beatoraja 拡張）

- `mine_channels`: `{x, y, damage}`。そのレーンの地雷。LN の中や、既にノーツのある位置なら捨てる（警告）。channel の `name` が地雷の音（`JBP:333-372`）。
- `key_channels`: `{x, y}`。見えないノーツ（そのレーンを押したときに鳴る音だけを置く）（`JBP:317-332`）。

### 2.6 小節線（lines）

- `lines` は小節線の y の配列。y=0 の線は省略してよい（出すかはプレイヤー次第）（`BMSON`「lines :: BarLine[]」）。
- **空の配列なら小節線なし**。**null・未定義なら 4/4 とみなし、4 拍（resolution 240 なら 960 pulse）ごとに引く**。
- beatoraja は `lines` があればその位置に引き、無ければ何もしない（`JBP:177-182`）[?: 既定の 4 拍ごとの線を別の所で補うかは未確認]。

### 2.7 judge_rank

- 仕様（`BMSON`「judge_rank」）: **プレイヤーの標準の判定幅に対する相対値（%）**。100 が標準、大きいほど広い。具体的な扱いはプレイヤー次第。参考（規範ではない）として「判定幅をこの割合で伸縮する」解釈を挙げ、LR2 の **#RANK 2（NORMAL）** を標準の例にしている（PGREAT ≤18ms、GREAT ≤40ms、GOOD ≤100ms、BAD ≤200ms）。
- beatoraja（`JBP:87-96`、`BRJ/play/BMSPlayerRule.java:57-66`、`BRJ/play/JudgeProperty.java:209-235`）:
  - 負 → 警告して既定値のまま
  - **1〜4 → BMS の #RANK として扱う**（仕様どおりでないかもしれない、と警告）
  - 5 以上 → bmson の値として扱う
  - 0 → 100 とみなす（`BMSPlayerRule.java:64`）
  - 判定幅 ＝ 基準の幅 × 値 / 100。beatoraja の #RANK の倍率は VERY HARD 25、HARD 50、**NORMAL 75**、EASY 100、VERY EASY 125（%）。つまり **beatoraja では bmson の judge_rank 100 は #RANK 3（EASY）と同じ幅**になる。#DEFEXRANK 100 は NORMAL（75%）に読み替えるので、bmson と #DEFEXRANK で 100 の意味が違う
  - 仕様の参考例（100 ＝ #RANK 2）とは **[差]**

### 2.8 total

- 仕様（`BMSON`「total」）: **標準の増え方に対する相対値（%）**。100 が標準。0 なら増えない。負なら絶対値。標準の増え方はプレイヤー次第。参考として IIDX の近似「全部 PGREAT で 7.605 × n / (0.01 × n + 6.5) % 増える」を挙げる。
- beatoraja: 0 以下なら警告して既定の扱い（`JBP:98-103`）。正なら **TOTAL ＝ total / 100 × 既定の TOTAL**、既定の TOTAL ＝ max(260, 7.605 × n / (0.01 × n + 6.5))（n はノーツ数）（`BRJ/play/BMSPlayerRule.java:68-88`）。仕様の「0 なら増えない」は実装していない。

### 2.9 BGA

- `bga_header`: `{id, name}`。id が重なれば後のもの（警告してよい）。推奨サイズ 1280×720（1920×1080 も可）。画面比が違うときは中央で切り取られうる。形式は PNG と WebM（音声は無視してよい）（`BMSON`「bga :: BGA」）。
- `bga_events`（背景）、`layer_events`（背景の上に重ねる）、`poor_events`（ミスのとき）: `{y, id}`。
- **layer は BMS の #xxx07 と違い、黒を透明にしない**。透明にしたいなら透明を持てる形式（PNG）を使う（同）。
- beatoraja: `poor_events` は MISS のときの表示として、`layer_events` は `condition`（play/miss/常時）付きのレイヤとして読む（`JBP:395-436`）。

---

## Part 3 — BMjoy への写し方

対象の型は `BMJ/chart/chart.h` の `Note` / `ScrollSeg` / `BarLine` / `BgmEvent` / `BgaEvent` / `ChartMeta` / `Chart`。API は BMS と同じ形で、`parse_osu_text/file` と `parse_bmson_text/file` が `...ParseResult{ok, error, warnings, chart}` を返す形が素直（`BMJ/chart/bms_parser.h:19-31`。osu と bmson には #RANDOM が無いので、乱数の欄は不要）。

### 3.1 osu!mania → 内部モデル

**取り込む条件**

- `[General] Mode: 3` だけ。キー数は `CircleSize` を四捨五入（1.2）。
- 7K → 皿なし 7K（`KeyLayout::K7`）。8K → 譜面ごとの選択（仕様書 2章）。それ以外のキー数は一覧に出さない。

**時刻（time_us）**

- ノーツ・ホールドの終端・タイミングポイント・PreviewTime: ファイルの ms（v5 未満は +24ms）× 1000。
- 音源（`AudioFilename`）は BgmEvent として時刻 0 に置く。
- 時刻が負のノーツ・タイミングポイントがありうる（音源の先頭より前）。AudioLeadIn は「時刻 0 より前に待つ時間」なので、開始前の待ち時間に足す。

**レーン**

| キー数 | 列 → lane |
|---|---|
| 7K | 列 0〜6 → lane 1〜7。lane 0 は空 |
| 8K「左端を皿」 | 列 0 → lane 0、列 1〜7 → lane 1〜7 |
| 8K「右端を皿」 | 列 7 → lane 0、列 0〜6 → lane 1〜7 |

- `SpecialStyle: 1`（N+1 配置）は「左端が特別なキー」という作者の意図を示す手がかりになる [?: stable の N+1 で特別なキーが左端になることは、wiki の記述と一般的な知識による。lazer はこの値を読むだけで使っていない（`LegacyBeatmapDecoder.cs:285-286`）]。

**ノーツの種類**

- ビット0 → `NoteKind::Normal`。ビット7（かつビット0なし）→ `NoteKind::Long`、`end_us` = 終端。
- 長さ 0 のホールド（終端 ≦ 始点）の扱い → 未決（下の表）。
- 地雷・見えないノーツは osu に無い。

**スクロール（ScrollSeg）**

- タイミングポイントの時刻ごとに 1 区間を作る:
  - `bpm` = 60000 / (その時点の非継承点の beatLength)。beatLength は 6〜60000 に丸め込む
  - `scroll` = その時点の SV（継承点の 100/(−beatLength)、0.01〜10 に丸め込む）。**非継承点で 1 に戻る**。同じ時刻に継承点があれば継承点の値
  - `stop` = 常に false
  - `speed` = bpm × scroll（拍/µs に換算）
- 最初のタイミングポイントより前は、最初の点の値で延ばす（lazer と同じ）。
- 最後のノーツより後の点は見た目に影響しないので、無視してよい（lazer は捨てている）。
- `pos` は speed を積分した値。これで lazer の Sequential と同じ形の「時刻→位置」になる（定数倍だけが違う）。
- `main_bpm` = 60000 / 最頻の拍の長さ（1.4.2 の 4 の求め方）。`initial_bpm` = 最初の非継承点の BPM。
- フローティングの基準 BPM を「一番長く使われる BPM」にすると、lazer の基準と一致する。ただし BMS の `main_bpm` は「ノーツ数が一番多い BPM」で求めている（`BMJ/chart/bms_parser.cpp:411-418`）ので、定義が違う。
- HS-FIX の CONSTANT（`Chart::make_constant_speed`）は、lazer の Constant Speed mod と同じ（BPM も SV も無視）。osu には STOP が無いので「止まる」ことも無い。
- 仕様書 6章の「osu のソフラン（未決）」は、本書 1.4 で「BPM の変化 → bpm、SV → scroll」と確定できる（lazer はそう動いている。stable も、曲の途中の BPM 変化で速さが変わることは wiki で確認できる）。

**小節線（BarLine）**: 1.5 の規則で作る。

**音**

- `hitSample` の `filename` があれば、それを `wav` に登録して `Note::sound` にする（キー音）。
- 無ければ、仕様書 5章のとおり「無音」か「汎用ヒット音」を設定で選ぶ。
- ホールドの終端の音（`end_sound`）は 0。
- 押したときに鳴る音のモデル（次の未判定ノーツの音）は、BMjoy の既存の動きと同じ。
- `[Events]` の `Sample` 行（`.osb` も含む）→ `BgmEvent`。

**画像**

- 背景画像 → `ChartMeta::stagefile` / `backbmp`。
- 動画 → 開始時刻に Base レイヤの `BgaEvent`。
- それ以外のストーリーボード（スプライト・アニメ）は対象外。

**メタデータ**

| 内部 | osu の項目 |
|---|---|
| `title` | `TitleUnicode`（無ければ `Title`） |
| `artist` | `ArtistUnicode`（無ければ `Artist`） |
| `subtitle` | `Version`（例 `[Version]`） |
| `subartist` | `Creator` |
| `playlevel` | 0（.osu にはレベルが無い。星の数はオンライン側の値） |

- `BeatmapID` / `BeatmapSetID` は外部 ID として持つ。

**その他**

- `total` = 0（未定義。仕様書 5章のとおり既定式で補う）。
- ハッシュは原本の MD5 と SHA-256（仕様書 5章）。

### 3.2 bmson → 内部モデル

**取り込む条件**

- `mode_hint` = `beat-7k`（省略時も beat-7k）だけ。
- `beat-5k` は仕様書 2章で未決。それ以外（10k / 14k / popn / generic）は一覧に出さない。
- `version` が無いものの扱い → 未決（下の表）。

**時刻と位置**

- 拍 = y / resolution。`init_bpm` と `bpm_events` で区間を作り、`stop_events` は `ScrollSeg::stop`（停止の長さ = 60 × duration ÷ (resolution × BPM) 秒）。
- `scroll_events`（beatoraja 拡張）は `ScrollSeg::scroll`。
- STOP と同じ y のノーツの時刻は、停止の始まり（BMS と同じ）。
- 同じ y の STOP を足すか、上書きするか → 未決（仕様は足す、beatoraja は上書き）。

**レーン（beat-7k）**: x 1〜7 → lane 1〜7、x 8 → lane 0。皿のノーツが無ければ BMS と同じく `KeyLayout::K7`（`BMJ/chart/bms_parser.cpp:409`、仕様書 2章）。x が 0・null・範囲外なら `BgmEvent`。

**LN**

- `l > 0` → `NoteKind::Long`、終点は y + l。
- `info.ln_type` / ノーツの `t`（beatoraja 拡張）→ `LnHint`。
- `up`（beatoraja 拡張）→ `end_sound`。
- LN の中のノーツは BGM に落とす（beatoraja と同じ）か、捨てる → 未決。

**キー音と `c`**

- `Note::sound` が今は「ファイル ID」なので、切り出しを表すには「(ファイル, 開始位置, 長さ)」の表が要る。
  - 例: `wav` とは別に `slices` を持ち、`sound` は切れ端の ID を指す
  - 長さ 0 を「最後まで」とするかは、2.5.2 の [差] によって決める
- 同じ y・同じ channel のノーツは同じ切れ端を指す（同時発音 1）。
- 重なったノーツ（別 channel の同じ (x, y)）は、1 つのノーツで複数の切れ端を鳴らす必要がある。今の `Note` は音を 1 つしか持てない。

**地雷・見えないノーツ（beatoraja 拡張）**

- `mine_channels` → `NoteKind::Mine`、`damage` は値そのまま。
- `key_channels` → `NoteKind::Invisible`。

**BGA**

- `bga_header` → `bmp`。
- `bga_events` → `BgaEvent::Layer::Base`、`layer_events` → `Layer`、`poor_events` → `Poor`。
- layer は黒を透明にしない（BMS と違う）。

**小節線**: `lines` があればその位置。空の配列なら無し。無ければ 4 拍ごと。

**メタデータ**

| 内部 | bmson の項目 |
|---|---|
| `title` | `title` |
| `subtitle` | `subtitle`（＋`chart_name`） |
| `artist` | `artist` |
| `subartist` | `subartists` |
| `genre` | `genre` |
| `playlevel` | `level` |
| `backbmp` | `back_image` |
| `stagefile` | `eyecatch_image` |
| `banner` | `banner_image` |

**判定と TOTAL**

- `judge_rank` → 下の表の未決事項（今の `Chart::judge_rank` は LR2 の #RANK 0〜3 の整数だけ）。
- `total` → 「total / 100 × 既定の TOTAL」。既定の TOTAL を何にするか → 未決。BMjoy は LR2 の既定式（`BMJ/chart/bms_parser.cpp:420-427`）、beatoraja は IIDX 型の式。

**ファイル名**

- 拡張子を省略したときと、見つからないときの代替（wav/ogg/m4a…）。仕様書 5章の「LR2 と同じ範囲に限る」との兼ね合いがある。
- 危ないパス（絶対パス、`..`、ヌル文字）は拒否する（仕様で必須）。

### 3.3 未決事項（中立に列挙）

| 項目 | 選択肢・論点 |
|---|---|
| osu の判定幅 | (a) OD から stable の式で作る（PERFECT 16 固定、他は 64−3·OD など）、(b) lazer の式（PERFECT も OD で変わる）、(c) 一番近い LR2 の #RANK に丸める、(d) OD を無視して BMS の NORMAL。BMS の判定（PG/GR/GD/BD/POOR の 5 段）と osu の 6 段の対応も要る |
| osu の判定の種類 | MAX / 300 / 200 / 100 / 50 の 6 段を出すか、BMS の 5 段に寄せるか。ホールドを 1 つの判定（stable）にするか、始点と終点の 2 つ（lazer・BMS の CN）にするか。終点の窓を 1.5 倍にするか |
| osu のライフ | HPDrainRate を使うか（1.8）、BMS のゲージと既定の TOTAL に揃えるか（仕様書 5章は後者） |
| osu 8K の皿 | 左端・右端・非対応のどれを既定にするか。`SpecialStyle` を初期値の手がかりにするか |
| osu 4K・6K など | 仕様書 2章では一覧に出さない。将来「7K の配置で遊ぶ」機能を入れるかは BMS 5K と同じ論点 |
| osu の長さ 0 のホールド | 通常ノーツにするか、長さ 0 の LN のまま残すか |
| osu のヒット音 | filename の無いノーツを無音にするか、汎用ヒット音にするか（仕様書 5章で設定）。追加音（whistle/finish/clap）を重ねるか（wiki と lazer で違う）。譜面フォルダのカスタムサンプル（`<set>-hit<sound><index>.wav`）に対応するか |
| osu の音量 | ノーツごと・タイミングポイントごとの音量。今の `Note` / `BgmEvent` に音量の欄が無い |
| osu のストーリーボードの音 | Pass / Fail レイヤの音を、ゲージの状態で鳴らし分けるか、全部鳴らすか、Background / Foreground だけ鳴らすか。`.osb` を読むか |
| osu の動画・背景 | 背景画像を STAGEFILE と BGA のどちらに使うか。動画の開始時刻が負のときの扱い |
| osu の AudioLeadIn | 開始前の待ち時間に足すか、BMjoy の既定の待ち時間に任せるか |
| 主 BPM の定義 | osu は「時間で最頻」（lazer）、BMS は「ノーツ数で最頻」（今の BMjoy）。揃えるか、形式ごとに変えるか |
| bmson の版 | `version` の無い 0.21 を読むか（Bemuse は読む）、拒否するか |
| bmson の judge_rank | (a) 仕様の参考どおり 100 ＝ #RANK 2 として判定幅を伸縮する、(b) beatoraja と同じく 100 ＝ EASY 相当、(c) 一番近い #RANK に丸める。どれも `Chart` に「判定幅の倍率」を持たせるかどうかに関わる。beatoraja の「1〜4 は #RANK」の特例を入れるか |
| bmson の total | 既定の TOTAL を LR2 式にするか IIDX 式にするか。0 を「増えない」とするか（仕様）、未定義とするか（beatoraja） |
| bmson の切り出し | 次が「頭に戻す」ノーツのとき、切れ端をそこで切るか（仕様の例）、最後まで鳴らすか（Bemuse・beatoraja）。同じ y で `c` が混ざったとき（仕様は「無し」）。切れ端を読み込み時に別バッファにするか、再生時に開始位置を指定するか（12章のメモリの方針と関わる） |
| bmson の重なったノーツ | 1 つのノーツに複数の音を持たせる形に `Note` を広げるか |
| bmson の同じ y の STOP | 足す（仕様）か、上書き（beatoraja）か |
| bmson の beatoraja 拡張 | `scroll_events`、`mine_channels`、`key_channels`、`ln_type`、`t`、`up`、BGA のシーケンスに対応するか（beatoraja 用に作られた bmson はこれを使う） |
| bmson の拡張子の代替 | 仕様は「拡張子を外して探す」。BMS は「LR2 と同じ範囲（wav と ogg の間）だけ」。bmson に m4a などを足すか |
