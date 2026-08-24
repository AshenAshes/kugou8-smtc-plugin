#include "media_control.h"

#include "metadata_source.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace plugin {
namespace {

constexpr std::uintptr_t kPreviousControlOffset = 0x478;
constexpr std::uintptr_t kNextControlOffset = 0x47C;
constexpr std::uintptr_t kPlayPauseControlOffset = 0x480;
constexpr std::uintptr_t kPlaybackServiceOffset = 0x550;
constexpr std::uintptr_t kControlNameOffset = 0x3C;
constexpr wchar_t kBridgeWindowClassName[] =
    L"KuGouSmtcPlugin.HiddenWindow.v1";
constexpr wchar_t kUiControlMessageName[] =
    L"KuGouSmtcPlugin.UiMediaControl.v1";
constexpr wchar_t kControlCompletionMessageName[] =
    L"KuGouSmtcPlugin.MediaControlCompletion.v1";
constexpr ULONGLONG kCommandStatusTrustMs = 30000;

HHOOK g_ui_thread_hook = nullptr;
DWORD g_ui_thread_id = 0;
HWND g_ui_target_window = nullptr;

UINT UiControlMessage() noexcept {
    static const UINT message =
        RegisterWindowMessageW(kUiControlMessageName);
    return message;
}

bool IsValidAction(WPARAM value) noexcept {
    return value <= static_cast<WPARAM>(MediaControlAction::next);
}

struct UiWindowCandidate final {
    DWORD process_id = 0;
    DWORD worker_thread_id = 0;
    HWND window = nullptr;
    std::uint64_t score = 0;
};

BOOL CALLBACK FindUiWindowCallback(HWND window,
                                   LPARAM parameter) noexcept {
    auto* candidate = reinterpret_cast<UiWindowCandidate*>(parameter);
    if (candidate == nullptr) {
        return FALSE;
    }

    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    if (process_id != candidate->process_id ||
        thread_id == candidate->worker_thread_id) {
        return TRUE;
    }

    std::array<wchar_t, 128> class_name{};
    if (GetClassNameW(window, class_name.data(),
                      static_cast<int>(class_name.size())) != 0 &&
        wcscmp(class_name.data(), kBridgeWindowClassName) == 0) {
        return TRUE;
    }

    RECT bounds{};
    std::uint64_t area = 0;
    if (GetWindowRect(window, &bounds)) {
        const auto width = (std::max)(0L, bounds.right - bounds.left);
        const auto height = (std::max)(0L, bounds.bottom - bounds.top);
        area = static_cast<std::uint64_t>(width) *
               static_cast<std::uint64_t>(height);
    }
    const std::uint64_t score =
        (IsWindowVisible(window) ? (1ULL << 62) : 0) +
        (GetWindow(window, GW_OWNER) == nullptr ? (1ULL << 61) : 0) +
        (GetWindowTextLengthW(window) > 0 ? (1ULL << 60) : 0) +
        (std::min)(area, (1ULL << 60) - 1);
    if (candidate->window == nullptr || score > candidate->score) {
        candidate->window = window;
        candidate->score = score;
    }
    return TRUE;
}

HWND FindUiWindow(DWORD* thread_id) noexcept {
    UiWindowCandidate candidate{GetCurrentProcessId(),
                                GetCurrentThreadId()};
    EnumWindows(&FindUiWindowCallback,
                reinterpret_cast<LPARAM>(&candidate));
    if (candidate.window != nullptr && thread_id != nullptr) {
        *thread_id = GetWindowThreadProcessId(candidate.window, nullptr);
    }
    return candidate.window;
}

bool ExecuteMediaControlInsideUiThread(
    MediaControlAction action) noexcept;

LRESULT CALLBACK MediaControlUiHook(int code, WPARAM wparam,
                                    LPARAM lparam) noexcept {
    if (code == HC_ACTION && wparam == PM_REMOVE && lparam != 0) {
        auto* event = reinterpret_cast<MSG*>(lparam);
        if (event->message == UiControlMessage() &&
            IsValidAction(event->wParam)) {
            const auto action =
                static_cast<MediaControlAction>(event->wParam);
            const HWND completion_window =
                reinterpret_cast<HWND>(event->lParam);
            const bool executed =
                ExecuteMediaControlInsideUiThread(action);
            if (completion_window != nullptr) {
                PostMessageW(completion_window,
                             MediaControlCompletionMessage(),
                             static_cast<WPARAM>(action),
                             executed ? 1 : 0);
            }
            event->message = WM_NULL;
            event->wParam = 0;
            event->lParam = 0;
        }
    }
    return CallNextHookEx(g_ui_thread_hook, code, wparam, lparam);
}

void ResetUiThreadHook() noexcept {
    if (g_ui_thread_hook != nullptr) {
        UnhookWindowsHookEx(g_ui_thread_hook);
    }
    g_ui_thread_hook = nullptr;
    g_ui_thread_id = 0;
    g_ui_target_window = nullptr;
}

bool EnsureUiThreadHook() noexcept {
    if (g_ui_thread_hook != nullptr &&
        IsWindow(g_ui_target_window) &&
        GetWindowThreadProcessId(g_ui_target_window, nullptr) ==
            g_ui_thread_id) {
        return true;
    }
    ResetUiThreadHook();

    DWORD thread_id = 0;
    const HWND target = FindUiWindow(&thread_id);
    if (target == nullptr || thread_id == 0) {
        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&MediaControlUiHook), &module)) {
        return false;
    }
    const HHOOK hook = SetWindowsHookExW(
        MediaControlHookType(), &MediaControlUiHook, module, thread_id);
    if (hook == nullptr) {
        return false;
    }
    g_ui_thread_hook = hook;
    g_ui_thread_id = thread_id;
    g_ui_target_window = target;
    return true;
}

