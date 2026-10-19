# third_party

| ファイル | 版 | ライセンス |
| --- | --- | --- |
| miniaudio.h | 0.11.22 | Public Domain / MIT-0 |
| dr_wav.h, dr_mp3.h | dr_libs `da35f9d` | Public Domain / MIT-0 |
| dr_flac.h | dr_libs `dfe8377`（v0.13.4） | Public Domain / MIT-0 |
| stb_vorbis.c, stb_image.h, stb_truetype.h | stb `f58f558` | Public Domain / MIT |
| sqlite/ | SQLite 3.46.1（amalgamation） | Public Domain |
| yyjson/ | yyjson 0.10.0 | MIT |
| ffmpeg/9/, ffmpeg/8/ | FFmpeg 9.0.1 と 8 系のヘッダーだけ（libavcodec・libavformat・libavutil・libswscale） | LGPL 2.1 以降 |

実装は `third_party_impl.c` でまとめて生成する。

FFmpeg は同梱しない。動画の BGA は、利用者が FFmpeg の共有ライブラリ（8 系か 9 系）を置いたときだけ、実行時に読み込んで使う（`src/video/`）。
