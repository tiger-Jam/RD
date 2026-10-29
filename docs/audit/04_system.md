# 監査 04: システム（リザルト・その他の画面・設定・音声と入力・スキンの読み込み）

プレイ・譜面・選曲を除く全部について、LR2 と RD の振る舞いを突き合わせた。

- 調べた日: 2026-10-19。RD は `src/` の現状のコードを読んで判定した（実行しての確認はしていない）。
- 振る舞いだけを文章と表で書いている。OpenLR2 のコードは写していない。
- コードは一切変更していない。

## 凡例

**出典の書き方**
- `O` = OpenLR2 `参考に/OpenLR2-main/LR2/` 配下（例 `O LR2_statplay.cpp:123`）
- `B` = RD `src/` 配下（例 `B play_session.cpp:456` は `src/app/play_session.cpp`）。章の中でファイル名だけのときも、RD 側は `src/` 配下
- `H` = 実物の LR2（`参考に/LR2beta3 2/` の `help/*.html`、`history.txt`、`LR2files/`。いずれも CP932）
- `仕様書` = `次世代BMSプレイヤー 仕様書 v0.1.md`
- 章ごとの略記（`configsave`、`load`、`ls` など）は各章の頭に書いた
- `:123` のように行番号だけのときは、同じ行の直前のファイルを指す

**印**
- `[?]` 確かでないもの（OpenLR2 と実物の LR2 が一致しているか未確認、など）
- `[EXT]` OpenLR2 にだけある拡張で、原典の LR2 には無いもの

**判定**
- 同じ／違う／RDに無い／意図した違い（仕様書などで RD が決めている違い。7K+S の 1P 専用、IR を持たない、RD/PLAY タブ、GAS、LIFT、ランプ 7 種、SHA-256 の記録キーなど）／LR2に無い（RD だけ、または OpenLR2 拡張にだけあるもの）

## 目次

1. リザルト・コースリザルト・スコア保存・リプレイ
2. その他の画面（起動・キーコンフィグ・スキンセレクト・コースリザルトの出入り・決定・一覧だけのもの）
3. 設定項目（config.xml と SETUP の各タブ）
4. 音声・画面・入力・スクリーンショット
5. スキンの読み込み・描画（命令と意味）
6. RD に無いもの（優先度の目安）
7. 違っているもの（直すべき候補）

## 1. リザルト・コースリザルト・スコア保存・リプレイ

### 1.1 リザルトの入力の流れ

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| タイマー150/151（グラフ） | 経過時間がゲージグラフの op3 を超えたら 150、op4 を超えたら 151 を立てる | 同じ | 同じ | O Scene05_Result.cpp:33-38 / B play_session.cpp:712-713 |
| 入力を受け付け始める時刻 | `#STARTINPUT` の 1 番目を過ぎてから | 同じ | 同じ | O Scene05_Result.cpp:266 / B play_session.cpp:481 |
| 1 回目の決定 | 151 がまだなら 151 を立て、グラフを一気に最後まで描く | 同じ | 同じ | O :268-276 / B :487-489 |
| 2 回目以降 | 151 から rank 待ち時間が過ぎたら 152（更新表示）。152 から update 待ち時間が過ぎたらタイマー2（フェードアウト） | 同じ | 同じ | O :280-303 / B :490-497 |
| 152 を飛ばす条件 | isNosave のとき（ラウドネス・低速・ADD NOTE・BATTLE・リプレイ再生・LUNARIS に加え、**ASSIST・HS固定CONSTANT・SCATTER/CONVERGE で保存したあとも**） | 記録しないとき・オートのときだけ。ASSIST や SCATTER でも 152 が出る | 違う | O LR2_bmsload.cpp:3334-3351, LR2_statplay.cpp:925 / B :492 |
| IR の応答待ち | LR2IR の通信が終わるまで 152 の手前で待つ | 無し | 意図した違い（公式IRは持たない） | O :279-283 / 仕様書:38 |
| 進めるキー | ESC、Enter、1P/2P の鍵盤 1〜7、マウスの左右クリック | ESC、Enter、1P の鍵盤 1〜7。マウスは効かない | 違う（マウス無し。2P は 1P のみの仕様の範囲外） | O :262-266 / B :666-669, 701-702 |
| 抜けた先 | 選曲。コースのステージなら次のステージかコースリザルト | 同じ | 同じ | O Main.cpp:1853-1863 / B :1110-1127 |
| クイックリトライ | フェードアウトが終わった時点で白鍵と黒鍵を同時に押していれば同じ譜面をもう一度。ランダムは変わらない（シードを保持し、BGA も読み直さない）。リプレイ再生と LUNARIS では効かない | 同じ。同じシードの譜面を使い回す | 同じ | O Scene05_Result.cpp:210-223, 314-338 / H help/12.html:31-33 / B :334-337, 714-722 |
| リトライで新しいランダム | 2 番の黒鍵で押すとシードを作り直す [EXT] | 無し | LR2に無い（OpenLR2 の拡張） | O :320 / LR2_configsave.cpp:1166 |
| コース中のリトライ | STAGE1 からやり直す（シードも作り直す）[?原典] | ステージのリザルトでもコースリザルトでも無効 | 違う[?] | O :216-219 / B :720, 1236-1241 |
| F5 ですぐリトライ | 無し | リザルトで F5 を押すとフェードアウトを待たずにもう一度 | LR2に無い | B :670-672 |
| リザルトでのゲージ表示の切り替え | GAS のとき、ボタン13 で表示するゲージを替える [EXT] | 無し | LR2に無い（OpenLR2 の拡張） | O :252-260 |
| クリア／失敗の音 | クリアなら clear、失敗なら fail | 同じ | 同じ | O :22-27 / B :458 |

### 1.2 スコアを保存する条件

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| オートプレイ | リザルトを出さず、保存もせずに選曲へ | 同じ | 同じ | O Main.cpp:1697-1705 / B :416-421 |
| 1 ノーツも処理していない、GOOD 以上が 0 | リザルトを出さず、保存もしない | 同じ | 同じ | O Main.cpp:1706-1735 / B :416-417 |
| 途中でやめた | FAILED として保存。EX は良ければ更新。残りのノーツを BP に足す | 同じ | 同じ | O LR2_statplay.cpp:997-1000 / B :121-123 |
| **閉店（HP切れ）** | 処理したノーツが総数に届かなければ、**いつでも**残りを BP に足す | `quit_early` のときだけ足す。閉店は `enter_result(false)` なので足さない。**最小 BP が少なく記録される** | 違う | O :992-1000 / B :122, 962 |
| ASSIST・SCATTER・CONVERGE・HS固定CONSTANT | ランプは EASY が上限（**FC でも EASY**）。記録するのはランプ・回数・オプション履歴だけで、**EX・判定数・最大コンボ・最小BP・ゴーストは記録しない** | ASSIST/SCATTER は EASY が上限だが FC は残す。EX・BP・コンボ・判定数も記録する。CONVERGE は上限が無い。CONSTANT は何も記録しない（プレイ回数も増えない） | 違う | O :895-926 / B :180-188, 437-445, lane_option.cpp:111 |
| コース中のステージ | ASSIST・SCATTER 以上・CONSTANT・オートキー・マニアックなどでは**そのステージを保存しない**。S-RANDOM は保存する（docs/lr2_course_spec.md の「S-RANDOM 以上」は誤り） | CONSTANT だけ止める。ASSIST・SCATTER でも EX などを書く | 違う | O :7-23, 598 / B :451 |
| リプレイ再生 | 保存しない | リプレイが無い | RDに無い | O LR2_bmsload.cpp:3342 |
| 遅延率が高すぎる | 保存しない [?原典] | 無し | RDに無い | O :767-771 |
| DB に無い譜面（ダイレクト起動） | 保存しない | SHA-256 で何でも保存する | 違う（害は小さい） | O :772-777 |
| BATTLE・DB・SP→DP・EXTRA | それぞれ別のランプ（clear_db/sd/ex）を残す | モード自体が無い | 意図した違い（7K+S の 1P のみ） | O :811-893 |

### 1.3 保存する値

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| ゲージ → ランプ | EASY→2、GROOVE→3、HARD/G-ATK/P-ATK→4（HP 2% 以上）、DEATH→3 [?]、全ノーツをつないだら 5 | 同じ。ただし GOOD が 0 なら 6 (PERFECT) | 意図した違い（仕様書:643 のランプ7種） | O :163-190 / B gauge.cpp:96-104 |
| ランプの更新 | 良いほうを残す。コースのステージでは FC (5) のときだけ上げる | 同じ | 同じ | O :951, 618 / B song_db.cpp:600 |
| 自己ベストの EX | **より大きいときだけ**、判定数・スコア・レート・ランク・総ノーツ・rseed・op_best をまとめて入れ替える | EX は大きいほう。判定数は EX が大きいときだけ入れ替える。rseed・op_best・ランクは無い | 違う（項目が足りない） | O :959-987 / B song_db.cpp:598-601 |
| 最小 BP | BAD+POOR に残りノーツを足して、小さいほう | 小さいほう（閉店時は足さない。1.2 参照） | 違う（閉店時） | O :992-1001 / B :602 |
| 最大コンボ | 大きいほう | 同じ | 同じ | O :989 / B :603 |
| op_history | ASSIST なら 0x1000000。EASY ゲージの EASY ランプなら 8。NORMAL 以上ならゲージ・ランダム・HID/SUD のビットを OR | 同じ | 同じ | O :59-100 / B :111-119, gauge.h:11 |
| op_best・rseed | 自己ベストを更新したときに記録 | 無し | RDに無い | O :977-984 |
| ゴーストのデータ | 自己ベストを更新したら、判定の並びを文字列にして DB に書く | 保存しない。EX から PG と GR だけの仮のゴーストを作る | RDに無い | O :1014-1016, LR2_ghost.cpp:697 / B :363, ghost.h:25 |
| complete（完走フラグ） | 全ノーツを処理したら 1 | 無し | RDに無い | O :947 |
| プレイ・クリア・失敗回数 | プレイ回数を増やし、クリアか失敗の片方を増やす（コースのステージでは増やさない） | プレイ回数とクリア回数だけ（失敗回数は差で出せる）。コースのステージでは増やさない | 同じ（ほぼ） | O :1003-1007 / B :604-607 |
| プレイヤーの累計 | 判定数とコンボは**プレイ中に毎回**数える（保存しないプレイも含む）。プレイ時間も足す | 保存したときにだけ判定数を足す。プレイ時間は無い。コースのステージは数えない | 違う | O Scene04_Play.cpp:81-110, LR2_statplay.cpp:936 / B song_db.cpp:615-622 |
| 総ノーツ・レート・ランク | DB に持つ | 持たない（その場で計算） | 違う（害は小さい） | O LR2_statlong.cpp:236 |
| scorehash（改ざん検査） | パスワードの MD5 を混ぜたハッシュ | 無し | RDに無い（DB が別なので低） | O LR2_statlong.cpp:234 |
| 記録のキー | MD5 | SHA-256 と LN の種類の組 | 意図した違い（仕様書:348） | B song_db.cpp:583 |
| IR への送信 | 自己ベスト一式とゴーストを送る | 無し | 意図した違い | O :1021-1078 |

### 1.4 リプレイ・スクリーンショット

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 記録するタイミング | 設定が OFF 以外のとき、オート以外の全プレイで入力を記録する。抜けるときに、ALWAYS なら常に、HISCORE なら EX がベスト以上、CLEAR ならクリア、FC なら FC のときに上書き（コードは 3=CLEAR、4=FC。help の並びとは逆 [?]） | 無し（スキンには常に「リプレイ無し」と返す） | RDに無い | O Main.cpp:1218-1239, 1791-1826 / H help/4.html:173-179 / B select_context.cpp:123 |
| 保存の枠 | 譜面ごとに 1 つ（`LR2files/Replay/<ID>/<md5>.lr2rep`）。コースはステージごとに `__N` に書き、終わりに `c/<コースhash>/N` へ移す | 無し | RDに無い | O LR2_replay.cpp:9-45, 120-141, Main.cpp:1872-1897 |
| ファイルの形 | 時刻・種類・値の固定長レコードの並び。時刻0にオプション・音量・EQ などのヘッダ、乱数シードは種類 200。保存のときに最終のゲージで書き直す。LIFT は [EXT] | 無し（仕様書では全プレイの入力ログを予定） | RDに無い | O LR2_replay.cpp:188-245, LR2_bmsload.cpp:2149 / 仕様書:273-276 |
| 選曲画面からの再生 | 6 番キーを押し続け、2 秒でリプレイ再生、その前に離すとオート。スキンのリプレイボタンもある。再生中は保存もリトライもせず、終わったら設定を元に戻す | 無し | RDに無い | O Scene02_Songselect.cpp:2105-2147 / H history.txt:393-394 |
| スクリーンショット | F6 でどの画面でも `screenshot/LR2 日時.png` に保存し、シャッター音を鳴らす | 無し（開発用の BMP 保存だけ）。**選曲画面の F6 はレーンカバーの切り替えに使っている** | RDに無い／キーがぶつかる | O LR2_gameloop.cpp:496, Main.cpp:2411-2418 / H help/3.html:139 / B select_scene.cpp:1295 |

