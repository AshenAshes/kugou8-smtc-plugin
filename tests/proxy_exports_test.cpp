#include <windows.h>

#include <array>
#include <filesystem>
#include <iostream>

int wmain() {
    constexpr std::array<const char*, 17> exports = {
        "GetFileVersionInfoA",
        "GetFileVersionInfoByHandle",
        "GetFileVersionInfoExA",
        "GetFileVersionInfoExW",
        "GetFileVersionInfoSizeA",
        "GetFileVersionInfoSizeExA",
        "GetFileVersionInfoSizeExW",
        "GetFileVersionInfoSizeW",
        "GetFileVersionInfoW",
        "VerFindFileA",
        "VerFindFileW",
        "VerInstallFileA",
        "VerInstallFileW",
        "VerLanguageNameA",
        "VerLanguageNameW",
        "VerQueryValueA",
        "VerQueryValueW",
    };

    std::array<wchar_t, 1024> executable_path{};
    const DWORD executable_path_length = GetModuleFileNameW(
        nullptr, executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (executable_path_length == 0 ||
        executable_path_length >= executable_path.size()) {
        std::wcerr << L"Could not locate the test executable\n";
        return 1;
    }
    const auto legacy_companion =
        std::filesystem::path(executable_path.data()).parent_path() /
        L"version_original.dll";
    if (std::filesystem::exists(legacy_companion)) {
        std::wcerr << L"Legacy version_original.dll would hide a forwarding "
                      L"regression\n";
        return 1;
    }

    HMODULE proxy = LoadLibraryW(L"version.dll");
    if (proxy == nullptr) {
        std::wcerr << L"LoadLibrary failed: " << GetLastError() << L'\n';
        return 1;
    }

    bool success = true;
    for (const char* name : exports) {
        if (GetProcAddress(proxy, name) == nullptr) {
            std::cerr << "Missing export: " << name << '\n';
            success = false;
        }
    }

    using GetSize = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    const auto get_size = reinterpret_cast<GetSize>(
        GetProcAddress(proxy, "GetFileVersionInfoSizeW"));
    DWORD ignored = 0;
    if (get_size == nullptr || get_size(L"version.dll", &ignored) == 0) {
        std::wcerr << L"Forwarded GetFileVersionInfoSizeW call failed: "
                   << GetLastError() << L'\n';
        success = false;
    }

    FreeLibrary(proxy);
    if (!success) {
        return 2;
    }

    std::cout << "All 17 exports resolve and the forwarding smoke test passed.\n";
    return 0;
}
