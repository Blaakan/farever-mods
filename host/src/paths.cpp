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
std::wstring character_data_dir(const std::string& account_uuid,
                                const std::string& character_id,
                                const std::string& character_name) {
    auto safe = [](const std::string& value, const char* fallback) {
        std::string out;
        for (char c : value)
            if (isalnum((unsigned char)c) || c == '_' || c == '-') out.push_back(c);
        return out.empty() ? std::string(fallback) : out;
    };
    std::wstring dir = data_dir() + L"data\\";
    CreateDirectoryW(dir.substr(0, dir.size() - 1).c_str(), nullptr);
    const std::string account = safe(account_uuid, "unknown-account");
    dir += std::wstring(account.begin(), account.end()) + L"\\";
    CreateDirectoryW(dir.substr(0, dir.size() - 1).c_str(), nullptr);
    const std::string folder = safe(character_name, "character");
    dir += std::wstring(folder.begin(), folder.end()) + L"\\";
    CreateDirectoryW(dir.substr(0, dir.size() - 1).c_str(), nullptr);
    return dir;
}}