### 1.5 コースリザルトとコースのスコア

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 通しの値 | 判定数・処理済みノーツ・最大コンボはコース全体の累計。EX は累計から計算し直す。総ノーツには遊ばなかったステージの分も足す | 各ステージの値を足していき、遊ばなかった分は DB のノーツ数で足す | 同じ | O LR2_statplay.cpp:241-256 / B :1118-1124, 1146-1151 |
| コースのランプ | 最終ステージまで生きて終えたらクリア。全体を 1 本のコンボでつないだら FC。HP の比較は「2 より大きい」。EASY の上限はかけない | 同じ。ただし PERFECT (6) があり、ASSIST/SCATTER では EASY が上限 | 違う[?]／PERFECT は意図した違い | O :257-272 / B :1155-1165 |
| 保存しない条件 | isNosave・オート・リプレイ再生 | 記録しないとき・オート | 同じ（ほぼ） | O Scene13_Courseresult.cpp:11 / B :1186 |
| 保存する値 | 初回は総ノーツ、complete、ランプは良いほう、EX が大きければ判定数・rseed・op_best、最大コンボ、最小BP（残りノーツを足す）、回数、op_history（ASSIST ビット付き）、プレイヤーの累計。ゴーストは書かない | ランプ・EX・判定数・最大コンボ・BP（残りを足す）・op_history（ASSIST ビット無し）・回数・累計 | 違う（rseed・op_best・complete・ASSIST ビットが無い） | O Scene13:13-95 / B :1186-1197 |
| 入力の流れ | 普通のリザルトと同じ処理（150/151/152 とリトライ） | 150/151/152 は同じ。リトライは無い | 違う（リトライ） | O Main.cpp:1484-1486 / B :1213-1241 |
| 音 | クリアなら courseclear、失敗なら coursefail | 同じ | 同じ | O Scene13:181-184 / B :1201 |

## 2. その他の画面

### 2.1 起動の流れ

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 構成 | 設定アプリ LR2.exe から本体 LR2body.exe を起動 | 設定アプリ（RD）から本体（RDbody）を起動。RD タブを足してある | 同じ（タブは意図した違い） | H help/2.html:93 / B CMakeLists.txt:111, src/launcher/launcher.cpp:1-3 |
| 曲フォルダが未登録 | エラーを出して終了 | 確認しない | 違う | O Main.cpp:299-302 / B src/main.cpp:400-428 |
| スコア DB が無い | エラーを出して終了 | 確認しない（自前の DB を開くだけ） | 意図した違い[?] | O Main.cpp:422-428 / B src/app/app.cpp:91 |
| 読み込み中の窓 | 一旦ウィンドウで出し、PUSH ANY KEY のあと設定の画面モードに切り替える | 最初から設定どおり | 違う（小） | O Main.cpp:491-497, 672-678 / B src/app/app.cpp:71 |
| 読み込み中の背景 | LR2files/Config/loading.bmp（beta3 の同梱物には無い） | 黒い画面 | 同じ[?] | O Main.cpp:544 |
| IR へのログイン表示 | LR2IR のログイン結果を表示。CustomIR とライバル同期は [EXT] | 無し。代わりに難易度表の取得状況を表示 | 意図した違い／LR2に無い | O Main.cpp:557-632 / B src/main.cpp:273-295 |
| 曲 DB の更新（起動時） | autoreload が 2 なら全部、それ以外はルートだけ確認 | `db_update` が 0 なら全部（Now Loading とパスを表示）、1 なら何もしない | 違う（ルートだけの確認が無い[?]） | O LR2_songmanage.cpp:2819-2834, :73 / B src/main.cpp:259-271 |
| 更新方式「開いたフォルダで自動」 | 有り（autoreload=1） | 灰色で選べない | RDに無い | O Scene02_Songselect.cpp:2383 / B src/launcher/launcher.cpp:474 |
| 手動の再読み込みキー | F8。F5 は IR に接続 | F5 | 違う | H history.txt:464 / O Main.cpp:2210 / B src/app/select_scene.cpp:1283 |
| PUSH ANY KEY | 版名と PUSH ANY KEY を出し、何か押すと READY | 同じ（キー・パッドのボタン・マウス） | 同じ | O Main.cpp:658-668 / B src/main.cpp:297-318 |
| タイトル画面 | 無い（OPENING 型は登録だけで画面は無い） | 無い | 同じ | O LR2_skinmanage.cpp:156, Main.cpp:847 |
| 譜面を指定して起動 | PUSH ANY KEY を飛ばしてプレイへ | 同じ | 同じ | O Main.cpp:649, 850-851 / B src/main.cpp:424, 456-457 |
| 録画・自動演奏用の引数（-auto2avi など） | 有り | 無し | RDに無い | O Main.cpp:371-397 |
| 選曲で ESC | フォルダの深さに関係なく、即座に終了 | フォルダの中なら一つ戻り、ルートでだけ終了 | 違う | O Main.cpp:2578-2584 / B src/app/select_scene.cpp:1278-1282 |
| 選曲で F1 | 押している間、F キーの説明を表示 | キー割り当てウィザードを開く | 違う | O Main.cpp:2208-2212 / B src/app/select_scene.cpp:1285-1289 |
| 終了時の保存 | config・midi などを書き出す | 設定を保存 | 同じ | O Main.cpp:2647-2651 / B src/main.cpp:459 |

### 2.2 キーコンフィグ

RD 側の略記：`ks` = src/app/keyconfig_skin_scene.cpp、`ke` = src/app/keyconfig_editor.cpp、`kc` = src/input/keyconfig.cpp。

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 入り方 | 選曲のボタン 13。タイマー2 を立て、システム音を止めてオプションの音を鳴らし、選曲の #FADEOUT の後に移る（決定音ではない） | 同じ。ただしフェードアウトの間は曲リストを描かない（選曲画面の描画の外で描いているため）。スキンが無ければこれまでのウィザードをすぐ開く | 直した（曲リストだけ違う） | O LR2_skinobject.cpp:3056-3061, Scene02_Songselect.cpp:2071-2094 / B ks `open_keyconfig_from_select`, select_scene.cpp のボタン 13 |
| 画面 | スキン（型 8, keyconfig.csv）で描く | 同じ（`keyconfig_skin`。空なら LR2 の既定、"none" ならウィザード）。スキンセレクトの型 8 でも選べる | 直した | O Main.cpp:1373-1380 / H Theme/LR2/KeyConfig/keyconfig.csv / B ks, config.h, skin_catalog.cpp |
| 入ったときの状態 | 7 鍵モード、1P の 1 鍵を選び、スロットは選んでいない | 同じ | 直した | O Main.cpp:1375-1378 / B keyconfig_editor.h |
| スキンの部品 | 文字 40-55（各スロットのキー名。空きは "-"）、OP 400-402（鍵盤モード）、ボタン 101-139（項目を直接選ぶ。番号の下 2 桁が項目：1-7 鍵、10/11 皿、12 START、13 SELECT、21-27・30-33 は 2P）、140-142（各モードの項目を前後に送る）、143（鍵盤モード）、150-157（スロット） | 文字 40-55、OP 400（常に真）/401/402（常に偽）、ボタン 101-113 の 1P の分・140・150-157。2P の項目・141・142・143 は押しても何もしない | 直した（2P・9 ボタン・5 鍵は意図した違い） | O Scene06_Keyconfig.cpp:6-18, 684-733, LR2_skinobject.cpp:1236-1244, 3591-3672 / B ke |
| 鍵盤モード | 7/14、9 ボタン、5/10 の 3 つをボタン 143 で切り替え。ファイルも別 | 7 鍵＋皿だけ | 意図した違い（7K+S 専用） | O LR2_skinobject.cpp:3637-3660 / H help/5.html |
| 2P 側 | 7 鍵モードに 2P のキーも含む（22 項目） | 1P だけ（ボタン 140 も 1P の 11 項目と「無し」だけを送る） | 意図した違い[?] | O Scene06_Keyconfig.cpp:684-733 |
| 割り当てる項目 | 鍵盤・皿の上下・START・SELECT | 同じ | 直した | O LR2_configsave.cpp:67-90 / B ke `keyconfig_item_button` |
| 操作の流れ | 項目を選ぶ→スロットを選ぶ→押す。一つのキーに 8 スロット。割り当てた後もスロットは選んだまま | 同じ。スロットは割り当ての並びの n 番目。空きは詰めて持つので、空いたスロットに入れると一番前の空きに入る。同じ項目のほかのスロットにある入力を入れると、そちらから移る | 直した（空きを詰める点だけ違う） | O Scene06_Keyconfig.cpp:54-70 / H help/5.html / B kc `set_slot` |
| 割り当てできる機器 | キーボード、パッド（ボタンと軸の上下左右、複数台）、MIDI（ノート・ベンド・ペダル）。名前は `1P:BUTTON4`・`MIDI NOTE:F3`・`MIDI BEND:-`・`MIDI PEDAL` | キーボード、パッド（ボタン・軸・ハット）、MIDI（ノート・ベンド・ペダル）。パッドが 2 台以上つながっていれば押した台に決めて割り当て（`JOY2:BUTTON4`）、1 台なら台を決めない（`JOY:BUTTON4`）。MIDI の名前は LR2 と同じ。アナログ皿はスキンの画面では設定しない（ウィザードと `--keyconfig` の一覧で設定する） | 直した（台の名前が `1P:` でなく `JOY1:`、1 台のときは台を決めない点が違う）／アナログ皿は LR2に無い | O Scene06_Keyconfig.cpp:638-678 / B ks, src/input/input.cpp `BindingCapture`, keyconfig.cpp `binding_label` |
| 割り当てられないキー | ESC・Enter・矢印・F1〜F12（help はテンキーでない数字キーも禁止と書くが、OpenLR2 の一覧には無い[?]） | ESC・Enter・矢印・F1〜F12（数字キーは割り当てられる） | 直した | O Scene06_Keyconfig.cpp:20-43 / H help/5.html / B ke `keyconfig_reserved` |
| 1 スロットを消す | Delete | 同じ（オプションの音も鳴らして保存する） | 直した | O Scene06_Keyconfig.cpp:57 / B ks |
| 全部を消す・既定に戻す | OpenLR2 は F1 が既定に戻して保存、F2 が全部消す（保存しない）。help は「F1 で全スロットの割り当てを解除」、スキンの絵は「F1:リセット　F2:初期設定に戻す」で、OpenLR2 と逆に読める[?] | OpenLR2 と同じ（F1 既定、F2 全部消す）。F2 も保存する。アナログ皿の設定はどちらでも残す | 直した（F2 を保存するのは違う。LR2 は保存しないので、選曲に戻ると読み直して元に戻る） | O Scene06_Keyconfig.cpp:72-93 / B ke, ks |
| 保存 | 割り当てるたびに keyconfig*.xml に書き、選曲に戻るとき読み直す | 割り当てる・消すたびに keyconfig.txt（RD の形式）に書き、戻るとき読み直す。LR2 の XML は読み書きしない | 直した（ファイルの形式は意図した違い） | O Scene06_Keyconfig.cpp:59-67, Main.cpp:1866-1868 / B ks, kc |
| LR2 の設定を引き継ぐ | — | keyconfig.xml の取り込みは無い | RDに無い | B src/app/lr2_import.cpp |
| 抜け方 | ESC・右クリック→#FADEOUT→選曲 | 同じ。選曲の BGM を鳴らし直す | 直した | O Scene06_Keyconfig.cpp:94-98, 46-51 / B ks |
| 入力の確認表示 | スキン側の光り方だけ | スキンの画面は同じ。`--keyconfig` の一覧画面では押している鍵盤・軸の値・機器の一覧を表示 | LR2に無い（一覧画面だけ） | B keyconfig_scene.cpp:218-268 |

