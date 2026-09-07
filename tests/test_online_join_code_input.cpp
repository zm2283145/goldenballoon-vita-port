// Production-widget regression: CPU draw commands and input events only.
// No platform backend, OS clipboard, native window, GPU or ROM is initialized.
#include "online_join_code_input.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::string clipboard;
const char *getClipboard(ImGuiContext *) { return clipboard.c_str(); }
void setClipboard(ImGuiContext *, const char *text) { clipboard = text; }

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL online join input: %s\n", message);
        std::exit(1);
    }
}

struct Editor {
    char code[7]{};
    std::size_t count = 0;
    ImGuiID id = 0;
    ImVec2 minimum{}, maximum{};
    float width;
    bool caretVisible = false;
    bool textVisible = false;
    bool selectionVisible = false;
    float caretX = 0.0f;
    float expectedCaretX = 0.0f;

    explicit Editor(float scale, float fieldWidth) : width(fieldWidth) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 320.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigMacOSXBehaviors = false; // deterministic Ctrl shortcuts
        io.ConfigInputTextCursorBlink = false;
        ImFontConfig font;
        font.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&font);
        unsigned char *pixels = nullptr;
        int pixelWidth = 0, pixelHeight = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &pixelWidth, &pixelHeight);
        expect(pixels != nullptr && pixelWidth > 0 && pixelHeight > 0,
               "CPU font atlas built");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1)); // draw-list token, not a GPU object
        ImGui::GetPlatformIO().Platform_GetClipboardTextFn = getClipboard;
        ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClipboard;
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScaleAllSizes(scale);
        style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
        style.Colors[ImGuiCol_InputTextCursor] = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);
        style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
        frame();
        frame(true);
        frame();
        expect(ImGui::GetActiveID() == id, "real editor receives keyboard focus");
    }

    ~Editor() { ImGui::DestroyContext(); }
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;

    ImGuiInputTextState &state() {
        ImGuiInputTextState *value = ImGui::GetInputTextState(id);
        expect(value != nullptr, "real editor owns input state");
        return *value;
    }

    void frame(bool focus = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
        ImGui::SetNextWindowSize(ImVec2(600.0f, 280.0f));
        ImGui::Begin("Join test", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetNextItemWidth(width);
        if (focus) ImGui::SetKeyboardFocusHere();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const int firstVertex = draw->VtxBuffer.Size;
        count = OnlineRoom_drawJoinCodeInput(code);
        id = ImGui::GetItemID();
        minimum = ImGui::GetItemRectMin();
        maximum = ImGui::GetItemRectMax();
        expect(count == std::strlen(code) && count <= 6u,
               "returned count matches this frame's bounded text");
        caretVisible = textVisible = selectionVisible = false;
        const ImU32 caretColour = ImGui::GetColorU32(ImGuiCol_InputTextCursor);
        const ImU32 textColour = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 selectionColour = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        float cursorMin = 0.0f, cursorMax = 0.0f;
        for (int index = firstVertex; index < draw->VtxBuffer.Size; ++index) {
            const ImDrawVert &vertex = draw->VtxBuffer[index];
            if (vertex.col == textColour) textVisible = true;
            if (vertex.col == selectionColour) selectionVisible = true;
            if (vertex.col != caretColour) continue;
            if (!caretVisible) cursorMin = cursorMax = vertex.pos.x;
            if (vertex.pos.x < cursorMin) cursorMin = vertex.pos.x;
            if (vertex.pos.x > cursorMax) cursorMax = vertex.pos.x;
            caretVisible = true;
        }
        caretX = (cursorMin + cursorMax) * 0.5f;
        if (ImGuiInputTextState *input = ImGui::GetInputTextState(id)) {
            const int cursor = input->GetCursorPos();
            expect(cursor >= 0 && static_cast<std::size_t>(cursor) <= count,
                   "insertion point stays in current text");
            expectedCaretX = std::floor(minimum.x + ImGui::GetStyle().FramePadding.x +
                ImGui::CalcTextSize(code, code + cursor).x - input->Scroll.x) + 0.5f;
        }
        ImGui::End();
        ImGui::Render();
    }

    void key(ImGuiKey key, bool ctrl = false, bool shift = false) {
        ImGuiIO &io = ImGui::GetIO();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        frame();
    }

    void type(const char *text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
    }

    void checkVisibleEditing() const {
        expect(caretVisible, "caret emits visible draw geometry");
        expect(std::fabs(caretX - expectedCaretX) < 1.1f,
               "visible caret follows actual insertion point and scrolling");
        expect(caretX >= minimum.x && caretX <= maximum.x,
               "caret stays within the editor");
        expect(count == 0u || textVisible, "entered digits emit visible text geometry");
    }
};

