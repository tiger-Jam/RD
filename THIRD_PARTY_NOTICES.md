# 使っているもののライセンス

BMjoy は、次のソフトウェアとデータを使っています。

## BMjoy に組み込んでいるもの

| もの | 版 | ライセンス | 表記 |
| --- | --- | --- | --- |
| SQLite | 3.46.1 | パブリックドメイン | 不要 |
| miniaudio | 0.11.22 | パブリックドメイン / MIT-0 | 不要（MIT-0 を選ぶ） |
| dr_wav・dr_mp3・dr_flac（dr_libs） | da35f9d / dfe8377 | パブリックドメイン / MIT-0 | 不要（MIT-0 を選ぶ） |
| stb_image・stb_vorbis・stb_truetype | f58f558 | MIT / パブリックドメイン | 下に MIT の全文 |
| yyjson | 0.10.0 | MIT | 下に全文 |
| Lua | 5.4.7 | MIT | 下に全文 |
| SDL3 | 3.4.14 | zlib | 下に全文（Windows では SDL3.dll を同梱） |

## 同梱の MOD に入っているもの

| もの | 使っている MOD | ライセンス | 表記 |
| --- | --- | --- | --- |
| walkure-offline（推定のモデルと計算） | リコメンド（mods/walkure） | MIT | mods/walkure/LICENSE・NOTICE.txt |
| oraja-constellator（譜面の傾向の分析とフォルダの分け方） | 譜面の傾向（mods/constellator）と本体の src/chart/chart_attr.cpp | MIT | mods/constellator/LICENSE・NOTICE.txt |

## 配る版に同梱するもの

| もの | 版 | ライセンス | 表記 |
| --- | --- | --- | --- |
| FFmpeg（avcodec・avformat・avutil・swscale など） | 8.1.1 | LGPL 2.1 以降 | licenses/FFmpeg-LGPL-2.1.txt |

- FFmpeg は動画の BGA を出すときだけ、実行ファイルの横の DLL を実行時に読み込みます。BMjoy には組み込んでいません（ソースには型を合わせるためのヘッダーだけがあります）。DLL は差し替えられます（8 系か 9 系）。
- FFmpeg のソースは https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz から入手できます。

## 同梱しないもの

- **libcurl**（macOS）：OS のものを使います。Windows では OS の WinHTTP を使います。
- **Visual C++ ランタイム**（Windows）：Microsoft の再頒布可能パッケージの条件で、実行ファイルの横に置きます。
- **フォント**：OS のもの（Windows は MS UI ゴシック、macOS はヒラギノ角ゴシック）を使い、同梱しません。

---

## MIT License（stb）

Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## MIT License（yyjson）

Copyright (c) 2020 YaoYuan <ibireme@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## MIT License（Lua）

Copyright (C) 1994-2024 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## zlib License（SDL3）

Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
