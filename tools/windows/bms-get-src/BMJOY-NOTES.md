# bms-get（改造版）

元は bms-get（MIT ライセンス、LICENSE を参照）。BMjoy の収集（`tools/windows/bms-fetch.ps1`）のために次を足した。

- exFAT でハードリンクが使えないときの保存（`store.rs`）
- `--body-dir`：持っている本体のフォルダへ差分だけを入れる
- `md5 --main-url / --diff-url / --hint / --artist / --threads`：表の URL、曲名、要望スレの索引
- `threads <file>`：要望スレ（したらばの BMSクレクレ板 travel/2565・発狂BMS板 game/39018、消えたスレは Internet Archive）の索引を作る
- ページの中のリンクを辿る（`page.rs`）、JSON の中のリンク、`<meta refresh>`・frame
- サイトごとの取り方（`site/`）：MediaFire、getuploader、MEGA（ファイルとフォルダ、復号）、OneDrive、Internet Archive（切れたリンクの写し）、JavaScript の人の確認（k-bms など）
- Google Drive のフォルダ・`docs.google.com`、Dropbox の共有フォルダ
- 7z、LZH、自己解凍 exe（実行はせず中の圧縮部分だけを取り出す）、zip・LZH の Shift_JIS の名前
- 譜面を Shift_JIS で読む、音・画像の拡張子の読み替え（wav/ogg など）
- パックで取れた譜面を音のあるフォルダへ移す、パック全体ではなく曲のフォルダだけを保存する

Windows でのビルドは `C:\Users\mmzkq\Tools\build_bmsget.cmd`（このフォルダの中身を `C:\Users\mmzkq\Tools\bms-get` に写してから）。
