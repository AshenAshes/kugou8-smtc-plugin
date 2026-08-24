#pragma once

#include "runtime_policy.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace plugin {

enum class MediaControlAction {
    play,
    pause,
    previous,
    next,
};

enum class MediaControlExecutionContext {
    plugin_worker,
    kugou_ui_thread,
};

class MediaControlEchoGuard final {
public:
    bool TryEnter(ULONGLONG now) noexcept;

private:
    ULONGLONG blocked_until_ = 0;
};

class PlaybackCommandStatusGuard final {
public:
    void RecordSuccessfulCommand(MediaControlAction action,
                                 ULONGLONG now) noexcept;
    std::optional<bool> Resolve(std::optional<bool> observed_playing,
                                ULONGLONG now) noexcept;

private:
    std::optional<bool> expected_playing_;
    ULONGLONG expires_at_ = 0;
};

std::size_t InternalCommandVtableOffset(
    MediaControlAction action) noexcept;
std::uintptr_t InternalCommandCallbackRva(
    MediaControlAction action) noexcept;
std::optional<bool> RequestedPlaybackState(
    MediaControlAction action) noexcept;
ProcessRole MediaControlExecutionRole() noexcept;
MediaControlExecutionContext MediaControlRequiredContext() noexcept;
int MediaControlHookType() noexcept;
UINT MediaControlCompletionMessage() noexcept;

class KuGouMediaController final {
public:
    ~KuGouMediaController();

    bool Dispatch(MediaControlAction action,
                  ProcessRole current_process_role,
                  HWND completion_window) const noexcept;
};

}  // namespace plugin
