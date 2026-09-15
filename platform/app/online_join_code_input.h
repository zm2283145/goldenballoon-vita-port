#ifndef MDKR_ONLINE_JOIN_CODE_INPUT_H
#define MDKR_ONLINE_JOIN_CODE_INPUT_H

#include "imgui.h"

#include <cstddef>
#include <cstring>

inline int OnlineRoom_joinCodeDigitFilter(ImGuiInputTextCallbackData *data) {
    // ImGui applies the same filter to typing and clipboard text. The fixed
    // buffer admits six ASCII digits, preserving leading zeroes.
    return (data->EventChar < '0' || data->EventChar > '9') ? 1 : 0;
}

inline std::size_t OnlineRoom_drawJoinCodeInput(char (&code)[7]) {
    // Keep text, caret, selection, click positions and horizontal scrolling
    // under one editor. Grouping belongs on the read-only host invitation,
    // never on an overlay over this editable field. The caller owns styling
    // and spoken guidance; the returned count reflects this frame's edit.
    ImGui::InputTextWithHint("##beta-join-code", "123456", code, sizeof(code),
                            ImGuiInputTextFlags_CallbackCharFilter,
                            OnlineRoom_joinCodeDigitFilter);
    return std::strlen(code);
}

#endif
