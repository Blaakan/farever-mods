#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "paths.h"
namespace fmk {
const std::wstring& game_dir() {
    static const std::wstring value = [] {
        wchar_t path[32768] = {};
        DWORD n = GetModuleFileNameW(nullptr, path, 32768);
        if (!n || n >= 32768) return std::wstring(L".\\");
        while (n && path[n - 1] != L'\\') --n;
        return n ? std::wstring(path, n) : std::wstring(L".\\");
    }();
    return value;
}
const std::wstring& data_dir() {
    static const std::wstring value = [] {
        std::wstring mods = game_dir() + L"mods";
        CreateDirectoryW(mods.c_str(), nullptr);
        mods += L"\\farever-mods";
        CreateDirectoryW(mods.c_str(), nullptr);
        mods += L"\\";
        return mods;
    }();
    return value;
}
}