### 2.3 スキンセレクト

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 入り方 | 選曲のボタン 14。選曲がフェードアウトしてから移る | ボタン 14 で、フェードせずにすぐ移る | 違う（小） | O Scene02_Songselect.cpp:2078-2094 / B select_scene.cpp:1174-1182 |
| 画面 | スキン（型 9） | LR2 の skinselect.lr2skin を読んで描く | 同じ | B skin_select_scene.cpp:124-135 |
| 最初に選ばれている種類 | 入るたびに 7KEYS（種類 0） | 選曲画面にいる間は前回の種類のまま[?] | 違う（小） | O Scene07_Skinselect.cpp:445-447 / B skin_catalog.h:53 |
| 種類の選択（ボタン 170-188） | 0〜15 のどれでも | 押せるが、変更できるのは 0・5・6・7・10・15 だけ | 一部が RDに無い（5/14/10/9 鍵・バトルは意図した違い） | O LR2_skinobject.cpp:3676-3740 / B skin_catalog.cpp:26-36 |
| テーマ（種類 11） | テーマに書かれた全部のスキンとサウンドセットを一度に替える | 無い | RDに無い | O Scene07_Skinselect.cpp:7-51, LR2_skinobject.cpp:3780-3784 |
| スキンを替える（ボタン 190） | 前・次へ。サウンドセットなら読み直して BGM を鳴らす | 同じ | 同じ | O LR2_skinobject.cpp:3770-3816 / B skin_select_scene.cpp:185-195 |
| ボタン 191 | 選んだスキンを確定する | 無視する | RDに無い（小） | O LR2_skinobject.cpp:3817-3823 |
| カスタマイズ（220-229・文字 100-114・スライダー 7・ホイール） | 5 件ずつ表示してスクロール | 同じ | 同じ | O LR2_skinobject.cpp:3841-3878, 2585-2596, Scene07_Skinselect.cpp:69-98 / B skin_catalog.cpp:117-150, skin_select_scene.cpp:199-220 |
| カスタマイズの保存 | 変えるたびに SkinCustomize/<スキンのパスの MD5>.xml へ書く。#CUSTOMFILE は**ファイル名**で残す | config.txt にスキンのパスごとに残す。#CUSTOMOPTION は番号、#CUSTOMFILE は**名前**。書き出すのは画面を閉じたとき | 違う（書き出す時期）。#CUSTOMFILE を名前で残すのは直した（古い番号の形式も読む） | O LR2_skinobject.cpp:3851-3864 / B skin_catalog.cpp, config.cpp `parse_skin_custom`, select_scene.cpp:1178 |
| プレビュー | 動くスキン。プレイ用なら sample_*.bme を自動演奏して音も鳴らし、title.bmp を STAGEFILE にする | スキンをタイマーだけで動かす。サンプル譜面の演奏と音は無い[?] | 違う | O Scene07_Skinselect.cpp:301-437 / B skin_select_scene.cpp:28-100 |
| プレビューを出さない設定 | 有り（サムネイル表示） | 設定画面で灰色 | RDに無い | O Scene07_Skinselect.cpp:101-103 / B launcher.cpp:539 |
| サウンドセットのプレビュー | サムネイル | サムネイル | 同じ | B skin_select_scene.cpp:42-51 |
| 抜け方 | ESC・右クリック→#FADEOUT→選曲（サウンドセットを読み直す） | 同じ（選曲スキンを替えていたら読み直す） | 同じ | O Scene07_Skinselect.cpp:57-65, Main.cpp:1869-1872 / B skin_select_scene.cpp:166-169, 222 |

### 2.4 コースリザルト（Scene13）の出入り

（保存・集計は 1.5 を参照）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 入り方 | 最後のステージか落ちたときにコースリザルトへ。1 ノーツも判定していなければ選曲へ | 同じ（ノーツ無しは `abandoned` で選曲へ） | 同じ | O Main.cpp:1852-1862 / B play_session.cpp:1319-1329, 419 |
| 入力 | リザルトと同じ処理。鍵盤・Enter・ESC・マウスの左右 | 鍵盤・Enter・ESC。マウスでは進まない | 違う（小） | O Scene05_Result.cpp:262-266 / B play_session.cpp:1236-1246 |
| タイマー 151/152 と #FADEOUT | リザルトと同じ | 同じ | 同じ | B play_session.cpp:1216-1229, 1250 |
| 段位でオプションを変える | 終わっても元に戻さない | コースが終わると元に戻す | 意図した違い（コメントで明記） | B play_session.cpp:1284-1287 |

### 2.5 決定画面

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 自動でプレイへ | #SCENETIME を過ぎたら。未指定なら 0 なので、すぐプレイへ | 同じ（0 ならすぐプレイへ） | 直した | O Scene03_Decide.cpp:7-10, LR2_skinload.cpp:188 / B decide_scene.cpp:95-97 |
| 飛ばす | #STARTINPUT のあと、鍵盤・Enter・左クリック→タイマー3→#SKIP | 同じ（1P のみ）。#IGNOREINPUT も見る | 同じ | O Scene03_Decide.cpp:12-16, 31-36 / B decide_scene.cpp:97-98, 105-106 |
| 戻る | ESC・右クリック・START+SELECT。タイマー3 の後は受け付けない。システム音を止める | 同じ | 直した | O Scene03_Decide.cpp:18-23 / B decide_scene.cpp:99-104 |
| 画像 | STAGEFILE・BACKBMP・BANNER（100/101/102） | 同じ | 直した | O Main.cpp:1167-1197 / B decide_scene.cpp:40-55 |
| 決定音 | 隠しモードなら exdecide | 常に decide | RDに無い（隠しモードが無い） | O Main.cpp:1201-1204 / B decide_scene.cpp:57 |

### 2.6 一覧だけ（どれも RD に無い）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| Scene08 Lunaris | プレイ中に遊べるブロック落としのおまけ | 無し | RDに無い | O Scene08_Lunaris.cpp, LR2_gameloop.cpp:424 |
| Scene09 PO4 メニュー | FREE / ARCADE / CHARANGE / COURSE / GRADE / OPTION / EXIT のモード選択（型 17） | 無し | RDに無い | O Main.cpp:1390-1447, Scene09_Po4menu.cpp |
| Scene10 PO4 決定 | モード決定の演出（型 18） | 無し | RDに無い | O Scene10_Po4decide.cpp |
| Scene11 PO4 選曲／スターター | event.csv で動くスターターモード（is_starter） | 無し | RDに無い | O Main.cpp:304-320, 902-905, Scene11_Po4select.cpp |
| LR2_makemp3 | BMS を wav/mp3 に書き出す | 無し | RDに無い | O LR2_makemp3.cpp:14-167 |
| En_recordmovie | auto2avi / replay2avi / bga2avi で AVI を録画し、音を合わせる | 無し（開発用の `--capture` で画像を連番保存するだけ） | RDに無い | O En_recordmovie.cpp:155-257 / B main.cpp:93 |

## 3. 設定項目

この章の出典の略記：`configsave` = O LR2_configsave.cpp、`skinobject` = O LR2_skinobject.cpp、`songmanage` = O LR2_songmanage.cpp、`Songselect` = O Scene02_Songselect.cpp、RD 側のファイル名は `src/app/`・`src/launcher/` 配下。

- LR2 の `config.xml` の読み込みは configsave:936-1149、書き出しは :150-565、OpenLR2 拡張（`openlr2-config.xml`）は :1151-1179。`config.xmh` を読むコードは OpenLR2 に無く、RD が LR2 から引き継ぐときに `lr2_import.cpp:25` で先に探しているだけ。
- SETUP のタブ構成は help/8.html（beta2 の説明）と LR2.exe（100201）に埋め込まれた画面の文字列（タブ名 @0xa9a08、OPTION/SELECT/SYSTEM の項目名 @0xaa364〜0xaa840）で確認した。
- RD の設定は `~/Library/Application Support/BMjoy/config.txt` の `key = value`（config.cpp:13-153）。LR2 の `config.xml` から引き継ぐのは**曲フォルダとプレイヤー ID だけ**（lr2_import.cpp:30-45）。
- RD の SETUP のタブは `MAIN / JUKEBOX1 / JUKEBOX2 / OPTION / SELECT / SYSTEM / RD / PLAY`（launcher.cpp:433）。RD と PLAY は仕様書どおりの意図した違い。

### 3.1 MAIN タブ（`<system>` / `<player>` / `<network>`）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| PLAYER ID / PASSWORD / 新規作成・削除 | player.id / pass（既定は空。パスワードは MD5 にして送る） | 灰色で操作不可。ID は LR2 から引き継ぐときにスコア DB を探すためだけに読む | 意図した違い（IR を持たない） | configsave:1102-1103 / launcher.cpp:457-464, lr2_import.cpp:41 |
| player.name / irid / irpass | 既定は空 | 無し | 意図した違い（仕様書:38） | configsave:1106-1108 |
| ウインドウモード | system.screenmode（0=FULL 1=WINDOW）。**既定 0＝フルスクリーン** | `fullscreen`。**既定 false＝ウインドウ** | **違う**（既定値が逆。仕様書:299 で未決） | configsave:953, Config/optionstr.csv `#SCREEN` / config.h:58, launcher.cpp:465 |
| ウインドウサイズ X/Y | windowsize_x/y（既定 640×480） | `window_w/h`（既定 0＝解像度と拡大率で決める） | 違う（既定値） | configsave:980-981 / config.h:61, app.cpp:117 |
| データベース自動更新 | autoreload（0=手動のみ F8、1=開いたフォルダ、**2=起動時（既定）**） | `db_update`（**0=起動時（既定）**、1=手動 F5）。「開いたフォルダ」は灰色 | 違う。①値の番号が違う ②開いたフォルダ方式が無い ③**F5 は全フォルダを読み直す**（LR2 の F8 は今のフォルダだけ）のに画面には「F5で現在選択中のフォルダの更新」と書いてある | configsave:965, songmanage:2820, Songselect:1951 / config.h:63, launcher.cpp:473-475, select_scene.cpp:1283,378 |
| LR2IR に参加 / ライバル取得 / 自動更新 / マイページ | network.lr2ir / getrival / autoupdate（既定 0） | 灰色 | 意図した違い | configsave:1111-1114 / launcher.cpp:476-480 |
| LR1/em IR、mail | lr1ir / lr1id / lr1pass / mail | 項目が無い | 意図した違い | configsave:1109-1115 |
| 曲データベースの再作成 | SETUP のボタン | RD タブに移してある | 同じ（置き場所だけ違う） | H help/8.html / launcher.cpp:609 |
| Language | system.language（SETUP の表示言語だけ） | 帯に文字を出すだけ | RDに無い（影響なし） | configsave:979, history.txt beta2 081203 / launcher.cpp:430 |

### 3.2 JUKEBOX1 / JUKEBOX2

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 曲検索フォルダ | `<jukebox><path>`（複数。末尾に /） | `song_dir`（複数。末尾の / は外す） | 同じ | configsave:28-54 / config.cpp:33, launcher.cpp:481-492 |
| 特殊フォルダ 8 種（RANDOM / FAVORITE / TOP10 / LEVEL / CLEAR / RANK / IGNORE / INSANE） | system.customfolder（ビット。既定 0） | 灰色 | RDに無い | configsave:966, songmanage:2702-2741 / launcher.cpp:495-497 |
| CLOSE RANDOM SELECT FOLDER | select.randomclose（既定 0） | 無し（画面にも無い） | RDに無い | configsave:997 |
| 検索最大ヒット数 | select.searchmax（既定 1000、0 で無制限） | `search_max` 1000 | 同じ | configsave:986 / config.h:68, select_scene.cpp:972 |
| タイトルのフラッシュ時間 | system.titleflash（既定 24。0 で NEW SONG フォルダも出ない） | 灰色 | RDに無い | configsave:963 / launcher.cpp:501-503 |
| 新曲のインストール先 | system.newsongfolder（既定 "NEW SONG/"） | 無し | RDに無い | configsave:961 |

### 3.3 OPTION タブ

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| HI SPEED MIN/MAX/MARGIN | play.hsmin 10 / hsmax 900 / hsmargin 10 | `hs_min/max/margin` 10/900/10 | 同じ | configsave:1061-1063 / config.h:27 |
| 基本スクロールスピード | play.basespeed 100 | `base_speed` 100 | 同じ | configsave:1081 / config.h:65, skin_renderer.cpp:252 |
| レーンカバーの変化間隔 | play.shuttermargin 10 | `shutter_margin` 10 | 同じ | configsave:1066 / config.h:32 |
| ミスBGA表示時間 | play.poorbga 500ms | `poor_bga_ms` 500 | 同じ | configsave:1075 / config.h:66 |
| 最低連打間隔 | system.inputinterval 16ms | `input_interval` 16 | 同じ | configsave:970 / config.h:67 |
| 選曲リスト速度（最初／以降） | select.speedfirst 300 / speednext 70 | 300 / 70 | 同じ | configsave:989-992 / config.h:71 |
| 画像フォントを無効にする / 無効時のフォント | skin.disableimagefont=0 / fontname | 灰色 | RDに無い | configsave:1100-1101 / launcher.cpp:530-533 |
| 選曲にポプコンを使う | select.control=0 | 灰色 | 意図した違い（9K は非目標。仕様書:34） | configsave:995 |
| フォルダランプ | select.folderlamp=0 | 灰色 | RDに無い | configsave:1011 |
| 1/3キーを選曲に割り当てる | select.buttonselect=0 | 灰色 | RDに無い | configsave:996 |
| システムキーの無効化 | system.disablesystemkey=0 | 灰色 | RDに無い | configsave:972, Main.cpp:536 |
| システムログの出力 | system.outputlog=0 | 灰色 | RDに無い（影響なし） | configsave:977 |
| スキンセレクトでプレビューを表示しない | system.disableskinpreview=0 | 灰色 | RDに無い | configsave:973, Scene07_Skinselect.cpp:102 |
| 暗転中に曲のロードを行う | system.disablebmsthread=0 | 灰色 | RDに無い（読み込みの仕組みが違い影響は小）[?] | configsave:976, LR2_bmsload.cpp:4111 |
| 瞬殺/esc のスコアも IR 登録 | play.gomiscore=0 | 灰色 | 意図した違い（IR） | configsave:1082 |
| 右クリックで曲を終了させない | play.disableleftclickexit=0。**既定では右クリック長押しで終了する** | 灰色のチェック無し（＝終了するように見える）。しかし**実際には右クリックで終了しない** | **違う**（挙動と表示の両方） | configsave:1083, Scene04_Play.cpp:1197 / launcher.cpp:542, play_session.cpp:626-665 |
| 上下キーを速度変更に割り当てる | play.disablecurspeedchange=0（既定で割り当てる） | 常に割り当てる（灰色・チェック有り） | 同じ（切り替えはできない） | configsave:1084, LR2_gameloop.cpp:476 / launcher.cpp:543, play_session.cpp:680 |
| 旧版の項目：MULTI THREADING JUDGEMENT / フォルダのロード中は画面を停止 | system.thread / disablefolderthread | 無し | RDに無い（100201 の画面には無い旧版項目 [?]） | configsave:1013,975 |
| 隠しキー：mainsleep / bmssleep / eventmode / directdraw | 3 / 3 / 0 / 0 | 無し | RDに無い（影響なし） | configsave:958,967-968,978 |

