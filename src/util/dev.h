// 開発用の口（RD_ で始まる環境変数）。画面を撮る・操作を流す・入力を流すなど、ふだんの遊び方では要らないものに使う。
// 配る版は RD_DEV を切ってビルドし、どれも読まない（入力を作る口から不正に使われないように）
#pragma once

#include <cstdlib>

namespace rd {

inline const char* dev_env(const char* name) {
#ifdef RD_DEV
    return std::getenv(name);
#else
    (void)name;
    return nullptr;
#endif
}

}  // namespace rd
