#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uintptr_t kControlNameOffset = 0x3C;
constexpr std::uintptr_t kPreviousControlOffset = 0x478;
constexpr std::uintptr_t kNextControlOffset = 0x47C;
constexpr std::uintptr_t kPlayPauseControlOffset = 0x480;
constexpr std::uintptr_t kPlaybackServiceOffset = 0x550;

struct ModuleRange {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
};

struct NamedControl {
    std::wstring name;
    std::uintptr_t address = 0;
};

struct WindowProbeContext {
    DWORD process_id = 0;
};

BOOL CALLBACK PrintProcessWindow(HWND window, LPARAM parameter) {
    const auto* context = reinterpret_cast<const WindowProbeContext*>(
        parameter);
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    if (context == nullptr || process_id != context->process_id) {
        return TRUE;
    }
    std::array<wchar_t, 256> class_name{};
    std::array<wchar_t, 256> title{};
    GetClassNameW(window, class_name.data(),
                  static_cast<int>(class_name.size()));
    GetWindowTextW(window, title.data(),
                   static_cast<int>(title.size()));
    std::wcout << L"  window hwnd=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(window) << std::dec
               << L" tid=" << thread_id
               << L" visible=" << IsWindowVisible(window)
               << L" class=" << class_name.data()
               << L" title=" << title.data() << L'\n';
    return TRUE;
}

class Handle final {
public:
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_ = nullptr;
};

template <typename T>
bool ReadValue(HANDLE process, std::uintptr_t address, T* value) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             value, sizeof(*value), &read) &&
           read == sizeof(*value);
}

std::wstring DecodeString(HANDLE process, std::uintptr_t address) {
    std::array<std::uint8_t, 24> object{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                           object.data(), object.size(), &read) ||
        read != object.size()) {
        return {};
    }
    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
    std::memcpy(&size, object.data() + 16, sizeof(size));
    std::memcpy(&capacity, object.data() + 20, sizeof(capacity));
    if (size == 0 || size > 512 || capacity < size) {
        return {};
    }
    if (capacity < 8) {
        return std::wstring(reinterpret_cast<const wchar_t*>(object.data()),
                            size);
    }
    std::uint32_t pointer = 0;
    std::memcpy(&pointer, object.data(), sizeof(pointer));
    std::wstring result(size, L'\0');
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(pointer),
                           result.data(), size * sizeof(wchar_t), &read) ||
        read != size * sizeof(wchar_t)) {
        return {};
    }
    return result;
}

bool InRanges(std::uintptr_t address,
              const std::vector<ModuleRange>& ranges) {
    return std::any_of(ranges.begin(), ranges.end(),
                       [address](const ModuleRange& range) {
                           return address >= range.start &&
                                  address < range.end;
                       });
}

std::vector<ModuleRange> ControlModuleRanges(DWORD process_id) {
    std::vector<ModuleRange> ranges;
    const Handle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        std::wcout << L"  module snapshot failed error="
                   << GetLastError() << L'\n';
        return ranges;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"ui.dll") == 0 ||
                _wcsicmp(entry.szModule, L"kugou.dll") == 0) {
                const auto start = reinterpret_cast<std::uintptr_t>(
                    entry.modBaseAddr);
                ranges.push_back(
                    {start, start + static_cast<size_t>(entry.modBaseSize)});
                std::wcout << L"  module " << entry.szModule << L" 0x"
                           << std::hex << start << L"-0x"
                           << ranges.back().end << std::dec << L'\n';
            }
        } while (Module32NextW(snapshot.get(), &entry));
    } else {
        std::wcout << L"  Module32First failed error="
                   << GetLastError() << L'\n';
    }
    return ranges;
}

bool IsPrivateWritable(const MEMORY_BASIC_INFORMATION& memory) {
    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
}

std::vector<NamedControl> FindControls(
    HANDLE process, const std::vector<ModuleRange>& ranges,
    const std::vector<std::wstring>& names) {
    std::vector<NamedControl> matches;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
        system.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(
        system.lpMaximumApplicationAddress);
    constexpr size_t kChunk = 1024 * 1024;
    constexpr size_t kOverlap = 128;
    std::vector<std::uint8_t> buffer(kChunk + kOverlap);

    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                           &memory, sizeof(memory)) == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (IsPrivateWritable(memory)) {
            size_t offset = 0;
            while (offset < memory.RegionSize) {
                const size_t remaining = memory.RegionSize - offset;
                const size_t requested =
                    (std::min)(buffer.size(), remaining);
                SIZE_T read = 0;
                const std::uintptr_t chunk_address = base + offset;
                if (ReadProcessMemory(
                        process, reinterpret_cast<const void*>(chunk_address),
                        buffer.data(), requested, &read)) {
                    for (size_t name_offset = kControlNameOffset;
                         name_offset + 24 <= read;
                         name_offset += sizeof(std::uint32_t)) {
                        std::uint32_t size = 0;
                        std::uint32_t capacity = 0;
                        std::memcpy(&size, buffer.data() + name_offset + 16,
                                    sizeof(size));
                        std::memcpy(&capacity,
                                    buffer.data() + name_offset + 20,
                                    sizeof(capacity));
                        if (capacity < size || capacity > 4096) {
                            continue;
                        }
                        const size_t control_offset =
                            name_offset - kControlNameOffset;
                        std::uint32_t vtable = 0;
                        std::memcpy(&vtable,
                                    buffer.data() + control_offset,
                                    sizeof(vtable));
                        if (!InRanges(vtable, ranges)) {
                            continue;
                        }
                        const auto object = chunk_address + control_offset;
                        const std::wstring decoded = DecodeString(
                            process, object + kControlNameOffset);
                        if (std::find(names.begin(), names.end(), decoded) !=
                            names.end()) {
                            const bool duplicate = std::any_of(
                                matches.begin(), matches.end(),
                                [object](const NamedControl& match) {
                                    return match.address == object;
                                });
                            if (!duplicate) {
                                matches.push_back({decoded, object});
                            }
                        }
                    }
                }
                if (remaining <= kChunk) {
                    break;
                }
                offset += kChunk - kOverlap;
            }
        }
        if (memory.RegionSize == 0 ||
            base + memory.RegionSize <= address) {
            break;
        }
        address = base + memory.RegionSize;
    }
    return matches;
}