### 3.4 SELECT タブ

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 難度変更の挙動を beta1 に戻す | select.difficultychangetype=0 | 灰色 | RDに無い | configsave:1010 / launcher.cpp:546 |
| 鍵盤数 ALL / SINGLE / DOUBLE を使用しない | ignorekeyall / single / double | 灰色 | RDに無い（DOUBLE は意図した違い） | configsave:998-1000 |
| DP 譜面は無視 / 9ボタン譜面は無視 | ignoredp / ignorepms | 灰色 | 意図した違い（もともと一覧に出さない。仕様書:52） | configsave:1001-1002 |
| 難度 ALL を使用しない | ignoredifficultyall | 灰色 | RDに無い | configsave:1003 |
| 7鍵があるとき 5鍵を無視 | ignore5key | 灰色 | RDに無い（5K は仕様書:51 で未決） | configsave:1004 |
| 難度フィルタを使用しない | disabledifficultyfilter | 灰色 | RDに無い | configsave:1012 |
| レベルバー最大値 5/10KEYS | levelbarflash_5=9 | `levelbar_5`=9 を**保存するが、どこからも読まない** | **違う**（設定が効かない） | configsave:1006, skinobject:239,2294 / config.h:69, select_context.cpp:115,138,294 |
| 〃 7/14KEYS | levelbarflash_7=12 | `levelbar_7`=12。**どの鍵盤数でもこれを使う** | 7K では同じ | 同上 |
| 〃 9BUTTONS | levelbarflash_9=42 | `levelbar_9` も読まない | 違う（9K は非目標） | 同上 |
| 選曲中プレビューを再生する | select.preview=1 | `preview`=1。**RD タブに置いてある** | 同じ（置き場所が違う） | configsave:1008 / launcher.cpp:653 |
| 選曲バーにサブタイトルを表示しない | select.disablesubtitle=0 | **SELECT タブに項目が無い**（灰色でも出していない） | RDに無い | configsave:1009, LR2_skindraw.cpp:1010 |

### 3.5 SYSTEM タブ（`<system>` / `<sound>`）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 16bit モードで起動 | system.highcolor=0 | 灰色 | RDに無い（影響なし） | configsave:971 |
| 垂直同期 | system.vsync=0 | `vsync`=false | 同じ | configsave:957 / config.h:56, app.cpp:70 |
| 高精度なタイマー | system.hptimer=0 | 灰色で**チェック有り表示** | 違う（表示だけ。RD は常に高精度） | configsave:974 / launcher.cpp:585 |
| 利用するディスプレイ | system.maindisplay=0 | 画面に無い | RDに無い | configsave:959, Main.cpp:499 |
| ソフトウェア描画 | system.softwarerendering=0 | 画面に無い | RDに無い（影響なし） | configsave:964 |
| OUTPUT TYPE / PLAYBACK DRIVER | sound.output / driver（DirectSound / ASIO / WASAPI）[?] | Windows は `audio_exclusive`（WASAPI 共有／排他）、それ以外は灰色 | 違う（作りが別） | configsave:1016-1017 / config.h:78, launcher.cpp:568-581 |
| バッファ | sound.bufferlength=384 / numbuffers=4 | `audio_buffer`=0（自動：共有 128、排他 64 フレーム） | 違う（単位も既定値も違う。画面の「バッファ数」がどちらのキーかは [?]） | configsave:1014-1015,1050-1052 / config.h:79 |
| FMOD Ex を使用しない | sound.disablefmod=0 | 灰色でチェック有り | 違う（表示だけ） | configsave:1018 / launcher.cpp:584 |
| ウインドウの拡大率（beta2 SETUP） | system.screenexrate=100 | `window_scale`=0（SD スキンなら 200%）。画面には無い | 違う [?] | configsave:969, H help/8.html / config.h:59, app.cpp:121-127 |

### 3.6 遊ぶときのオプション（`<play>`。LR2 は選曲画面のパネルで変える）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| hs | 100 | `hi_speed` 100 | 同じ | configsave:1055 / config.h:22 |
| hstype（HS-FIX） | 0=OFF。0〜4（5 MAIN BPM は [EXT]） | `hs_fix` 0〜5 | 同じ（5 は OpenLR2 拡張に合わせた） | configsave:1060, structure.h:250 / config.h:30 |
| gauge | 0=GROOVE（0〜5） | `gauge` 0〜5 | 同じ | configsave:1057 / config.h:51 |
| random | 0=OFF（0〜5） | `random` 0〜5 | 同じ | configsave:1058 / config.h:49 |
| effect | 0（OFF / HID / SUD / HID+SUD） | `effect` 0〜3 | 同じ | configsave:1059 / config.h:47 |
| autoscratch | 0 | 無い（AUTO SCRATCH が無い）。代わりに `seven_key`（7k モード。LR2 からは取り込まない） | 違う | configsave:1068 / config.h |
| autokey | 0 | 無し | RDに無い [?] | configsave:1069 |
| shuttertype / shutter | 0 / 0（0〜100） | `lane_cover` / `lane_cover_v` | 同じ | configsave:1064,1070, skinobject:2564 / config.h:32 |
| autojudgeadjust | 0（OFF / ON / SILENT） | `auto_adjust` 0〜2 | 同じ | configsave:1072 / config.h:25 |
| judgetime | 0（±199）。ずれるのは描画の時刻だけ | `judge_timing` ±199、描画だけずらす | 同じ | configsave:1073, Scene04_Play.cpp:398 / play_session.cpp:1009 |
| bga | 1（OFF / ON / AUTOPLAY / NO MISS） | `bga` 1（0〜3） | 同じ | configsave:1074 / config.h:45 |
| bgasize | 0（NORMAL / EXTEND） | 無し。表示は常に NORMAL | RDに無い | configsave:1076, skinobject:112 / select_context.cpp:234 |
| ghost（位置 TYPE A〜C） | 0=OFF | 無し。表示は常に OFF | RDに無い | configsave:1077, skinobject:126 / select_context.cpp:72,231 |
| scoregraph | **0=OFF** | **常に ON**（op 39 を常に真）。切り替えられない | **違う** | configsave:1078, skinobject:139 / select_context.cpp:78,230 |
| defaulttarget | 90（50〜100） | 90 で固定（保存しない） | 違う（設定できない） | configsave:1079, skinobject:3463 / play_session.cpp:366 |
| target | 0（0〜8） | 無し。"NO TARGET" で固定 | RDに無い [?] | configsave:1080, Scene04_Play.cpp:1999 / select_context.cpp:209 |
| replaysave | 0（OFF / ALWAYS / HISCORE / CLEAR / FULLCOMBO） | 無し。表示は OFF | RDに無い | configsave:1067 / select_context.cpp:239 |
| battle / flip | 保存しない | 無し | 意図した違い（DP は非目標） | — |
| select.sort / key / difficulty | 0 / 1 / 0 を**保存して次回に戻す** | 選曲画面の中だけで持ち、保存しない | 違う（起動し直すと戻る） | configsave:982-984 / select_scene.cpp:435-438 |

### 3.7 `<sound>` のエフェクター、`<skin>`、`<course>`、`<tools>`

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 音量（volumeflag / key / bgm / master） | 1 / 100 / 100 / 100 | 無し | RDに無い | configsave:1019-1022 |
| EQ / ピッチ / FX ×3 | 既定は全部 0 | `eq_on`・`eq_0`〜`eq_6`・`pitch_on`・`pitch`・`pitch_type`・`fx0_on`/`_type`/`_target`/`_p1`/`_p2`（〜`fx2_`）。既定は全部 0 | 直した | configsave:1023-1048 / config.h:75, config.cpp:103-115 |
| skin.play_7 | "" | `play_skin` | 同じ | configsave:1085 / config.h:12 |
| skin.play_5 / 14 / 10 / 9、play_*_b | "" | 無し | 意図した違い（7K+S 専用） | configsave:1086-1089,1097-1099 |
| skin.select / decide / result / soundset | "" | `select_skin` / `decide_skin`（"none" で決定画面を出さない）/ `result_skin` / `sound_set` | 同じ（"none" は RD の追加） | configsave:1090-1095 / config.h:13-17 |
| skin.keyconfig / skinselect / theme | "" | 無し | RDに無い（theme は LR2 でも廃止済み） | configsave:1093-1096 |
| スキンのカスタマイズ | スキンごとに別の xml | `skin_custom`（config.txt にまとめる） | 違う（保存場所・形式） | configsave:746-934 / config.cpp:78 |
| course.*（ランダムコースの条件 13 個） | stage 5、bpmrange 10 ほか | 無し | RDに無い | configsave:1118-1130 |
| tools.*（BMSE / lame / ogg / 動画など 15 個） | bmse.exe、lame.exe、"-q 6" など | 無し。動画作成ボタンは灰色 | RDに無い | configsave:1131-1145 / launcher.cpp:705 |
| keyconfig*.xml / midi.xml | 別ファイル | `keyconfig.txt`（LR2 からは引き継がない） | 違う（形式が別） | configsave:56,97 / src/input/keyconfig.cpp:157 |

### 3.8 OpenLR2 の拡張（`openlr2-config.xml`）[EXT]

| 項目 | OpenLR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| resolution / fullscreenfilter / fullscreenfitstretch | 0 / 0 / 0 | `skin_res`（"" / sd / hd / fhd）だけ | 意図した違い（仕様書 7 章） | configsave:1158-1160 / config.h:18 |
| screenmode=2（ボーダーレス） | 0 | 無し（fullscreen は実質ボーダーレス。4.4 参照） | 未決（仕様書:299） | configsave:1161 |
| gaugeautoshift | 0 | `gas` | 意図した違い（仕様書:27） | configsave:1165 / config.h:52 |
| newrandomrestart | 1 | 無し | LR2に無い（OpenLR2 拡張） | configsave:1166 |
| lifttype / lift | 0 / 0 | `lift` / `lift_v` | 意図した違い | configsave:1167-1171 / config.h:40-41 |
| skin.courseresult | "" | `courseresult_skin` | 同じ | configsave:1173 / config.h:14 |
| display_ir | "" | 無し | 意図した違い | configsave:1175 |

### 3.9 RD だけの項目

| 項目 | RD | 判定 | 出典 |
|---|---|---|---|
| lr2_root（素材のフォルダ） | "" | LR2に無い | config.h:11, launcher.cpp:615 |
| table（難易度表の URL、複数） | 空 | LR2に無い（難易度表中心の方針） | config.h:21, launcher.cpp:643-652 |
| hs_mode / fhs_green / fhs_base / gear_mode | 0 / 300 / 3 / 0 | LR2に無い・意図した違い（仕様書 6 章） | config.h:34-38, launcher.cpp:659-671 |
| ln_mode | 0（LN） | LR2に無い・意図した違い（仕様書:196） | config.h:43 |
| fps | 0（モニタに合わせる） | LR2に無い（仕様書:290） | config.h:55 |
| joystick_directinput | false（Windows だけ） | LR2に無い | config.h:75 |

## 4. 音声・画面・入力・スクリーンショット

~~この章でいちばん大きい点：RD の自前ミキサーには音量・イコライザ・ピッチ・エフェクタが一切無い。~~ 音量（MASTER/KEY/BGM）、イコライザ、ピッチ、エフェクタ 3 つを足した（直した。4.3 参照）。

