// file_dialog_win.cpp — GetOpenFileNameW. See file_dialog.h.
//
// comdlg32 rather than the newer COM IFileOpenDialog on purpose: comdlg32.dll
// is a stock Windows system DLL present on every install, whereas the COM route
// would pull ole32/shell32 initialization into the shell for no user-visible
// gain here. See the import-table note in docs/APP_SHELL.md — comdlg32.dll is a
// deliberate, documented addition to the windows-validate allowlist.
#include "file_dialog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

namespace filedialog {

namespace {

// UTF-16 -> UTF-8, so the rest of the shell keeps working in char paths.
std::string toUtf8(const wchar_t *w) {
    if (w == nullptr || w[0] == L'\0') return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::vector<char> buf((size_t)need);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), need, nullptr, nullptr) <= 0) {
        return std::string();
    }
    return std::string(buf.data());
}

}  // namespace

bool isAvailable() { return true; }

bool openRom(std::string &out) {
    // Double-NUL-terminated filter pairs, as the Win32 API expects.
    static const wchar_t kFilter[] =
        L"Nintendo 64 ROM images\0*.z64;*.n64;*.v64\0"
        L"All files\0*.*\0"
        L"\0";

    std::vector<wchar_t> file(32768, L'\0');   // room for a long path

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = file.data();
    ofn.nMaxFile    = (DWORD)file.size();
    ofn.lpstrTitle  = L"Choose a Nintendo 64 ROM";
    // NOCHANGEDIR matters: without it the dialog mutates the process working
    // directory, which would silently change how every later relative path in
    // the app resolves.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) {
        return false;   // cancelled, or the dialog failed — `out` untouched
    }

    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openCharacterSource(std::string &out) {
    static const wchar_t kFilter[] =
        L"Character sources (*.mdkrchar;*.glb;*.dae;*.zip;*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds)\0*.mdkrchar;*.glb;*.dae;*.zip;*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds\0"
        L"Golden Balloon packages (*.mdkrchar)\0*.mdkrchar\0"
        L"glTF binary models (*.glb)\0*.glb\0"
        L"COLLADA models (*.dae)\0*.dae\0"
        L"Authoring archives (*.zip)\0*.zip\0"
        L"DCC sources requiring GLB export (*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds)\0*.gltf;*.fbx;*.obj;*.blend;*.usd;*.usda;*.usdc;*.usdz;*.ma;*.mb;*.max;*.c4d;*.3ds\0"
        L"\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Import a custom character";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openCharacterLicense(std::string &out) {
    static const wchar_t kFilter[] =
        L"License and notice files\0LICENSE*;COPYING*;NOTICE*;*.txt;*.md\0"
        L"All files\0*.*\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Choose the character license or notice file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool openPortraitImage(std::string &out) {
    static const wchar_t kFilter[] = L"PNG images\0*.png\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Choose character portrait artwork";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterConvertedGlb(std::string &out) {
    static const wchar_t kFilter[] = L"glTF binary models\0*.glb\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"converted-character.glb";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Save converted character model";
    ofn.lpstrDefExt = L"glb";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterCapture(std::string &out) {
    static const wchar_t kFilter[] = L"PNG images\0*.png\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-inspection.png";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Save a custom character inspection";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

bool saveCharacterReport(std::string &out) {
    static const wchar_t kFilter[] = L"HTML documents\0*.html\0\0";
    std::vector<wchar_t> file(32768, L'\0');
    const wchar_t initial[] = L"character-visual-report.html";
    std::copy(std::begin(initial), std::end(initial), file.begin());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == nullptr) window = SDL_GetMouseFocus();
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
        SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
        windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        ofn.hwndOwner = windowInfo.info.win.window;
    }
    ofn.lpstrFilter = kFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Export custom character visual report";
    ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::string picked = toUtf8(file.data());
    if (picked.empty()) return false;
    out = picked;
    return true;
}

}  // namespace filedialog
