#include "forward_stubs.h"

#include <algorithm>
#include <array>
#include <string>

namespace {

HMODULE g_real_version = nullptr;

extern "C" {
FARPROC g_version_targets[17]{};
}

constexpr std::array<const char*, 17> kExportNames{
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

std::wstring BuildSystemVersionPath() {
    std::array<wchar_t, 1024> path{};
    const UINT length = GetSystemDirectoryW(
        path.data(), static_cast<UINT>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }

    std::wstring result(path.data(), length);
    if (!result.empty() && result.back() != L'\\') {
        result += L'\\';
    }
    result += L"version.dll";
    return result;
}

}  // namespace

namespace plugin {

bool InitializeVersionForwarding() noexcept {
    try {
        const std::wstring real_path = BuildSystemVersionPath();
        if (real_path.empty()) {
            return false;
        }

        g_real_version = LoadLibraryExW(
            real_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g_real_version == nullptr) {
            return false;
        }

        for (size_t index = 0; index < kExportNames.size(); ++index) {
            g_version_targets[index] =
                GetProcAddress(g_real_version, kExportNames[index]);
            if (g_version_targets[index] == nullptr) {
                FreeLibrary(g_real_version);
                g_real_version = nullptr;
                std::fill(std::begin(g_version_targets),
                          std::end(g_version_targets), nullptr);
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace plugin

#define VERSION_JUMP_STUB(index, name)               \
    extern "C" __declspec(naked) void Proxy_##name() { \
        __asm jmp dword ptr[g_version_targets + index * 4] \
    }

VERSION_JUMP_STUB(0, GetFileVersionInfoA)
VERSION_JUMP_STUB(1, GetFileVersionInfoByHandle)
VERSION_JUMP_STUB(2, GetFileVersionInfoExA)
VERSION_JUMP_STUB(3, GetFileVersionInfoExW)
VERSION_JUMP_STUB(4, GetFileVersionInfoSizeA)
VERSION_JUMP_STUB(5, GetFileVersionInfoSizeExA)
VERSION_JUMP_STUB(6, GetFileVersionInfoSizeExW)
VERSION_JUMP_STUB(7, GetFileVersionInfoSizeW)
VERSION_JUMP_STUB(8, GetFileVersionInfoW)
VERSION_JUMP_STUB(9, VerFindFileA)
VERSION_JUMP_STUB(10, VerFindFileW)
VERSION_JUMP_STUB(11, VerInstallFileA)
VERSION_JUMP_STUB(12, VerInstallFileW)
VERSION_JUMP_STUB(13, VerLanguageNameA)
VERSION_JUMP_STUB(14, VerLanguageNameW)
VERSION_JUMP_STUB(15, VerQueryValueA)
VERSION_JUMP_STUB(16, VerQueryValueW)

#undef VERSION_JUMP_STUB
