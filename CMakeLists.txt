cmake_minimum_required(VERSION 3.24)
project(BMjoy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BMJ_ASAN "AddressSanitizer付きでビルドする" OFF)

# SDL3：入っていればそれを使い、無ければソースを取ってきて一緒にビルドする（Windowsで vcpkg が無くても通るように）
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3 URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-3.4.14.tar.gz)
    FetchContent_MakeAvailable(SDL3)
endif()
find_package(Threads REQUIRED)

# 単一ヘッダのライブラリは実装を1つの翻訳単位にまとめる
add_library(bmj_third_party STATIC third_party/third_party_impl.c third_party/stb_vorbis_impl.c
    third_party/sqlite/sqlite3.c third_party/yyjson/yyjson.c)
target_include_directories(bmj_third_party SYSTEM PUBLIC third_party third_party/sqlite third_party/yyjson)
target_compile_definitions(bmj_third_party PUBLIC SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION)
if(NOT WIN32)
    find_package(CURL REQUIRED)  # Windows は WinHTTP を使う
endif()
if(APPLE)
    target_link_libraries(bmj_third_party PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
elseif(UNIX)
    target_link_libraries(bmj_third_party PUBLIC ${CMAKE_DL_LIBS} m)
endif()
target_link_libraries(bmj_third_party PUBLIC Threads::Threads)

# ゲームの中身（SDLに依存しない部分）。テストからも使う
add_library(bmj_core STATIC
    src/chart/chart.cpp
    src/chart/bms_parser.cpp
    src/chart/osu_parser.cpp
    src/chart/bmson_parser.cpp
    src/chart/chart_loader.cpp
    src/game/judge.cpp
    src/game/lane_option.cpp
    src/game/gauge.cpp
    src/game/hispeed.cpp
    src/game/ghost.cpp
    src/game/replay.cpp
    src/util/text.cpp
    src/util/path.cpp
    src/util/dxa.cpp
    src/skin/lr2_skin.cpp
    src/util/hash.cpp
    src/db/song_db.cpp
    src/db/difficulty_table.cpp
    src/db/custom_folder.cpp
    src/audio/dsp.cpp
)
target_include_directories(bmj_core PUBLIC src)
# 開発用の口（BMJ_ で始まる環境変数で画面を撮る・操作や入力を流す）。配る版は -DBMJ_DEV=OFF でビルドする
option(BMJ_DEV "開発用の環境変数を読む" ON)
if(BMJ_DEV)
    target_compile_definitions(bmj_core PUBLIC BMJ_DEV)
endif()
target_link_libraries(bmj_core PUBLIC bmj_third_party)
if(WIN32)
    target_link_libraries(bmj_core PUBLIC winhttp hid)
else()
    target_link_libraries(bmj_core PUBLIC CURL::libcurl)
endif()
if(APPLE)
    target_link_libraries(bmj_core PUBLIC iconv)
endif()

# 本体（LR2 の LR2body.exe に相当）。LR2 と同じくコンソールは出さない（ログはコマンドプロンプトから動かしたときだけ見える）
add_executable(BMjoybody WIN32
    src/main.cpp
    src/app/app.cpp
    src/app/config.cpp
    src/app/play_session.cpp
    src/app/select_scene.cpp
    src/app/select_context.cpp
    src/app/course.cpp
    src/app/lr2_import.cpp
    src/app/decide_scene.cpp
    src/app/skin_catalog.cpp
    src/app/skin_select_scene.cpp
    src/app/keyconfig_editor.cpp
    src/app/keyconfig_skin_scene.cpp
    src/video/video_decoder.cpp
    src/video/ffmpeg9.cpp
    src/video/ffmpeg8.cpp
    src/audio/sound_bank.cpp
    src/audio/mixer.cpp
    src/audio/system_sound.cpp
    src/input/input.cpp
    src/input/keyconfig.cpp
    src/input/win_rawhid.cpp
    src/input/midi.cpp
    src/render/keyconfig_scene.cpp
    src/render/frame_hooks.cpp
    src/render/play_scene.cpp
    src/render/skin_play.cpp
    src/skin/skin_renderer.cpp
    src/render/text_cache.cpp
)
# 動画の BGA：FFmpeg のヘッダーだけを使い、ライブラリは実行時に読み込む（同梱しない）。
# 構造体が版ごとに違うので、8 系と 9 系のヘッダーでそれぞれビルドし、見つかった版を使う
set_source_files_properties(src/video/ffmpeg9.cpp PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/9)
set_source_files_properties(src/video/ffmpeg8.cpp PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/8)
target_link_libraries(BMjoybody PRIVATE bmj_core SDL3::SDL3)
if(WIN32)
    # 日本語のファイル名を扱えるように、プロセスのコードページを UTF-8 にする（OpenLR2 と同じ）
    target_sources(BMjoybody PRIVATE src/app/bmjoy.manifest)
    target_compile_definitions(bmj_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# 起動前の設定アプリ（LR2 の LR2.exe に相当）。「起動」で BMjoybody を黒い画面つきで立ち上げる
add_executable(BMjoy WIN32 src/launcher/launcher.cpp src/app/config.cpp src/app/lr2_import.cpp src/app/skin_catalog.cpp
    src/render/text_cache.cpp)
target_link_libraries(BMjoy PRIVATE bmj_core SDL3::SDL3)
if(WIN32)
    target_sources(BMjoy PRIVATE src/app/bmjoy.manifest)
endif()

add_executable(bmj_tests tests/test_main.cpp tests/test_bms_parser.cpp tests/test_judge.cpp tests/test_skin.cpp
    tests/test_input.cpp tests/test_db.cpp tests/test_table.cpp tests/test_lane.cpp tests/test_import.cpp tests/test_dsp.cpp src/input/input.cpp src/input/keyconfig.cpp src/input/midi.cpp
    src/app/config.cpp src/app/lr2_import.cpp src/app/skin_catalog.cpp src/app/keyconfig_editor.cpp src/audio/sound_bank.cpp)
target_link_libraries(bmj_tests PRIVATE bmj_core SDL3::SDL3)
target_compile_definitions(bmj_tests PRIVATE BMJ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(WIN32)
    target_sources(bmj_tests PRIVATE src/app/bmjoy.manifest)
endif()

# MIDI の入力（src/input/midi.cpp）。macOS は CoreMIDI、Windows は winmm
foreach(t BMjoybody bmj_tests)
    if(APPLE)
        target_link_libraries(${t} PRIVATE "-framework CoreMIDI")
    elseif(WIN32)
        target_link_libraries(${t} PRIVATE winmm)
    endif()
endforeach()

foreach(t bmj_core BMjoybody BMjoy bmj_tests)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(BMJ_ASAN AND NOT MSVC)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

enable_testing()
add_test(NAME bmj_tests COMMAND bmj_tests)