void testEditing(float scale) {
    Editor editor(scale, 340.0f);
    editor.type("00a1-23 4");
    expect(std::strcmp(editor.code, "001234") == 0, "typing filters and retains zeroes");
    expect(editor.count == 6u, "six digits ready in the same frame");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_Home);
    for (int index = 0; index < 3; ++index) editor.key(ImGuiKey_RightArrow);
    expect(editor.state().GetCursorPos() == 3, "middle insertion reached by arrows");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_Delete);
    expect(std::strcmp(editor.code, "00134") == 0, "Delete removes character after caret");
    editor.key(ImGuiKey_Backspace);
    expect(std::strcmp(editor.code, "0034") == 0, "Backspace removes character before caret");
    editor.type("9");
    expect(std::strcmp(editor.code, "00934") == 0, "middle insertion preserves suffix");
    editor.key(ImGuiKey_Z, true);
    expect(std::strcmp(editor.code, "0034") == 0, "native undo restores middle edit");
    editor.key(ImGuiKey_Y, true);
    expect(std::strcmp(editor.code, "00934") == 0, "native redo restores insertion");
    editor.key(ImGuiKey_A, true);
    expect(editor.state().HasSelection() && editor.selectionVisible,
           "select-all has visible selection geometry");
    clipboard = "code 00-12 345";
    editor.key(ImGuiKey_V, true);
    expect(std::strcmp(editor.code, "001234") == 0,
           "paste replaces selection and retains first six filtered digits");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_A, true);
    clipboard = "no digits here";
    editor.key(ImGuiKey_V, true);
    expect(std::strcmp(editor.code, "001234") == 0,
           "fully filtered paste cannot erase the existing code");
    editor.key(ImGuiKey_End);
    const float clickX = editor.minimum.x + ImGui::GetStyle().FramePadding.x +
        ImGui::CalcTextSize("001").x;
    const float clickY = (editor.minimum.y + editor.maximum.y) * 0.5f;
    ImGui::GetIO().AddMousePosEvent(clickX, clickY);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    editor.frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    editor.frame();
    editor.frame(); // allow queued mouse transitions to drain at normal trickle settings
    expect(editor.state().GetCursorPos() == 3, "click targets the displayed digit boundary");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_RightArrow, false, true);
    editor.key(ImGuiKey_RightArrow, false, true);
    expect(editor.state().HasSelection() && editor.selectionVisible,
           "middle selection stays visibly editable");
    editor.type("9");
    expect(std::strcmp(editor.code, "00194") == 0,
           "middle selection replacement preserves both sides");
    editor.checkVisibleEditing();
}

void testNarrowScrolling(float scale) {
    Editor editor(scale, 42.0f * scale);
    editor.type("001234");
    expect(editor.state().Scroll.x > 0.0f, "narrow editor scrolls real text");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_Home);
    expect(editor.state().GetCursorPos() == 0, "Home returns to leading zero");
    editor.checkVisibleEditing();
    editor.key(ImGuiKey_End);
    expect(editor.state().GetCursorPos() == 6, "End returns to final digit");
    editor.checkVisibleEditing();
}
} // namespace

int main() {
    for (float scale : {1.0f, 2.0f}) {
        testEditing(scale);
        testNarrowScrolling(scale);
    }
    std::puts("online join code input: PASS");
    return 0;
}