### 4.1 サウンドセット（.lr2ss）の読み込み

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 対応している命令 | SELECT / DECIDE / EXSELECT / EXDECIDE / FOLDER_OPEN / FOLDER_CLOSE / PANEL_OPEN / PANEL_CLOSE / OPTION_CHANGE / DIFFICULTY / SCREENSHOT / CLEAR / FAIL / STOP / MINE / SCRATCH / COURSECLEAR / COURSEFAIL の 18 種 | 同じ 18 種 | 同じ | O LR2_audio.cpp:428-499 / B src/audio/system_sound.cpp:20-26 |
| 同じ命令が何度も出たとき | 最初に読めたものを使う | 同じ | 同じ | O LR2_audio.cpp:428-499, history.txt:536 / B system_sound.cpp:70,83 |
| 音が見つからないとき | wav と ogg を入れ替えて探す | 同じ | 同じ | O En_audio.cpp:231 / B system_sound.cpp:28-39 |
| `#` で始まらない行（同梱 lr2ss 末尾の「保険」の行） | 読まない | `#` を外して命令として読む | 違う [?]（実物の LR2 では未確認） | O LR2_audio.cpp:356 / B system_sound.cpp:55 |
| #CUSTOMFILE の選択 | ユーザーの選択（SkinCustomize の xml）を使う。RANDOM にも対応 | 常に既定値（4 列目）を使い、ユーザーの選択を読まない | 違う | O LR2_audio.cpp:334-335,401-411 / B system_sound.cpp:64-80, src/app/app.cpp:104-112 |
| #CUSTOMOPTION と #IF/#ELSEIF/#ELSE/#ENDIF | 対応 | 無い | RDに無い | O LR2_audio.cpp:360-399 |
| #CUSTOMFOLDER と #INCLUDE | 対応 | 無い | RDに無い | O LR2_audio.cpp:412-419,500-502 |
| パスの `*` を毎回ランダムに選ぶ | 選ぶ | #CUSTOMFILE で置き換えたときだけ。ファイル名の `*` は通らない | 違う | O LR2_audio.cpp:429 / B system_sound.cpp:81, src/skin/lr2_skin.cpp:615-641 |
| ループする音 | SELECT・EXSELECT・**COURSECLEAR・COURSEFAIL** | SELECT だけ | 違う | O LR2_audio.cpp:429,485,493,497 / B play_session.cpp:1200 |
| 出力先のグループ | SELECT/DECIDE/CLEAR/FAIL/STOP/COURSE* は BGM 側、パネル・フォルダ・オプション・皿・地雷・スクショは KEY 側 | グループも音量も無い | RDに無い | O Main.cpp:1162, Scene05_Result.cpp:23, Scene02_Songselect.cpp:2539 |

### 4.2 システム音を鳴らすタイミング

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| SELECT | 選曲画面に入ったとき、ループ | 同じ | 同じ | O Main.cpp:1159-1162 / B select_scene.cpp:576,838 |
| EXSELECT / EXDECIDE | EX モード（2 鍵の長押しで切り替え）で SELECT/DECIDE と差し替え | 読み込むだけ。EX モード自体が無い | RDに無い | O Scene02_Songselect.cpp:1330-1346, Main.cpp:1202-1205 |
| プレビュー中に BGM を絞る | 1000ms かけて 0.025 倍まで下げる | 同じ | 同じ | O En_audio.cpp:383-395, Scene02_Songselect.cpp:1710,2733 / B select_scene.cpp:1430,698 |
| DECIDE | 決定画面 | 決定画面。決定スキンが無いときはプレイ開始時 | 同じ | O Main.cpp:1205 / B decide_scene.cpp:57, play_session.cpp:234 |
| SCRATCH | 選曲バーが動いたとき | 同じ | 同じ | O Scene02_Songselect.cpp:2539 / B select_scene.cpp:737 |
| FOLDER_OPEN / CLOSE、DIFFICULTY | フォルダの出入り。難度を変えたら DIFFICULTY（無ければ FOLDER_OPEN） | 同じ | 同じ | O Scene02_Songselect.cpp:2985-2989,3040 / B select_scene.cpp:801,907,930 |
| PANEL_OPEN / CLOSE、OPTION_CHANGE | パネルの開閉と各種ボタン | 同じ | 同じ | O Scene02_Songselect.cpp:1897,1938 / B select_scene.cpp:744,662 |
| MINE | 地雷。#WAV00 があればそちら | 同じ | 同じ | O Scene04_Play.cpp:761-764 / B src/game/judge.cpp:237, play_session.cpp:849 |
| STOP（閉店） | キー音を止めてから鳴らす | 同じ | 同じ | O Scene04_Play.cpp:1594-1604 / B play_session.cpp:946-950 |
| CLEAR / FAIL | クリア種別が 2 未満なら FAIL | 同じ | 同じ | O Scene05_Result.cpp:22-27 / B play_session.cpp:459 |
| COURSECLEAR / COURSEFAIL | コースのリザルト（ループ） | 1 回だけ | 違う | O Scene13_Courseresult.cpp:182-184 / B play_session.cpp:1200 |
| SCREENSHOT | F6 で保存したとき | 読み込むだけで鳴らさない | RDに無い | O Main.cpp:2418 |
| リザルトを抜けるときの音のフェードアウト | スキンの fadeout の時間で全体を絞る | 無い（全体の音量が無い） | RDに無い | O Scene05_Result.cpp:286,299, En_audio.cpp:1015-1032 |

### 4.3 音量スライダー・EQ・ピッチ・エフェクタ

（スライダー番号は LR2 の slider.txt / OpenLR2 の定義による。依頼文の「10-19」は EQ 10-16・音量 17-19 にあたる）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| スライダー 10〜16 | イコライザ 7 バンド、±12 dB | 同じ（表示。ドラッグは選曲画面の側） | 直した | O LR2_skinobject.cpp:2610-2651 / B select_context.cpp:322, src/render/skin_play.cpp:361 |
| スライダー 17・18・19 | MASTER・KEY・BGM の音量、0〜100 | 無い | RDに無い | O LR2_skinobject.cpp:2652-2669 |
| スライダー 20〜25 | エフェクタ 3 つ × パラメータ 2 つ、0〜100 | 同じ（表示） | 直した | O LR2_skinobject.cpp:2670-2705 / B select_context.cpp:323-327, skin_play.cpp:364-368 |
| スライダー 26 | ピッチ ±12 半音 | 同じ（表示） | 直した | O LR2_skinobject.cpp:2706-2711 / B select_context.cpp:328, skin_play.cpp:369 |
| 数値 50〜66・ボタン 20〜33 | 50〜56 EQ、57〜59 音量、60〜65 FX のパラメータ、66 ピッチ。ボタン 20〜22 FX の種類、23〜25 FX のオンオフ、26〜28 掛け先、29 EQ のオンオフ、30 EQ のプリセット（OpenLR2 でも未実装）、31 音量のオンオフ、32 ピッチのオンオフ、33 ピッチの方式 | 同じ（値。30 は 0 のまま。クリックは選曲画面の側） | 直した | O LR2_skinobject.cpp:1723-1755,3134-3285 / B select_context.cpp:197-206,284-291, skin_play.cpp:198-203,406-411 |
| FX の種類を変えたときのパラメータ | LPF なら 100 / 0、それ以外は 0 / 0 に戻す | 同じ（`SoundFx::set_fx_type`） | 直した | O LR2_audio.cpp:221-230 / B src/audio/dsp.cpp:37 |
| 音量の掛け方 | BGM 側 = BGM×MASTER×フェード、KEY 側 = KEY×MASTER×フェード | ゲインの処理が無い | RDに無い | O En_audio.cpp:740-747 / B mixer.cpp:242-339 |
| イコライザの中身 | 63 / 160 / 400 / 1k / 2.5k / 6.3k / 16k Hz のピーキング（Q=1.5）を全体に。既定はオフ | 同じ（自前の 2 次 IIR。FMOD の EQ と特性は少し違う [?]） | 直した | O En_audio.cpp:833-882 / H help/4.html / B src/audio/dsp.cpp:59,112, mixer.cpp:385 |
| ピッチの方式 | FREQ（回転数）、PITCH（音程だけ）、SPEED（速さだけ）。FREQ と SPEED で 0 以外ならスコアを保存しない（history）。OpenLR2 は**遅くしたときだけ**保存しない | FREQ は全部の音を読む速さを変え、曲の時計も同じ倍率で進む。PITCH・SPEED は全体の後ろで遅延線 2 本のクロスフェードで音程を変える（音は粗い。平均 20ms 遅れ、その分は曲の時計で差し引く）。保存しない判定は `AppConfig::pitch_changes_speed()`（0 以外）を用意しただけで、まだ使っていない | 直した（保存の判定はまだ。速くしたときに保存するかは history と OpenLR2 で食い違う [?]） | O En_audio.cpp:758-831, LR2_bmsload.cpp:2115-2124,3336, LR2_statplay.cpp:791 / H history.txt:912-915 / B dsp.cpp:25-35,317, mixer.cpp:178,328 |
| エフェクタの種類と掛け先 | 0 OFF・1 リバーブ・2 エコー・3 LPF・4 HPF・5 フランジャー・6 コーラス・7 ディストーション。掛け先は MASTER/KEY/BGM。パラメータの換算（リバーブ 残響 p1×200ms・返り 10log10(p2/100) dB、エコー 遅れ p1×10ms・返り同じ、LPF 22000/1.06^(100-p1) Hz、HPF 22×1.06^p1 Hz、どちらも共振 p2×0.09+1、フランジャー 深さ p1/100・速さ 0.02×1.06^p2 Hz、コーラス 速さ p1/5 Hz・深さ p2、ディストーション 強さ p1/100） | 同じ種類・掛け先・換算。中身は自前（リバーブはくし形 8 本＋全域通過 4 本、エコーは返り 50%、フランジャー・コーラスは揺らした遅延線を半々に混ぜる）で、FMOD と同じ音ではない | 直した（音色は違う） | O En_audio.cpp:891-999 / H help/4.html / B dsp.cpp:166-315, mixer.cpp:377-384 |
| MIDI のツマミへの割り当て | midi.xml の S01〜S39 → スライダー | 無い | RDに無い | O LR2_audio.cpp:9-206, LR2_configsave.cpp:107-140, Config/midi_def.xml |
| リプレイへの記録 | エフェクタの変化もリプレイに残す | 無い | RDに無い | O LR2_skinobject.cpp:2611-2711 / H history.txt:885 |
| コースのつなぎのピッチ | stagePitch で合わせる | 無い | RDに無い | O En_audio.cpp:749-754, LR2_bmsload.cpp:2431 |

### 4.4 音声エンジン（FMOD と miniaudio）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| ライブラリ | FMOD Ex。OpenLR2 は FMOD の新 API で書き直し | miniaudio と自前ミキサー | 意図した違い | 仕様書:67 / B mixer.cpp:20-60 |
| 出力の種類 | DirectSound / WASAPI / ASIO とドライバを選べる。OpenLR2 は DirectSound を WASAPI に読み替え [EXT] | WASAPI 共有／排他だけ（失敗したら共有）。ASIO もデバイス選択も無い | 違う | H help/8.html, O En_audio.cpp:1068-1101 / B mixer.cpp:23,34-38, config.h:76-79 |
| バッファ | bufferlength=384 × numbuffers=4 | 周期は共有 128／排他 64 フレーム（`audio_buffer`）。個数は miniaudio の低遅延プロファイル任せ [?] | 違う | O LR2_configsave.cpp:1014-1015, En_audio.cpp:1064 / B app.cpp:85, mixer.cpp:27-28 |
| サンプルレート・チャンネル | OpenLR2 はドライバの値（無ければ 48000、ステレオ）。FMOD Ex 実機は [?] | デバイスの既定値、float32、ステレオ。キー音は読み込み時に変換 | ほぼ同じ [?] | O En_audio.cpp:1096-1106 / B mixer.cpp:24-26, sound_bank.cpp:46-47 |
| 同時発音数 | 256 | 256 | 同じ | O En_audio.cpp:1066,1108 / B mixer.h:127 |
| 同じ WAV を鳴らし直したとき | 前の音を止めて鳴らし直す | 同じ | 同じ | O En_audio.cpp:676-679 / B mixer.cpp:177-180 |
| 256 音が埋まったとき | FMOD の優先度で止める [?] | 一番長く鳴っている音を止める | 違う [?] | B mixer.cpp:188-193 |
| LN を早く離したとき | そのキー音を止める | 同じ | 同じ | O Scene04_Play.cpp:1092-1095 / B play_session.cpp:847 |
| BGM を鳴らす時刻 | メインループでフレーム単位 | 予定表から 1 サンプル単位 | 意図した違い | O Scene04_Play.cpp:1389 / B mixer.cpp:310-316, 仕様書:99-104 |
| 曲の時計 | タイマー（ms） | 音声デバイスの時計をならし、出力遅延を実測して引く | 意図した違い | B mixer.cpp:89-116,155-167,221-240, 仕様書:175 |
| 遅れの検出 | muon.wav を 5 秒ごとに鳴らす [?] | 無い | RDに無い（低） | O Scene04_Play.cpp:1351-1361 |

### 4.5 画面

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 画面モード | 0=フルスクリーン（排他）、1=ウインドウ。2=ボーダーレスは [EXT] | `fullscreen` の真偽値だけ。実際はデスクトップ解像度のボーダーレス | 違う（仕様書では選べる予定、既定は未決） | O Main.cpp:190-222, LR2_configsave.cpp:953,1161 / B app.cpp:71, 仕様書:297-299,592 |
| 解像度 | スキンの解像度（history.txt:12「フルスクリーンは 640x480 のみ」）。resolution は [EXT] | スキン解像度で描き、縦横比を保って拡大（`skin_res`） | 意図した違い | O Main.cpp:470-471 / B app.cpp:116-144, 仕様書:282-288 |
| ウインドウの大きさ | screenexrate=100%、640x480 | window_scale（0 なら SD スキンは 2 倍） | 違う | O LR2_configsave.cpp:969,980-981 / B app.cpp:116-131 |
| VSYNC | ON/OFF。判定タイミングに影響する。OpenLR2 は本物の VSYNC を使わず待つ間も入力を回す [EXT] | `vsync`（既定 OFF）。判定は入力の時刻で行うので影響しない | 意図した違い | H help/8.html, O Main.cpp:2402-2409 / B config.h:53-56, app.cpp:70, 仕様書:290-295 |
| fps の上限 | VSYNC 任せ（mainsleep=3）[?] | プレイは `fps`（0=モニタ、-1=無制限、指定値）を守る。選曲は 4ms、決定は 2ms 待つだけ | LR2に無い | B play_session.cpp:331-332,990-994, select_scene.cpp:1784, decide_scene.cpp:132 |
| 表示方式（VSYNC / IMMEDIATE / MAILBOX） | 無い | ON/OFF だけ（仕様書では 3 択の予定） | 仕様との差 | 仕様書:300 |
| F4（ウインドウ切り替え） | 選曲画面だけ | 選曲に加えプレイでも効く。ボタン 80 でも切り替え | 違う（RD が広げている） | O Main.cpp:2523 / H history.txt:601 / B select_scene.cpp:1300-1303,1204-1206, play_session.cpp:673-675 |
| F7（FPS 表示） | ある | 無い | RDに無い | O LR2_gameloop.cpp:499-501 / H help/3.html |
| 16bit 表示・maindisplay | ある | 無い | RDに無い（低） | O LR2_configsave.cpp:959,971 |