bool HasName(HANDLE process, std::uintptr_t control,
             const wchar_t* expected) {
    return control != 0 &&
           DecodeString(process, control + kControlNameOffset) == expected;
}

void ProbePanelOwners(HANDLE process, std::uintptr_t play_pause) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
        system.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(
        system.lpMaximumApplicationAddress);
    constexpr size_t kChunk = 1024 * 1024;
    constexpr size_t kOverlap = 0x600;
    std::vector<std::uint8_t> buffer(kChunk + kOverlap);
    unsigned candidates = 0;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(address),
                           &memory, sizeof(memory)) == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (IsPrivateWritable(memory)) {
            size_t offset = 0;
            while (offset < memory.RegionSize) {
                const size_t remaining = memory.RegionSize - offset;
                const size_t requested =
                    (std::min)(buffer.size(), remaining);
                SIZE_T read = 0;
                const std::uintptr_t chunk_address = base + offset;
                if (ReadProcessMemory(
                        process, reinterpret_cast<const void*>(chunk_address),
                        buffer.data(), requested, &read)) {
                    for (size_t item = 0; item + sizeof(std::uint32_t) <= read;
                         item += sizeof(std::uint32_t)) {
                        std::uint32_t value = 0;
                        std::memcpy(&value, buffer.data() + item,
                                    sizeof(value));
                        if (value != play_pause ||
                            item < kPlayPauseControlOffset) {
                            continue;
                        }
                        ++candidates;
                        const std::uintptr_t panel = chunk_address + item -
                                                     kPlayPauseControlOffset;
                        std::uint32_t previous = 0;
                        std::uint32_t next = 0;
                        std::uint32_t service = 0;
                        ReadValue(process, panel + kPreviousControlOffset,
                                  &previous);
                        ReadValue(process, panel + kNextControlOffset, &next);
                        ReadValue(process, panel + kPlaybackServiceOffset,
                                  &service);
                        std::wcout << L"  panel candidate 0x" << std::hex
                                   << panel << L" prev=0x" << previous
                                   << L" next=0x" << next << L" service=0x"
                                   << service << std::dec
                                   << L" names="
                                   << HasName(process, previous,
                                              L"PlaybackControlPanelPlayPrev")
                                   << L"," << HasName(
                                          process, next,
                                          L"PlaybackControlPanelPlayNext")
                                   << L'\n';
                    }
                }
                if (remaining <= kChunk) {
                    break;
                }
                offset += kChunk - kOverlap;
            }
        }
        if (memory.RegionSize == 0 ||
            base + memory.RegionSize <= address) {
            break;
        }
        address = base + memory.RegionSize;
    }
    std::wcout << L"  panel pointer candidates=" << candidates << L'\n';
}

std::wstring ProcessPath(HANDLE process) {
    std::array<wchar_t, 1024> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        return {};
    }
    return std::wstring(path.data(), length);
}

}  // namespace

int wmain() {
    const std::vector<std::wstring> names = {
        L"PlaybackControlPanelSongStatus",
        L"PlaybackControlPanelPlayPrev",
        L"PlaybackControlPanelPlayNext",
        L"PlaybackControlPanelPlayPause"};
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return 1;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned processes = 0;
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"KuGou.exe") != 0) {
                continue;
            }
            const Handle process(OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                entry.th32ProcessID));
            if (process.get() == nullptr) {
                continue;
            }
            ++processes;
            std::wcout << L"PID " << entry.th32ProcessID << L" "
                       << ProcessPath(process.get()) << L'\n';
            const WindowProbeContext window_context{entry.th32ProcessID};
            EnumWindows(&PrintProcessWindow,
                        reinterpret_cast<LPARAM>(&window_context));
            const auto ranges = ControlModuleRanges(entry.th32ProcessID);
            const auto controls = FindControls(process.get(), ranges, names);
            for (const auto& control : controls) {
                std::wcout << L"  control " << control.name << L" 0x"
                           << std::hex << control.address << std::dec << L'\n';
            }
            std::wcout << L"  named controls=" << controls.size() << L'\n';
            for (const auto& control : controls) {
                if (control.name == L"PlaybackControlPanelPlayPause") {
                    ProbePanelOwners(process.get(), control.address);
                }
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    return processes == 0 ? 2 : 0;
}
