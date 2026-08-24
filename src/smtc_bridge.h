#pragma once

#include <windows.h>

#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

#include "audio_session.h"
#include "cover_art.h"
#include "media_control.h"
#include "metadata_transition_filter.h"
#include "shared_metadata.h"

namespace plugin {

class SmtcBridge final {
public:
    explicit SmtcBridge(HMODULE module) noexcept;
    ~SmtcBridge();

    SmtcBridge(const SmtcBridge&) = delete;
    SmtcBridge& operator=(const SmtcBridge&) = delete;

    int Run(bool require_playback_module = true);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) noexcept;

    bool CreateBridgeWindow();
    bool InitializeSmtc();
    void Poll() noexcept;
    void PublishMetadata(const std::wstring& raw_title);
    void PublishPlaybackStatus(bool playing);
    void TryPublishCoverArt() noexcept;
    void OnButtonPressed(
        winrt::Windows::Media::SystemMediaTransportControlsButton button)
        noexcept;
    void HandleMediaControl(MediaControlAction action) noexcept;
    void HandleMediaControlCompletion(MediaControlAction action,
                                      bool executed) noexcept;
    HMODULE module_ = nullptr;
    HWND window_ = nullptr;
    bool primary_process_ = false;
    SharedMetadata shared_metadata_;
    metadata::PlaybackMetadataFilter metadata_filter_;
    std::wstring last_raw_title_;
    bool last_playing_ = false;
    bool playback_initialized_ = false;
    AudioActivityMonitor audio_monitor_;
    KuGouMediaController media_controller_;
    MediaControlEchoGuard media_control_echo_guard_;
    PlaybackCommandStatusGuard playback_command_status_guard_;
    CoverArtResolver cover_art_resolver_;
    std::wstring current_artist_;
    std::wstring current_title_;
    std::wstring published_cover_path_;
    ULONGLONG cover_retry_until_ = 0;
    ULONGLONG next_cover_retry_ = 0;
    winrt::Windows::Media::SystemMediaTransportControls smtc_{nullptr};
    winrt::event_token button_pressed_token_{};
    bool button_handler_registered_ = false;
};

DWORD WINAPI PluginThreadEntry(void* module) noexcept;

}  // namespace plugin
