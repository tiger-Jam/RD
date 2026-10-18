cmake_minimum_required(VERSION 3.24)
project(BMjoy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BMJ_ASAN "AddressSanitizer付きでビルドする" OFF)

find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)

# 単一ヘッダのライブラリは実装を1つの翻訳単位にまとめる
add_library(bmj_third_party STATIC third_party/third_party_impl.c third_party/stb_vorbis_impl.c
    third_party/sqlite/sqlite3.c third_party/yyjson/yyjson.c)
target_include_directories(bmj_third_party SYSTEM PUBLIC third_party third_party/sqlite third_party/yyjson)
target_compile_definitions(bmj_third_party PUBLIC SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION)
find_package(CURL REQUIRED)
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
    src/game/judge.cpp
    src/game/gauge.cpp
    src/util/text.cpp
    src/util/path.cpp
    src/util/dxa.cpp
    src/skin/lr2_skin.cpp
    src/util/hash.cpp
    src/db/song_db.cpp
    src/db/difficulty_table.cpp
)
target_include_directories(bmj_core PUBLIC src)
target_link_libraries(bmj_core PUBLIC bmj_third_party CURL::libcurl)
if(APPLE)
    target_link_libraries(bmj_core PUBLIC iconv)
endif()

add_executable(bmjoy
    src/main.cpp
    src/app/app.cpp
    src/app/config.cpp
    src/app/play_session.cpp
    src/app/select_scene.cpp
    src/audio/sound_bank.cpp
    src/audio/mixer.cpp
    src/audio/system_sound.cpp
    src/input/input.cpp
    src/input/keyconfig.cpp
    src/render/keyconfig_scene.cpp
    src/render/play_scene.cpp
    src/render/skin_play.cpp
    src/skin/skin_renderer.cpp
    src/render/text_cache.cpp
)
target_link_libraries(bmjoy PRIVATE bmj_core SDL3::SDL3)

add_executable(bmj_tests tests/test_main.cpp tests/test_bms_parser.cpp tests/test_judge.cpp tests/test_skin.cpp
    tests/test_input.cpp tests/test_db.cpp tests/test_table.cpp src/input/input.cpp src/input/keyconfig.cpp)
target_link_libraries(bmj_tests PRIVATE bmj_core SDL3::SDL3)

foreach(t bmj_core bmjoy bmj_tests)
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
