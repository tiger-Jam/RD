#include "app/keyconfig_editor.h"

namespace bmj {

int keyconfig_item_button(int item) {
    if (item >= 1 && item <= 7) return kKey1 + item - 1;
    switch (item) {
    case 10: return kScratchA;
    case 11: return kScratchB;
    case 12: return kStart;
    case 13: return kSelect;
    default: return -1;
    }
}

bool keyconfig_reserved(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: return true;
    default: return sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12;
    }
}

namespace {
int order_index(int item) {
    for (int i = 0; i < kKeyconfigOrderCount; ++i)
        if (kKeyconfigOrder[i] == item) return i;
    return 0;
}
}  // namespace

bool KeyconfigEditor::click(int type, int dir) {
    if (type >= 101 && type <= 139) {
        // 項目を選ぶ。BMjoy に無い項目（2P）は押せない
        if (keyconfig_item_button(type - 100) < 0) return false;
        item_ = type - 100;
        return true;
    }
    if (type == 140) {
        // 項目を順に送る
        int i = order_index(item_) + (dir < 0 ? -1 : 1);
        i = (i % kKeyconfigOrderCount + kKeyconfigOrderCount) % kKeyconfigOrderCount;
        item_ = kKeyconfigOrder[i];
        return true;
    }
    if (type >= 150 && type < 150 + kKeySlots) {
        slot_ = type - 150;
        return true;
    }
    // 141・142（9 ボタン・5 鍵の項目）と 143（鍵盤モードの切り替え）は、7 鍵モードしか無いので押せない
    return false;
}

bool KeyconfigEditor::assign(const Binding& b) {
    int button = keyconfig_item_button(item_);
    if (button < 0 || slot_ < 0) return false;
    set_slot(cfg_, button, slot_, b);
    return true;
}

bool KeyconfigEditor::erase() {
    int button = keyconfig_item_button(item_);
    if (button < 0 || slot_ < 0) return false;
    clear_slot(cfg_, button, slot_);
    return true;
}

void KeyconfigEditor::reset_default() {
    KeyConfig d = KeyConfig::lr2_default();
    cfg_.buttons = d.buttons;
}

void KeyconfigEditor::clear_all() { clear_bindings(cfg_); }

bool KeyconfigEditor::option(int op, bool& v) const {
    // 400 / 401 / 402 = 鍵盤モードが 7 鍵 / 9 ボタン / 5 鍵。BMjoy は 7 鍵だけ
    if (op < 400 || op > 402) return false;
    v = op == 400;
    return true;
}

bool KeyconfigEditor::text(int id, std::string& s) const {
    // 40〜55 = 選んでいる項目の 1〜16 番目の割り当て。無ければ "-"
    if (id < 40 || id > 55) return false;
    int button = keyconfig_item_button(item_);
    size_t n = static_cast<size_t>(id - 40);
    if (button >= 0 && n < cfg_.buttons[button].size()) s = binding_label(cfg_.buttons[button][n]);
    else s = "-";
    return true;
}

bool KeyconfigEditor::button(int type, int& v) const {
    if (type >= 101 && type <= 139) v = item_ == type - 100 ? 1 : 0;
    else if (type == 140) v = order_index(item_);
    else if (type >= 141 && type <= 143) v = 0;
    else if (type >= 150 && type <= 157) v = slot_ == type - 150 ? 1 : 0;
    else return false;
    return true;
}

}  // namespace bmj