bool ReadPointer(std::uintptr_t address,
                 std::uintptr_t* value) noexcept {
    SIZE_T bytes_read = 0;
    return value != nullptr &&
           ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(address), value,
                             sizeof(*value), &bytes_read) &&
           bytes_read == sizeof(*value);
}

bool HasControlName(std::uintptr_t control,
                    const wchar_t* expected) {
    return control != 0 &&
           metadata::DecodeLegacyMsvcWstring(
               reinterpret_cast<const void*>(control + kControlNameOffset)) ==
               expected;
}

bool IsExecutableAddress(std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (address == 0 ||
        VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                     sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ValidatePlaybackPanel(std::uintptr_t panel) {
    std::uintptr_t previous = 0;
    std::uintptr_t next = 0;
    std::uintptr_t play_pause = 0;
    std::uintptr_t service = 0;
    std::uintptr_t vtable = 0;
    if (!ReadPointer(panel + kPreviousControlOffset, &previous) ||
        !ReadPointer(panel + kNextControlOffset, &next) ||
        !ReadPointer(panel + kPlayPauseControlOffset, &play_pause) ||
        !ReadPointer(panel + kPlaybackServiceOffset, &service) ||
        !ReadPointer(service, &vtable)) {
        return false;
    }

    return HasControlName(previous, L"PlaybackControlPanelPlayPrev") &&
           HasControlName(next, L"PlaybackControlPanelPlayNext") &&
           HasControlName(play_pause, L"PlaybackControlPanelPlayPause") &&
           vtable != 0;
}

bool IsScannablePointerMemory(
    const MEMORY_BASIC_INFORMATION& memory) noexcept {
    if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY;
}

std::uintptr_t FindPlaybackPanel() {
    const std::uintptr_t play_pause =
        metadata::FindNamedControlAddress(
            L"PlaybackControlPanelPlayPause");
    if (play_pause == 0) {
        return 0;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
        system_info.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(
        system_info.lpMaximumApplicationAddress);
    constexpr size_t kChunkBytes = 1024 * 1024;
    constexpr size_t kOverlap = 0x600;
    std::vector<std::uint8_t> buffer(kChunkBytes + kOverlap);

    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                         sizeof(memory)) == 0) {
            break;
        }

        const auto region_start = reinterpret_cast<std::uintptr_t>(
            memory.BaseAddress);
        const size_t region_size = memory.RegionSize;
        if (IsScannablePointerMemory(memory)) {
            size_t region_offset = 0;
            while (region_offset < region_size) {
                const size_t remaining = region_size - region_offset;
                const size_t requested =
                    (std::min)(buffer.size(), remaining);
                SIZE_T bytes_read = 0;
                const std::uintptr_t chunk_address =
                    region_start + region_offset;
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(chunk_address),
                        buffer.data(), requested, &bytes_read)) {
                    for (size_t offset = kPlayPauseControlOffset;
                         offset + sizeof(std::uintptr_t) <= bytes_read;
                         offset += sizeof(std::uintptr_t)) {
                        std::uintptr_t value = 0;
                        std::memcpy(&value, buffer.data() + offset,
                                    sizeof(value));
                        if (value != play_pause) {
                            continue;
                        }
                        const std::uintptr_t panel =
                            chunk_address + offset -
                            kPlayPauseControlOffset;
                        if (ValidatePlaybackPanel(panel)) {
                            return panel;
                        }
                    }
                }

                if (remaining <= kChunkBytes) {
                    break;
                }
                region_offset += kChunkBytes;
                if (region_offset >= kOverlap) {
                    region_offset -= kOverlap;
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

bool InvokePlaybackCallback(std::uintptr_t panel,
                            std::uintptr_t callback_rva) noexcept {
    __try {
        const HMODULE kugou_module = GetModuleHandleW(L"kugou.dll");
        if (kugou_module == nullptr || callback_rva == 0) {
            return false;
        }
        const std::uintptr_t callback =
            reinterpret_cast<std::uintptr_t>(kugou_module) + callback_rva;
        if (!IsExecutableAddress(callback)) {
            return false;
        }

        using ControlCallback = void(__thiscall*)(void*, int);
        reinterpret_cast<ControlCallback>(callback)(
            reinterpret_cast<void*>(panel), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ExecuteMediaControlInsideUiThread(
    MediaControlAction action) noexcept {
    try {
        const std::uintptr_t panel = FindPlaybackPanel();
        return panel != 0 &&
               InvokePlaybackCallback(
                   panel, InternalCommandCallbackRva(action));
    } catch (...) {
        return false;
    }
}

}  // namespace

bool MediaControlEchoGuard::TryEnter(ULONGLONG now) noexcept {
    constexpr ULONGLONG kEchoSuppressionMs = 250;
    if (now < blocked_until_) {
        return false;
    }
    blocked_until_ = now + kEchoSuppressionMs;
    return true;
}

void PlaybackCommandStatusGuard::RecordSuccessfulCommand(
    MediaControlAction action, ULONGLONG now) noexcept {
    expected_playing_ = RequestedPlaybackState(action);
    expires_at_ = expected_playing_.has_value()
                      ? now + kCommandStatusTrustMs
                      : 0;
}

std::optional<bool> PlaybackCommandStatusGuard::Resolve(
    std::optional<bool> observed_playing, ULONGLONG now) noexcept {
    if (!expected_playing_.has_value()) {
        return observed_playing;
    }
    if (observed_playing.has_value() &&
        *observed_playing == *expected_playing_) {
        expected_playing_.reset();
        expires_at_ = 0;
        return observed_playing;
    }
    if (now < expires_at_) {
        return expected_playing_;
    }
    expected_playing_.reset();
    expires_at_ = 0;
    return observed_playing;
}

std::size_t InternalCommandVtableOffset(
    MediaControlAction action) noexcept {
    switch (action) {
        case MediaControlAction::next:
            return 0x40;
        case MediaControlAction::play:
        case MediaControlAction::pause:
            return 0x44;
        case MediaControlAction::previous:
            return 0x48;
    }
    return 0;
}

std::uintptr_t InternalCommandCallbackRva(
    MediaControlAction action) noexcept {
    switch (action) {
        case MediaControlAction::previous:
            return 0x63247D;
        case MediaControlAction::next:
            return 0x632491;
        case MediaControlAction::play:
        case MediaControlAction::pause:
            return 0x6324A5;
    }
    return 0;
}

std::optional<bool> RequestedPlaybackState(
    MediaControlAction action) noexcept {
    switch (action) {
        case MediaControlAction::pause:
            return false;
        case MediaControlAction::play:
        case MediaControlAction::previous:
        case MediaControlAction::next:
            return true;
    }
    return std::nullopt;
}

ProcessRole MediaControlExecutionRole() noexcept {
    return ProcessRole::publisher;
}

MediaControlExecutionContext MediaControlRequiredContext() noexcept {
    return MediaControlExecutionContext::kugou_ui_thread;
}

int MediaControlHookType() noexcept {
    return WH_GETMESSAGE;
}

UINT MediaControlCompletionMessage() noexcept {
    static const UINT message =
        RegisterWindowMessageW(kControlCompletionMessageName);
    return message;
}

KuGouMediaController::~KuGouMediaController() {
    ResetUiThreadHook();
}

bool KuGouMediaController::Dispatch(
    MediaControlAction action,
    ProcessRole current_process_role,
    HWND completion_window) const noexcept {
    try {
        const UINT message = UiControlMessage();
        return current_process_role == MediaControlExecutionRole() &&
               completion_window != nullptr && message != 0 &&
               EnsureUiThreadHook() &&
               PostMessageW(g_ui_target_window, message,
                            static_cast<WPARAM>(action),
                            reinterpret_cast<LPARAM>(completion_window));
    } catch (...) {
        return false;
    }
}

}  // namespace plugin