### 4.6 入力

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| キーコンフィグのファイル | `LR2files/Config/keyconfig.xml`（CP932 XML。`<keyNN><id>番号</id>…`）。5 鍵は `_5`、9 ボタンは `_p` | 設定フォルダの `keyconfig.txt`（`key1 = key:Z \| joy:button 3 \| joy2:button 3 \| midi:note 41`）。`joyN:` は N 台目のパッドだけ、`joy:` はどの台でも。MIDI は `midi:note 番号`・`midi:bend -`／`+`・`midi:pedal`。前の形式のファイルはそのまま読める。LR2 の XML は読まない | 違う | O LR2_configsave.cpp:59-90, LR2_skinobject.cpp:3645-3651 / B src/input/keyconfig.cpp:104-160 |
| 入力元の番号 | 0〜255 = キーボード（DIK）、256〜 = パッド（32 ずつ）、0x200〜 = MIDI ノート、0x300/0x301 = ピッチベンド −/+、0x302 = ペダル | SDL スキャンコード、パッドはボタン 64・軸 8・ハット 4 を「どの台でも」の分と台 1〜8 の分、MIDI ノート 128・ベンド −/+・ペダル | 違う（番号の振り方だけ。区別できるものは同じ） | O En_input.cpp:100-131,266-333 / B src/input/input.h:40-48 |
| 割り当てられるボタン | 1P/2P の鍵盤 7・皿 2 方向・START・SELECT（key01-13, 21-33）と 9 ボタン用 | 1P の 11 個だけ（キーコンフィグの画面で START・SELECT も割り当てられる） | 意図した違い（DP・9K は対象外） | O LR2_configsave.cpp:67-90 / B keyconfig.h:12-14, 仕様書:34,52 |
| 1 ボタンに割り当てられる数 | 8 個（help）。OpenLR2 は 16 [EXT] | キーコンフィグの画面からは 8 個（スロット）。ファイルと一覧画面では制限なし（9 個目以降は文字 48-55 に出るが選べない） | 直した（画面）／LR2に無い（ファイル） | H help/5.html / B kc `kKeySlots` |
| 既定の割り当て | Z S X D C F V、皿は LCtrl/Space と LShift、Q/W。パッドは B4 B7 B3 B8 B2 B5 と左、皿は下/上、B9/B10。MIDI は鍵盤 41〜47・53〜59、皿 36/38/48/50/ベンド− と 40/52/ベンド+、START 39/51、SELECT 37/49 | 同じ（パッドの既定は台を決めない。並びはキーボード・パッド・MIDI の順） | 直した（MIDI の既定を足した） | Config/keyconfig_def.xml / B keyconfig.cpp:78-102 |
| ジョイスティック | DirectInput。軸はしきい値で方向ボタン扱い。台ごとに番号を分ける（keyconfig_def.xml の 1P は 0x100〜、2P は 0x120〜。OpenLR2 は 1 台目を 0x120 に入れていて実物と合わない [?]） | SDL と Windows Raw Input。軸のしきい値 16384 [?]。台に番号（1〜8）を振り、台を決めた割り当てはその台だけに効く。台を決めない割り当て（`joy:`）は前と同じく全部の台をまとめる。番号は見つけた順（起動時は OS の並び）で空いている一番小さいもの。抜くと空く | 直した（番号の付け方は OS の並び任せ。台を決めない割り当ても残している） | O En_input.cpp:264-271, Scene06_Keyconfig.cpp:639-664 / H keyconfig_def.xml / B input.cpp `pad_number`, win_rawhid.cpp |
| アナログ皿 | 無い（help/8.html：止めても回転信号が続くのはソフトで対処できない） | `analog_axis`。止まって `analog_release_ms`（100ms）で離す | LR2に無い（RD の拡張） | H help/8.html / B keyconfig.h:25-28, input.cpp:89-107,157-162 |
| 皿の 2 方向をまとめる規則 | どちらかが新しく入れば押下、両方離したら離す | 同じ | 同じ | O En_input.cpp:444-465 / B input.cpp:148-153 |
| 最低連打間隔 | 16ms。すぐ離した分はその時間まで押したまま | 同じ | 同じ | O En_input.cpp:274-286 / B input.cpp:109-135 |
| OS のキーリピート | 使わない | repeat イベントを捨てる | 同じ | B input.cpp:54 |
| 入力の時刻 | メインループで状態を読んだ時刻 | イベントの ns 時刻 | 意図した違い | O En_input.cpp:170-333 / 仕様書:172-175 |
| 選曲の送り | 押した瞬間 300ms、押し続けると 70ms ずつ。key10 で上、key11 で下、↑↓も同じ | 同じ | 同じ | O Scene02_Songselect.cpp:2024-2066 / B select_scene.cpp:1369-1381 |
| 鍵盤で選曲する設定（buttonselect / control） | 2・4 鍵または 5・7 鍵で送る | 無い | RDに無い（低） | O LR2_configsave.cpp:1002-1003, Scene02_Songselect.cpp:2024-2036 |
| MIDI 鍵盤・ツマミ | 入力機器を全部（15 台まで）開き、機器もチャンネルも区別しない。ノートオン（ベロシティ 0 はオフ）／オフ、ピッチベンドは上位 7 ビットが 64 未満で −・超で +・64 で両方離す、CC 64（ペダル）は 64 未満で離す・超で押す。CC はスライダーにも使う（midi.xml） | 鍵盤・ベンド・ペダルは同じ読み方（台数の上限は無い。macOS は CoreMIDI でつないだ・抜いたも追う、Windows は winmm で 2 秒ごとに台数を見て開き直す）。時刻は届いた瞬間（macOS は CoreMIDI の時刻）。LR2 はフレームごとに状態を読むので 1 フレーム内の押して離すを落とすが、RD は落とさない。CC→スライダーは無い | 直した（CC のスライダーは RDに無い） | O En_input.cpp:94-131,314-339,467-484 / H keyconfig_def.xml / B src/input/midi.cpp |
| 選曲画面のファンクションキー | F1 説明、F2 変態オプション、F3 レベル、F4 ウインドウ、F5 IR、F6 スクショ、F7 FPS、F8 再読み込み | F1 キーコンフィグ、F2 オート、F4 ウインドウ、F5 更新、**F6 レーンカバー** | 違う | H help/3.html:134-140, O Main.cpp:2210 / B select_scene.cpp:1283-1304 |

### 4.7 スクリーンショット

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| キー | F6。どの画面でも効く | 無い（選曲の F6 はレーンカバー） | RDに無い | O LR2_gameloop.cpp:496-498 / B select_scene.cpp:1295-1299 |
| 形式 | PNG（以前は中身が bmp だったのを修正済み） | 開発用に環境変数を指定したときだけ BMP | RDに無い | H history.txt:164 / B main.cpp:248-255, play_session.cpp:1088-1094 |
| 保存先と名前 | 実物は LR2.exe と同じフォルダ（history.txt:516）。OpenLR2 は `screenshot/LR2 YYYY-MM-DD HH-MM-SS.png`（実物と同じかは [?]） | 無い | RDに無い | O Main.cpp:454,2411-2418 |
| 大きさ | スキンの解像度 | 無い | RDに無い | O Main.cpp:2417 |
| 音 | #SCREENSHOT（シャッター音） | 鳴らさない | RDに無い | H help/3.html:139 |

## 5. スキンの読み込み・描画（命令と意味）

命令を受け付けるかどうかの一覧は `docs/lr2_skin_ids.md` §3 にあるので繰り返さない。この章は各欄の意味と、同文書以降に変わった点を扱う。

この章の略記：`load` = O LR2_skinload.cpp、`draw` = O LR2_skindraw.cpp、`val` = O En_value.cpp、`fu` = O En_fileutil.cpp、`cfgsv` = O LR2_configsave.cpp、`mng` = O LR2_skinmanage.cpp、`sh` = LR2 の skin_history.txt、ほかの `*.txt` = LR2files/スキン関連ドキュメント（行番号は CP932 から変換後）。RD 側は `ls` = src/skin/lr2_skin.cpp、`rd` = src/skin/skin_renderer.cpp。

**lr2_skin_ids.md の監査以降に実装済みになったもの**：決定画面（decide_scene.cpp）、スキンセレクト（skin_select_scene.cpp）、gr100/101 をプレイで読む（play_session.cpp:785-786）、blend 10（rd:716-718）、`#SCENETIME` / `#IGNOREINPUT`（decide_scene.cpp:95-98）。

### 5.0 行・CSV・#IF の扱い

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 文字コード | CP932 に固定 | 自動判定（UTF-8 のスキンも読める） | LR2に無い | load:886 / ls:112 |
| 有効な行 | 先頭が `#` で、**改行を含めて** 7 文字以上 | CRLF・LF・CR のどれでも行に分け、改行を除いて 6 文字以上 | 直した（以前は LF だけのファイルで素の `#ENDIF` が捨てられ、#IF の入れ子が崩れた）。LR2 は最後の行に改行が無いとその行の最後の 1 文字を落とすが、それはまねない | load:868,888-892 / ls `lines_of` |
| 素の `#ELSE`（CRLF） | 6 文字なので無視される（WMIX `play/csv/AC_1p.csv:1453` など 6 ファイル。無害） | 同じく無視 | 同じ | 同上 |
| 行頭の空白 | `#` が 1 文字目でない行は無視（others.txt:32 は「タブは大丈夫」と書く [?]） | 同じ | 同じ | load:891 / ls:245 |
| 命令名の大文字・小文字 | 区別する | 大文字にそろえて比べる | LR2に無い（受け入れが広い） | load:958 / ls:278 |
| CSV | 最大 30 欄。`"…"` の引用符を外す。`!N` は -N | 欄数の上限なし。引用符は外さない。`!` の前の空白を許す | 違う（引用符。実スキンでの使用は 0） | fu:478-535 / ls:27-53 |
| #IF 系 | 欄 1〜9 の AND。負の値と 999 超は偽。入れ子 99 段まで。偽の枝の中の #IF も自分の条件だけで評価される（癖） | 同じ癖まで再現 | 同じ | load:894-954 / ls:250-275 |
| 描画順の番号（sortID） | 処理した行ごとに +1。`#DST_NOTE` は +2、`#DST_BGA`・`#DST_BARGRAPH` は +1 を足す。#INCLUDE は戻り値を二重に足す | 同じ | 同じ | load:1129,1177,1400,1760 / ls:280,318,377,393,420 |

