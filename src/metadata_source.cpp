#include "metadata_source.h"
#include "runtime_policy.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string_view>
#include <utility>
#include <vector>

namespace plugin::metadata {
namespace {

constexpr std::wstring_view kPlaybackControlName =
    L"PlaybackControlPanelSongStatus";
constexpr size_t kControlNameOffset = 0x3C;
constexpr size_t kControlTextOffset = 0x54;
constexpr size_t kLegacyStringObjectSize = 24;

std::uintptr_t g_playback_control = 0;
ULONGLONG g_next_control_scan_tick = 0;
MetadataScanBackoff g_control_scan_backoff;

struct ModuleRange {
    std::uintptr_t start;
    std::uintptr_t end;
};

struct ControlMatch {
    bool found;
    size_t control_offset;
    std::wstring text;
};

bool IsScannableControlMemory(const MEMORY_BASIC_INFORMATION& memory) {
    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) {
        return false;
    }
    const DWORD protection = memory.Protect;
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD basic = protection & 0xFF;
    return basic == PAGE_READWRITE || basic == PAGE_WRITECOPY;
}

void RecordControlScanFailure(ULONGLONG now) noexcept {
    g_next_control_scan_tick =
        now + g_control_scan_backoff.RecordFailure();
}

void RecordControlScanSuccess() noexcept {
    g_control_scan_backoff.Reset();
    g_next_control_scan_tick = 0;
}

std::vector<ModuleRange> FindControlVtableRanges() {
    std::vector<ModuleRange> ranges;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ranges;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"ui.dll") == 0 ||
                _wcsicmp(entry.szModule, L"kugou.dll") == 0) {
                const auto start = reinterpret_cast<std::uintptr_t>(
                    entry.modBaseAddr);
                ranges.push_back(
                    {start, start + static_cast<size_t>(entry.modBaseSize)});
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ranges;
}

ControlMatch FindNamedControlTextInRange(
    const void* range_start, size_t range_size, std::uintptr_t vtable_start,
    std::uintptr_t vtable_end, const std::wstring& control_name) {
    if (range_start == nullptr || range_size < kControlTextOffset +
                                                       kLegacyStringObjectSize ||
        control_name.empty()) {
        return {};
    }

    const auto* bytes = static_cast<const std::uint8_t*>(range_start);
    const size_t control_name_size = control_name.size();
    for (size_t name_offset = kControlNameOffset;
         name_offset + kLegacyStringObjectSize <= range_size;
         name_offset += sizeof(std::uint32_t)) {
        std::uint32_t size = 0;
        std::uint32_t capacity = 0;
        std::memcpy(&size, bytes + name_offset + 16, sizeof(size));
        if (size != control_name_size) {
            continue;
        }
        std::memcpy(&capacity, bytes + name_offset + 20,
                    sizeof(capacity));
        if (capacity < size || capacity > 4096) {
            continue;
        }

        const size_t control_offset = name_offset - kControlNameOffset;
        if (control_offset + kControlTextOffset +
                kLegacyStringObjectSize >
            range_size) {
            continue;
        }

        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, bytes + control_offset, sizeof(vtable));
        if (vtable < vtable_start || vtable >= vtable_end) {
            continue;
        }

        if (DecodeLegacyMsvcWstring(bytes + name_offset) != control_name) {
            continue;
        }

        std::wstring text = DecodeLegacyMsvcWstring(
            bytes + control_offset + kControlTextOffset);
        return {true, control_offset, std::move(text)};
    }
    return {};
}

}  // namespace

std::wstring DecodeLegacyMsvcWstring(const void* string_object) {
    constexpr size_t kInlineCharacterCapacity = 8;
    constexpr size_t kMaximumCharacters = 512;
    constexpr size_t kStorageBytes =
        kInlineCharacterCapacity * sizeof(wchar_t);
    constexpr size_t kObjectBytes =
        kStorageBytes + sizeof(std::uint32_t) * 2;

    if (string_object == nullptr || sizeof(void*) != sizeof(std::uint32_t)) {
        return {};
    }

    std::array<std::uint8_t, kObjectBytes> object{};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), string_object, object.data(),
                           object.size(), &bytes_read) ||
        bytes_read != object.size()) {
        return {};
    }

    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
    std::memcpy(&size, object.data() + kStorageBytes, sizeof(size));
    std::memcpy(&capacity,
                object.data() + kStorageBytes + sizeof(size),
                sizeof(capacity));
    if (size == 0 || size > kMaximumCharacters || capacity < size) {
        return {};
    }

    if (capacity < kInlineCharacterCapacity) {
        const auto* characters =
            reinterpret_cast<const wchar_t*>(object.data());
        return std::wstring(characters, characters + size);
    }

    const wchar_t* characters = nullptr;
    std::memcpy(&characters, object.data(), sizeof(characters));
    if (characters == nullptr) {
        return {};
    }

    std::wstring result(size, L'\0');
    bytes_read = 0;
    const size_t byte_count = static_cast<size_t>(size) * sizeof(wchar_t);
    if (!ReadProcessMemory(GetCurrentProcess(), characters, result.data(),
                           byte_count, &bytes_read) ||
        bytes_read != byte_count) {
        return {};
    }
    return result;
}