### 5.1 ヘッダ・シーン命令（意味の違いがあるもの）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| `#INFORMATION` の 6・7 欄目、`#RESOLUTION` | 解像度 [EXT] | 読む | LR2に無い [EXT] | mng:246-280 / ls:655-672 |
| `#ENDOFHEADER` | LR2 では走査が止まる。OpenLR2 は 11 文字で比べているので止まらない（不具合） | 止まる | 同じ [?] | mng:390 / ls:651 |
| `#CUSTOMOPTION` の反映 | 保存値のうち **900〜999 だけ** op を立てる | 同じ（900〜999 だけ） | 直した | load:1886-1889 / ls `load_lr2_skin` |
| `#CUSTOMFILE` の候補 | 拡張子あり＝ファイル、なし＝フォルダ。.txt を除く。名前は**最初の `.`** の手前まで。最後に RANDOM を足す | 同じ（フォルダは名前のまま、ファイルは最初の `.` の手前。名前の末尾が txt のものを除く） | 直した | customize.txt:41 / mng:322-336 / ls `custom_file_candidates` |
| `#CUSTOMFILE` の既定値 | 保存値が候補に無ければ第 3 欄（既定）。既定が空なら `*` が残り、読むたびにランダム | 同じ（候補に無い既定は selected = -1 とし、既定の文字列をそのまま使う。スキンセレクトには既定、空なら RANDOM と出す） | 直した。WMIX_Result_HD の clear image など既定が空の項目は、読むたびにランダムになる（以前は先頭に固定） | mng:362-371 / ls `read_skin_header` |
| `#SETOPTION` | 1000 未満を受ける。0〜899 は #IF だけに効く（描画時は実際の状態を見る） | プレイ画面では、知らない 1000 未満の番号に読み込み時の値を返す | 違う（軽微） | load:1790 / src/render/skin_play.cpp:159-160 |
| `#FLIPRESULT` / `#DISABLEFLIP` | 読んだ時点で op350/351 を書き換え、後の #IF に効く。後に書いた方が勝つ。リザルトスキン側の #FLIPRESULT も効く | 同じ。旗は読み込み開始時の op 351（リザルトではプレイスキンの旗）から始め、読んだ順に旗と op 350/351 を書き換える | 直した（LR2 標準リザルトの RESULT SIDE が効く） | load:1720-1734, Main.cpp:1040-1042,1218 / ls `#FLIPRESULT`, play_session.cpp（リザルトに入るところ） |
| `#FLIPSIDE` | BATTLE で 1P/2P の部品・タイマー・ボタンを入れ替える | 無い | 意図した違い（BATTLE は対象外） | load:691-778,1858 |
| `#NOBGA` | BGA を読まない | 無い | RDに無い（実スキンでの使用は 0） | H history.txt:840 / load:1716 |
| `#TEXTMERGIN` | readme の余白（既定 10,10） | 無い | RDに無い | load:1736 |
| `#LR2FONT` の第 2 引数 =1 | 「画像フォントを使わない」設定でも強制的に読む | その設定が無く、常に読む | 同じ（RD は値が固定） | H history.txt:660,673 / load:1686 |
| `#IGNOREINPUT`、`#SAMPLEBMS` | OpenLR2 は解釈しない [?] | IGNOREINPUT はあり、SAMPLEBMS は無い | [?] | decide_scene.cpp:97 |
| `#CUSTOMFOLDER`、`#HORIZONTAL`、`#SRC/DST_MASK`、`_THUMBNAIL`、`#EVENT_*` | [EXT]（THUMBNAIL は LR2 標準 SkinSelect に `#` 抜きの行がある [?]） | 無い | LR2に無い [EXT] | load:1149-1160,1603-1680,1778,1828 |
| `#BAR_STAR`、`#DST_BAR_STAGEFILE` | 資料に記載なし [EXT?] | 読むが描かない | [?] | load:1589,1296 |

### 5.2 SRC の欄

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| gr | 0〜99 は `#IMAGE` の順番（CONTINUE も 1 枠使う）。100 以降は特殊画像（5.6） | 同じ | 同じ | load:960-993 / ls:291-303 |
| x, y < 1 | 0 にする | 同じ | 同じ | load:129-130 / ls:154-155 |
| w, h < 0 | 画像の大きさにする | 同じ。100〜102 で画像がまだ無いときは描くときのテクスチャの大きさ | 意図した違い（LR2 は仮の画像 640x480 / 300x80 の大きさ [?]） | sh:316 / load:127-128,391-394 / rd:124-131 |
| div_x, div_y | 1 未満は 1。1 コマ = w/div（整数）。番号は行優先 | 同じ | 同じ | load:131-155 / rd:98-103 |
| cycle | コマ = (t % cycle) × コマ数 / cycle。cycle ≦ 0 なら 0 コマ目 | 同じ | 同じ | draw:148-161 / ls:812-819 |
| timer（SRC 側が停止中） | 0 コマ目を描く（資料 timer.txt:2 は「表示しない」） | 同じ | 同じ | draw:151 / ls:813 |
| SRC と DST のタイマーが同じ | DST の最初のキー時刻からアニメーションを始める | 同じ。ただし ONMOUSE にも適用（LR2 は ONMOUSE では引かない） | 同じ／ONMOUSE は違う（軽微） | draw:373-378,487-492 / rd:105-110,593 |
| 動画 gr | 切り出しをせず、動画の 1 枚全体を描く | 動画を読まない | RDに無い | load:146-152 |

### 5.3 DST の欄と時間の評価

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 1 行目だけから読む欄 | n、loop、timer、op1〜5 | 同じ | 同じ | load:61-71 / ls:183-189 |
| acc | 0 等速、1 三次の加速、2 三次の減速、それ以外は前のキーの値のまま。前側のキーの acc を使う | 同じ | 同じ | val:28-52 / ls:755-765 |
| 補間する値 | x, y, w, h, a, r, g, b, angle, sortID。blend・filter・center は前のキーの値 | 同じ | 同じ | draw:107-121 / ls:790-803 |
| loop | 負: 最後のキーの後に消える。=最終時刻または 1 キー: 最後の形で止まる。0 ≦ loop < 最終: [loop, 最終) を繰り返す。loop > 最終: 時刻 0 の形で止まる | 同じ。スライダーだけ、戻り先が最初のキーより前なら最後の形で止める | 同じ／スライダーは意図した違い | draw:84-99 / ls:748-751,773-781 |
| 最初のキーより前 | 表示しない | 同じ | 同じ | draw:84 / ls:773 |
| timer の範囲 | 符号なしで扱い、負と 500 超は表示しない。140 は拍。停止中は -1 | 0〜499 が有効 | 同じ（500 ちょうどだけ違う） | O En_timer.cpp:114-123 / src/skin/skin_renderer.h:31-39 |
| op1〜3 | AND。`!` で反転。op4 = 皿の回転 1/2。op5 は不明 [?] | AND と `!` は同じ。op4 は 1 だけ | 同じ／一部 | dst_option.txt:1-4 / O Main.cpp:2010-2016 / rd:612 |

### 5.4 描画（blend・filter・angle・center）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 描かない条件 | a < 2 かつ blend ≠ 0 | 同じ | 同じ | draw:743 / rd:710 |
| blend 1 で a ≧ 255 | 0（ノーブレンド）に落とす | 同じ | 同じ | draw:746 / rd:711 |
| blend 0 | ノーブレンド。a は無視、画像のαは効く | a=255 のαブレンド | 同じ | H history.txt:676 / rd:725 |
| blend 2 加算 / 3 減算 | 加算 / 減算（dst − src×α） | 同じ | 同じ | rd:726-731 |
| blend 4 乗算 | 乗算（αを見ない） | SDL の MOD | 同じ | rd:732-733 |
| blend 5 | 資料に記載なし。OpenLR2 は SUB2 [EXT?] | 減算 | 同じ扱い | load:98 / rd:728 |
| blend 6 XOR | 資料にはある。OpenLR2 は 1 に置き換え。DxLib の XOR はソフト描画時だけ [?] | 1 に置き換え | 違う [?]（実スキンでの使用は 0） | H history.txt:682 / ls:203 |
| blend 9 描画先の色を反転して掛ける | src × (1 − dst) | 同じ式（αは見ない [?]） | 同じ [?] | rd:734-737 |
| blend 10 描画元の色を反転 | 描画元を反転してαブレンド | 反転したテクスチャで描く | 同じ（LR2 標準 decide が 20 行で使う） | rd:716-718 |
| blend 11 αつき乗算 | αを考えて乗算 | MOD（αを見ない） | 違う（使用 0） | H history.txt:684 / rd:733 |
| filter | 0 は最近傍、1 は双線形（2 は異方性） | 0 以外は双線形 | 同じ | draw:749 / rd:719 |
| w, h が負 | 反転せず、絶対値にして原点をずらす | 同じ | 同じ | draw:752-759 / rd:712-713 |
| 座標 | +0.5 して整数に丸める | 同じ | 同じ | draw:829 / rd:744-746 |
| angle | 度単位で時計回り。補間する | 同じ | 同じ | draw:766 / rd:772 |
| center | テンキー配置: 1=左下、2=下中央、3=右下、4=左中央、6=右中央、7=左上、8=上中央、9=右上、それ以外（0・5）は中央 | 同じ（丸めた後の矩形を基準。誤差 1px 未満） | 同じ | draw:767-801 / rd:761-771 |
| 描画順 | sortID を整数で、安定でない qsort で並べる | 浮動小数のまま安定ソート | 意図した違い（実質同じ） | draw:976-985 / rd:700 |
| スキン全体の移動・拡大（SkinCustomize の rate/shift） | 描くときに掛ける。プレイ中の 1〜4 キーで移動・拡大・判定位置・ノーツの大きさを変える | 無い | RDに無い | draw:989-993 / H history.txt:731-735 |

### 5.5 文字（#FONT / #LR2FONT / 数値）

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| `#FONT,size,thick,type` | DxLib のフォント種類（0 普通、1 縁取り、2 AA、3 AA＋縁取り…）。書体は LR2 の設定、4 欄目以降は無視 [?] | size と thick だけ使い、type を無視 | 違う（type=3 の縁取りが出ない。実スキンに 19 行） | load:996-1010 / src/render/text_cache.h:20 |
| 番号 n の解決 | `#LR2FONT n` があり size > 0 なら画像フォント、無ければ `#FONT n` | 同じ | 同じ | draw:847-848 / rd:786-827 |
| 大きさ・寄せ | 縦は h/size 倍。横は w を超えるときだけ縮める。align 0 左、1 中央、2 右 | 同じ | 同じ | draw:855-866 / rd:802-806,831-836 |
| 回転 | 文字は回転しない | 同じ | 同じ | draw:183 |
| 文字の blend | DST の blend をそのまま。0 なら a を無視して不透明 | 2 なら加算、それ以外は a つきαブレンド | 違う [?]（blend 0 で a < 255 の DST_TEXT が 66 行。LR2 は不透明、RD はフェードする） | draw:851 / rd:785 |
| 文字色 | 色指定と SetDrawBright の両方に r,g,b を渡すので二重に掛かる可能性 [?] | 1 回だけ掛ける | [?] | draw:866-867 |
| `.lr2font` の形式 | #S 大きさ、#M 字間、#T 番号,画像、#R 文字コード,画像,x,y,w,h。コード 0〜255 ASCII、256〜8126 は SJIS−32832、8127〜15306 は SJIS−49281。無い字は `?`、空白は描かずに進める | 同じ（dxa 圧縮にも対応） | 同じ | load:443-521 / ls:533-596 |
| 画像フォントの透過色 | そのときの #TRANSCOLOR で読む [?] | 透過色なしで読む | [?] | load:519 / ls:588-592 |
| 文字の編集（SRC_TEXT の op1=edit、op2=panel） | 編集可能な文字列は何でも、IME 表示あり | 検索欄の st 30 だけ | 一部 | text.txt:2-6 / select_scene.cpp:1212-1216 |
| `#SRC_NUMBER` | op1=num / op2=align（0 右、1 左、2 中央）/ op3=桁、コマ数 10・11・24、裏 0、符号 | 同じ | 同じ（number.txt:11 は「桁数は ± を含む」と書くが、実装は符号を桁の外に描く [?]） | draw:575-684 / rd:150-207 |

### 5.6 特殊画像（gr）

| gr | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| 100 STAGEFILE | 決定画面で読み、リザルトの終わりまで持つ | 決定・プレイ・リザルトで差し替える | 同じ | H history.txt:844 / decide_scene.cpp:50, play_session.cpp:785 |
| 101 BACKBMP | 決定画面からリザルトまで | 決定・プレイ・リザルトで差し替える | 同じ（決定画面は直した） | H history.txt:845 / decide_scene.cpp:51, play_session.cpp:786 |
| 102 BANNER | 選曲画面。`#RELOADBANNER` があるときだけ曲ごとに変わる | 同じ | 同じ | sh:305 / select_scene.cpp:1495-1519 |
| 104 | 256x256 の空の画像。用途不明 [?] | 無い | [?] | load:389-390 |
| 105 | スキンセレクトのプレビュー | あり | 同じ | sh:307 / skin_select_scene.cpp:141 |
| 110 / 111 | `LR2files/Config/black.bmp` / `white.bmp` | 同じ（無ければ 1x1） | 同じ | load:395-398 / ls:733-742 |
| 画像が無いとき | 仮の画像を描く（黒くなるか [?]） | 何も描かない | [?] | load:391-394 |

### 5.7 動画・ワイルドカード・カスタマイズの保存

| 項目 | LR2 | RD | 判定 | 出典 |
|---|---|---|---|---|
| #IMAGE の動画 | 拡張子の末尾 3 文字が mpg/avi/wma/ogv なら動画。シーン開始から無音でループ | 空の画像にする | RDに無い（同梱スキンでの使用は 0） | load:974-978,1849-1855 / H history.txt:729 / ls:300 |
| `*` ワイルドカード | 最後の段の `*` は読むたびにランダムな 1 ファイル | 同じ | 同じ | fu:874-879 / ls:627-637 |
| 途中の段の `*` | #CUSTOMFILE のフォルダ置き換えでだけ解決 | 同じ。RANDOM はパターンが `*` で終わるか途中の段に `*` があればフォルダから、それ以外はファイルから選ぶ | 直した（例 `WMIX_Result_HD` の `FRAME\*\Shutter.png`） | fu:829-866 / ls `custom_file_candidates` |
| CUSTOMFILE の置き換え | パスの先頭がパターンと一致したら `*` を選んだ名前に（RANDOM/ERROR/空は置き換えない）。#IMAGE・#LR2FONT・#INCLUDE が対象 | 同じ（比較は大文字小文字を区別しない [?]） | 同じ | load:980-985,1690,1751 / ls:223-233 |
| 保存先 | `LR2files/SkinCustomize/<スキンのパス文字列の md5（小文字16進）>.xml`（shift_jis）。`<skinconfig><config_N>` に op 番号（N=0..39）、`<customfile><filename_N>`、さらに shift/rate/judge/size/dark/note_1p/note_2p | config.txt に `skin_custom <絶対パス> = 選択肢の番号,…` | 意図した違い（形式）。LR2 の xml は取り込める（#CUSTOMFILE は名前で）。調整値（shift/rate など）は RDに無い | cfgsv:746-800 / load:1868-1891 / config.cpp:78-84,147-148, app.cpp:154-157 |