std::wstring ReadNamedControlTextInRange(
    const void* range_start, size_t range_size, std::uintptr_t vtable_start,
    std::uintptr_t vtable_end, const std::wstring& control_name) {
    return FindNamedControlTextInRange(range_start, range_size, vtable_start,
                                       vtable_end, control_name)
        .text;
}

std::uintptr_t FindNamedControlAddress(const std::wstring& control_name) {
    if (control_name.empty()) {
        return 0;
    }
    const std::vector<ModuleRange> vtable_ranges =
        FindControlVtableRanges();
    if (vtable_ranges.empty()) {
        return 0;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
        system_info.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(
        system_info.lpMaximumApplicationAddress);
    constexpr size_t kScanChunkBytes = 1024 * 1024;
    constexpr size_t kChunkOverlap = 128;
    std::vector<std::uint8_t> buffer(kScanChunkBytes + kChunkOverlap);

    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                         sizeof(memory)) == 0) {
            break;
        }

        const auto region_start = reinterpret_cast<std::uintptr_t>(
            memory.BaseAddress);
        const size_t region_size = memory.RegionSize;
        if (IsScannableControlMemory(memory)) {
            size_t region_offset = 0;
            while (region_offset < region_size) {
                const size_t remaining = region_size - region_offset;
                const size_t requested =
                    (std::min)(buffer.size(), remaining);
                SIZE_T bytes_read = 0;
                const auto chunk_address = region_start + region_offset;
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(chunk_address),
                        buffer.data(), requested, &bytes_read) &&
                    bytes_read >= kControlTextOffset +
                                      kLegacyStringObjectSize) {
                    for (const ModuleRange& vtable_range : vtable_ranges) {
                        const ControlMatch match = FindNamedControlTextInRange(
                            buffer.data(), bytes_read, vtable_range.start,
                            vtable_range.end, control_name);
                        if (match.found) {
                            return chunk_address + match.control_offset;
                        }
                    }
                }

                if (remaining <= kScanChunkBytes) {
                    break;
                }
                region_offset += kScanChunkBytes;
                if (region_offset >= kChunkOverlap) {
                    region_offset -= kChunkOverlap;
                }
            }
        }

        if (region_size == 0 || region_start + region_size <= address) {
            break;
        }
        address = region_start + region_size;
    }
    return 0;
}

std::wstring ReadPlaybackControlTitle(bool* found) {
    if (found != nullptr) {
        *found = false;
    }
    if (g_playback_control != 0) {
        const auto* control =
            reinterpret_cast<const std::uint8_t*>(g_playback_control);
        if (DecodeLegacyMsvcWstring(control + kControlNameOffset) ==
            kPlaybackControlName) {
            if (found != nullptr) {
                *found = true;
            }
            return DecodeLegacyMsvcWstring(control + kControlTextOffset);
        }
        g_playback_control = 0;
        RecordControlScanSuccess();
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_next_control_scan_tick) {
        return {};
    }

    const std::vector<ModuleRange> vtable_ranges =
        FindControlVtableRanges();
    if (vtable_ranges.empty()) {
        RecordControlScanFailure(now);
        return {};
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
        system_info.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(
        system_info.lpMaximumApplicationAddress);
    constexpr size_t kScanChunkBytes = 1024 * 1024;
    constexpr size_t kChunkOverlap = 128;
    std::vector<std::uint8_t> buffer(kScanChunkBytes + kChunkOverlap);
    const std::wstring control_name(kPlaybackControlName);

    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                         sizeof(memory)) == 0) {
            break;
        }

        const auto region_start = reinterpret_cast<std::uintptr_t>(
            memory.BaseAddress);
        const size_t region_size = memory.RegionSize;
        if (IsScannableControlMemory(memory)) {
            size_t region_offset = 0;
            while (region_offset < region_size) {
                const size_t remaining = region_size - region_offset;
                const size_t requested =
                    (std::min)(buffer.size(), remaining);
                SIZE_T bytes_read = 0;
                const auto chunk_address = region_start + region_offset;
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(chunk_address),
                        buffer.data(), requested, &bytes_read) &&
                    bytes_read >= kControlTextOffset +
                                      kLegacyStringObjectSize) {
                    for (const ModuleRange& vtable_range : vtable_ranges) {
                        const ControlMatch match = FindNamedControlTextInRange(
                            buffer.data(), bytes_read, vtable_range.start,
                            vtable_range.end, control_name);
                        if (match.found) {
                            g_playback_control =
                                chunk_address + match.control_offset;
                            if (found != nullptr) {
                                *found = true;
                            }
                            RecordControlScanSuccess();
                            return match.text;
                        }
                    }
                }

                if (remaining <= kScanChunkBytes) {
                    break;
                }
                region_offset += kScanChunkBytes;
                if (region_offset >= kChunkOverlap) {
                    region_offset -= kChunkOverlap;
                }
            }
        }

        if (region_size == 0 || region_start + region_size <= address) {
            break;
        }
        address = region_start + region_size;
    }
    RecordControlScanFailure(now);
    return {};
}

}  // namespace plugin::metadata