## 6. RD に無いもの（優先度の目安）

### 高

| 項目 | 理由 | 章 |
|---|---|---|
| ミキサーの音量（MASTER / KEY / BGM）とスライダー 17〜19、config への保存 | ゲインの処理が一つも無い。リザルトの音のフェードアウト、EQ・ピッチなどすべての前提 | 4.3 |
| F6 のスクリーンショット（PNG・スキン解像度・#SCREENSHOT の音・全画面共通） | LR2 の基本操作。いまは選曲の F6 がレーンカバーに使われていてぶつかる | 1.4, 4.7 |
| ~~キーコンフィグで START / SELECT を割り当てる手段（通常の画面から）~~ 直した（スキンの画面で割り当てられる） | ウィザードでは割り当てられず、一覧画面は開発用 | 2.2 |
| ~~スキンで描くキーコンフィグ画面（型 8・文字 40-55・OP 400-402・ボタン 101-157）とスロット方式（8 枠・Delete で 1 つ消す・F1/F2）~~ 直した | LR2 の KeyConfig スキンがそのまま使えない | 2.2 |
| `scoregraph` / `defaulttarget` / `target` の設定と、選曲の sort / key / difficulty の保存 | scoregraph は LR2 の既定（OFF）と違う値で固定、ターゲットは 90 固定、選曲の状態は起動ごとに戻る | 3.6 |

### 中

| 項目 | 章 |
|---|---|
| リプレイの保存（OFF / ALWAYS / HISCORE / CLEAR / FC）と再生（6 番キー長押し）。仕様書は全プレイの入力ログを予定 | 1.4 |
| ゴーストのデータ（判定の並び）の保存。いまは EX からの近似 | 1.3 |
| rseed・op_best の保存（自己ベストのランダムの再現） | 1.3 |
| サウンドセットのカスタマイズ（ユーザーの #CUSTOMFILE 選択と RANDOM、#CUSTOMOPTION、#IF 系、#INCLUDE） | 4.1 |
| ピッチ（FREQ / PITCH / SPEED。FREQ・SPEED は保存しない規則付き）、イコライザ 7 バンド（スライダー 10〜16） | 4.3 |
| EX モードと EXSELECT / EXDECIDE | 4.2, 2.5 |
| テーマ（スキン種類 11）で全部のスキンを一度に替える | 2.3 |
| スキンセレクトのプレビューでサンプル譜面を自動演奏する機能、プレビューを出さない設定 | 2.3 |
| DB 更新の「開いたフォルダで自動」（autoreload=1）と、今のフォルダだけを読み直す更新（LR2 の F8） | 2.1, 3.1 |
| ghost（位置）・bgasize（EXTEND）・replaysave の設定、右クリック長押しで曲を終える操作 | 3.3, 3.6 |
| folderlamp・buttonselect・disablesubtitle（SELECT タブから項目も抜けている）・difficultychangetype・ignorekey*・titleflash と NEW SONG | 3.2-3.4 |
| スキンの #IMAGE の動画（無音ループ）、`#FONT` の type（縁取り・AA）、SkinCustomize の調整値（移動・拡大・判定位置・ノーツの大きさ・dark）とプレイ中の 1〜4 キー | 5.4, 5.5, 5.7 |
| F7 の FPS 表示 | 4.5 |
| ~~パッドを台ごとに区別すること（いまは全部 1P にまとまる）~~ 直した（`joy2:` のように台を決めた割り当て） | 4.6 |
| LR2 の keyconfig.xml の取り込み | 2.2, 4.6 |
| MIDI の入力（~~鍵盤・ピッチベンドの皿・ペダル~~ 直した。CC のスライダーはまだ） | 2.2, 4.6 |

### 低

- スコア: complete フラグ、失敗回数、プレイ時間、プレイヤー累計をプレイ中に数える方式、scorehash、DB に無い譜面を保存しない扱い、遅延率の検査、コースのリトライ（STAGE1 から）、リザルトとコースリザルトのマウス操作。
- 画面: ~~決定画面の BACKBMP・BANNER（gr101/102）~~（直した）、ボタン 191（スキンの確定）、F1 の F キー説明、曲フォルダ未登録の起動時チェック。
- 設定: customfolder（特殊フォルダ）、randomclose、maindisplay、disablesystemkey、disableimagefont / fontname、course.*（ランダムコース）、tools.*、newsongfolder、newrandomrestart [EXT]、autokey [?]。影響の無いもの（highcolor、outputlog、mainsleep、language、eventmode、directdraw）。
- 音声: エフェクタ（スライダー 20〜25）、出力デバイスの選択と ASIO、コースのステージごとのピッチ、muon.wav による遅れの検出。
- スキン: LR2 の SkinCustomize xml の取り込み、`#NOBGA`、`#TEXTMERGIN`、blend 11 のα、CSV の引用符、st 30 以外の文字列編集。
- 一覧だけ: PO4 / スターター（Scene09-11）、Lunaris（Scene08）、録音（LR2_makemp3）・録画（En_recordmovie、-auto2avi など）。
- 対象外（意図した違い）: 5/9/10/14 鍵と BATTLE の各スキン・キーコンフィグ、`#FLIPSIDE`、2P 側のキー、IR まわり一式。

## 7. 違っているもの（直すべき候補）

LR2 の挙動を守る方針（memory: LR2挙動の維持）なので、どれも直していない。直すかどうかの判断が要るものは「判断」と書いた。

### スコア（記録が LR2 と食い違うので優先）

1. **閉店したときの最小 BP が少なく記録される**（B play_session.cpp:122）。LR2 は処理したノーツが総数に届かなければ必ず残りを BP に足す（O LR2_statplay.cpp:992-1000）。RD は `quit_early` のときだけ。
2. **ASSIST・SCATTER・CONVERGE での保存**（B play_session.cpp:437-456、lane_option.cpp:111）。LR2 はランプ（EASY まで。FC でも EASY）・回数・op_history だけを残し、EX・BP・コンボ・判定数・ゴーストは残さない。リザルトの 152（更新表示）も飛ばす。RD は EX なども書き、SCATTER の FC はそのまま、CONVERGE はランプも下げない。
3. **HS固定 CONSTANT**（B play_session.cpp:187）。LR2 は EASY までのランプとプレイ回数を記録する。RD は何も残さない。
4. **コース中のステージの保存**（B play_session.cpp:451）。LR2 は ASSIST・SCATTER 以上などでステージを保存しない。RD は保存する。コースのランプに EASY の上限をかけている点も LR2 と違う [?]。コースの op_history に ASSIST ビットが付かない。あわせて `docs/lr2_course_spec.md` の「S-RANDOM 以上は保存しない」は誤り（S-RANDOM は保存する）。

### 設定

5. **`levelbar_5` / `levelbar_9` が効かない**。SETUP で編集・保存できるが、本体は鍵盤数に関係なく `levelbar_7` を使う（B select_context.cpp:115,138,294）。7K+S 専用なら 2 つは灰色にする、5K を扱うなら実装する（判断）。
6. **SCORE GRAPH が常に ON**（B select_context.cpp:78,230）。LR2 の既定は OFF。
7. **DB 更新の表示と挙動**。SETUP には「F5で現在選択中のフォルダの更新」とあるが、RD の F5 は全フォルダを読み直す（B launcher.cpp:473、select_scene.cpp:1283）。LR2 は F8 で今のフォルダだけ、F5 は IR に接続。値の番号も LR2 は 2=起動時、RD は 0=起動時。
8. **右クリックで曲を終える操作が無い**のに、SETUP では「右クリックで曲を終了させない」がチェック無しで表示されている（B launcher.cpp:542）。
9. **SETUP の灰色項目の表示が LR2 の既定と違う**。「高精度なタイマー」「FMOD Exを使用しない」がチェック有りで出ている（B launcher.cpp:584-585）。灰色の項目は LR2 の既定値で見せるという launcher.cpp:369 のコメントと食い違う。
10. **項目の置き場所と抜け**。「選曲中プレビューを再生する」が RD タブにある（B launcher.cpp:653）。「選曲バーにサブタイトルを表示しない」は SELECT タブから抜けている。
11. **フルスクリーンとウインドウの既定**。LR2 は既定がフルスクリーン（排他）、640×480・100%。RD は既定がウインドウで、フルスクリーンはボーダーレスだけ、SD スキンは 200%（B app.cpp:71,127）。仕様書:299 で未決（判断）。
12. **LR2 からの引き継ぎが曲フォルダと ID だけ**（B lr2_import.cpp:30-45）。ハイスピの範囲・判定タイミング・オプション・スキンのパス・keyconfig.xml・SkinCustomize などは引き継がない（判断）。

### 画面と操作

13. **選曲の F6 がレーンカバー**（B select_scene.cpp:1295-1299）。LR2 ではスクリーンショット。F キー全体も F1 説明 / F5 IR / F7 FPS / F8 再読み込みと違う（判断）。
14. **選曲の ESC**。LR2 はフォルダの中でもすぐ終了、RD は一つ戻る（B select_scene.cpp:1278-1282）。LR2 に合わせるか、意図した違いとして仕様書に書くか（判断）。
15. ~~**決定画面**。`#SCENETIME` が無い（0 の）スキンで LR2 はすぐプレイへ進むが、RD は待ち続ける（B decide_scene.cpp:96）。LR2 はタイマー3 の後は ESC を受け付けず、戻るときにシステム音を止める。~~ 直した。
16. ~~**キーコンフィグ**。Esc で抜けると保存しない（LR2 は割り当てるたびに保存）。矢印と F1〜F12 を割り当てられる（LR2 は禁止。選曲の操作とぶつかる恐れ）。~~ スキンの画面で直した（ウィザードはスキンが無いときだけ出る）。
17. **スキンセレクトに入るときにフェードしない**（キーコンフィグは直した）。LR2 は選曲がフェードアウトしてから移る。スキンセレクトは LR2 では毎回 7KEYS から始まる [?]。
18. **F5 ですぐリトライ**（B play_session.cpp:670）と **プレイ中の F4**（B play_session.cpp:673）は LR2 に無い操作（判断）。

### 音声

19. **COURSECLEAR / COURSEFAIL が 1 回しか鳴らない**（B play_session.cpp:1200）。LR2 はループ（O LR2_audio.cpp:493,497）。
20. **サウンドセットの #CUSTOMFILE で常に既定値を使う**（B system_sound.cpp:64-80）。スキンセレクトで選んだ BGM フォルダが音に反映されない。
21. **サウンドセットの `#` 無しの行を命令として読む**（B system_sound.cpp:55）。LR2 は `#` の行しか読まない [?]。
22. 256 音が埋まったときに一番長い音を止める方式が FMOD とそろっているか未確認 [?]（B mixer.cpp:188-193）。

### スキン

23. **`#FLIPRESULT` / `#DISABLEFLIP` を読み込み順どおりに処理していない**（B lr2_skin.cpp:355-358、play_session.cpp:463-466）。LR2 は読んだ時点で op350/351 を書き換え、後に書いた方が勝ち、リザルト側の #FLIPRESULT も効く。LR2 標準 result.lr2skin の RESULT SIDE（911/912）が効かない。
24. **行の長さの判定に改行を含めていない**（B lr2_skin.cpp:245）。LF だけのスキンでは素の `#ENDIF` が捨てられて #IF の入れ子が崩れる。
25. **`#CUSTOMFILE` の既定値が候補に無いと先頭の候補を選ぶ**（B lr2_skin.cpp:708-709）。LR2 は既定値（空ならランダム）。候補名を最後の `.` で切るのでフォルダ名の `.` まで切れ、フォルダ型の RANDOM も失敗する（:631,702）。
26. **カスタマイズを番号で保存している**（B skin_catalog.cpp:126）。壁紙などのファイルを足したり消したりすると選択がずれる。LR2 はファイル名で残す。
27. **文字の blend 0 で a を使っている** [?]（B skin_renderer.cpp:785）。LR2 は不透明で描くので、フェードインする文字の見え方が違う（実スキンに 66 行）。
28. 軽微: `#CUSTOMOPTION` を 900 未満でも反映（B lr2_skin.cpp:729）、`#SETOPTION` の 900 未満が描画時の op に漏れる（B src/render/skin_play.cpp:160）、ONMOUSE でも SRC の時刻を DST の最初のキーから数える（B skin_renderer.cpp:593）、blend 11 をαなしで描く（:733）、~~決定画面で gr101 が無い~~（直した）。

### 実物の LR2 で確かめれば決まる [?]

- 文字の blend 0 が本当に不透明か、文字色が二重に掛かるか、STAGEFILE が無いときの仮の画像が黒いか（5 章）。
- リプレイ保存モード 3/4 の並び（コードと help が逆）、コース中のクイックリトライ（1 章）。
- サウンドセットの `#` 無しの行の扱い、スクリーンショットのファイル名（4 章）。
