#include "smtc_bridge.h"

#include "audio_session.h"
#include "cover_art.h"
#include "logging.h"
#include "media_control.h"
#include "metadata_source.h"
#include "runtime_policy.h"

#include <windows.h>
#include <windows.media.h>
#include <SystemMediaTransportControlsInterop.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace plugin {
namespace {

constexpr wchar_t kWindowClassName[] = L"KuGouSmtcPlugin.HiddenWindow.v1";
constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollIntervalMs = 500;
constexpr UINT kMediaControlMessage = WM_APP + 0x150;
constexpr ULONGLONG kCoverRetryDurationMs = 10000;
constexpr ULONGLONG kCoverRetryIntervalMs = 750;

void WaitForPlaybackModule() noexcept {
    const auto wait_started = std::chrono::steady_clock::now();
    for (;;) {
        if (GetModuleHandleW(L"kgplayer.dll") != nullptr) {
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - wait_started;
        auto delay = std::chrono::milliseconds(250);
        if (elapsed >= std::chrono::minutes(1)) {
            delay = std::chrono::seconds(5);
        } else if (elapsed >= std::chrono::seconds(10)) {
            delay = std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(delay);
    }
}

ProcessRole CurrentProcessRole() noexcept {
    std::array<wchar_t, 1024> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return ProcessRole::disabled;
    }
    return ClassifyProcessPath(std::wstring(path.data(), length));
}

std::pair<std::wstring, std::wstring> SplitArtistAndTitle(
    const std::wstring& value) {
    constexpr std::wstring_view separator = L" - ";
    const auto separator_index = value.find(separator);
    if (separator_index == std::wstring::npos) {
        return {L"", value};
    }

    std::wstring artist = value.substr(0, separator_index);
    std::wstring title = value.substr(separator_index + separator.size());
    if (title.empty()) {
        return {L"", value};
    }
    return {std::move(artist), std::move(title)};
}

}  // namespace

SmtcBridge::SmtcBridge(HMODULE module) noexcept : module_(module) {}

SmtcBridge::~SmtcBridge() {
    try {
        if (smtc_) {
            if (button_handler_registered_) {
                smtc_.ButtonPressed(button_pressed_token_);
                button_handler_registered_ = false;
            }
            smtc_.PlaybackStatus(
                winrt::Windows::Media::MediaPlaybackStatus::Closed);
            smtc_.IsEnabled(false);
        }
    } catch (...) {
    }
}

int SmtcBridge::Run(bool require_playback_module) {
    logging::Initialize();
    logging::Write(L"KuGou SMTC plugin worker started");

    const ProcessRole process_role =
        require_playback_module ? CurrentProcessRole()
                                : ProcessRole::publisher;
    if (process_role == ProcessRole::disabled) {
        logging::Write(L"Process role: disabled");
        return 0;
    }
    primary_process_ = process_role == ProcessRole::publisher;
    logging::Write(primary_process_
                       ? L"Process role: SMTC publisher"
                       : L"Process role: metadata provider");

    if (primary_process_ && require_playback_module) {
        WaitForPlaybackModule();
    }

    if (primary_process_) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error& error) {
            logging::WriteError(L"winrt::init_apartment", error.code().value);
            return 2;
        }
    }

    if (!shared_metadata_.Open()) {
        return 3;
    }

    if (!CreateBridgeWindow()) {
        return 4;
    }
    if (primary_process_ && !InitializeSmtc()) {
        DestroyWindow(window_);
        window_ = nullptr;
        return 5;
    }

    Poll();
    SetTimer(window_, kPollTimerId, kPollIntervalMs, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    logging::Write(L"KuGou SMTC plugin worker stopped");
    return 0;
}

bool SmtcBridge::CreateBridgeWindow() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &SmtcBridge::WindowProc;
    window_class.hInstance = module_;
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logging::WriteError(L"RegisterClassExW", GetLastError());
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClassName,
        L"KuGou SMTC Bridge", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        module_, this);
    if (window_ == nullptr) {
        logging::WriteError(L"CreateWindowExW", GetLastError());
        return false;
    }
    return true;
}

bool SmtcBridge::InitializeSmtc() {
    try {
        auto interop = winrt::get_activation_factory<
            winrt::Windows::Media::SystemMediaTransportControls,
            ISystemMediaTransportControlsInterop>();

        const HRESULT result = interop->GetForWindow(
            window_,
            __uuidof(ABI::Windows::Media::ISystemMediaTransportControls),
            winrt::put_abi(smtc_));
        winrt::check_hresult(result);

        smtc_.IsEnabled(true);
        smtc_.IsPlayEnabled(true);
        smtc_.IsPauseEnabled(true);
        smtc_.IsPreviousEnabled(true);
        smtc_.IsNextEnabled(true);
        smtc_.IsStopEnabled(false);
        smtc_.PlaybackStatus(
            winrt::Windows::Media::MediaPlaybackStatus::Stopped);
        button_pressed_token_ = smtc_.ButtonPressed(
            [this](
                const winrt::Windows::Media::SystemMediaTransportControls&,
                const winrt::Windows::Media::
                    SystemMediaTransportControlsButtonPressedEventArgs& args) {
                OnButtonPressed(args.Button());
            });
        button_handler_registered_ = true;
        logging::Write(L"SMTC session initialized");
        return true;
    } catch (const winrt::hresult_error& error) {
        logging::WriteError(L"SMTC initialization", error.code().value);
    } catch (...) {
        logging::Write(L"SMTC initialization failed with an unknown error");
    }
    return false;
}

void SmtcBridge::Poll() noexcept {
    try {
        bool control_found = false;
        const std::wstring control_title =
            metadata::ReadPlaybackControlTitle(&control_found);
        const auto stable_title =
            metadata_filter_.Observe(control_found, control_title);
        if (stable_title.has_value()) {
            shared_metadata_.Publish(*stable_title);
            logging::Write(L"Shared metadata updated");
        }

        if (!primary_process_) {
            return;
        }

        const std::optional<std::wstring> raw_title =
            shared_metadata_.Read();
        if (raw_title.has_value() && *raw_title != last_raw_title_) {
            PublishMetadata(*raw_title);
            last_raw_title_ = *raw_title;
        }

        const ULONGLONG now = GetTickCount64();
        if (!last_raw_title_.empty() && now <= cover_retry_until_ &&
            now >= next_cover_retry_) {
            TryPublishCoverArt();
            next_cover_retry_ = now + kCoverRetryIntervalMs;
        }

        const std::optional<bool> playing =
            playback_command_status_guard_.Resolve(
                PlaybackStateForActivity(audio_monitor_.Query()), now);
        if (playing.has_value() &&
            (!playback_initialized_ || *playing != last_playing_)) {
            PublishPlaybackStatus(*playing);
            last_playing_ = *playing;
            playback_initialized_ = true;
        }
    } catch (const winrt::hresult_error& error) {
        logging::WriteError(L"SMTC polling", error.code().value);
    } catch (...) {
        logging::Write(L"SMTC polling failed with an unknown error");
    }
}

void SmtcBridge::PublishMetadata(const std::wstring& raw_title) {
    auto updater = smtc_.DisplayUpdater();
    updater.ClearAll();

    if (raw_title.empty()) {
        current_artist_.clear();
        current_title_.clear();
        published_cover_path_.clear();
        cover_retry_until_ = 0;
        next_cover_retry_ = 0;
        updater.Update();
        smtc_.PlaybackStatus(
            winrt::Windows::Media::MediaPlaybackStatus::Stopped);
        logging::Write(L"SMTC metadata cleared");
        return;
    }

    const auto [artist, title] = SplitArtistAndTitle(raw_title);
    current_artist_ = artist;
    current_title_ = title;
    updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
    auto music = updater.MusicProperties();
    music.Title(title);
    music.Artist(artist);
    updater.Update();

    published_cover_path_.clear();
    cover_retry_until_ = GetTickCount64() + kCoverRetryDurationMs;
    next_cover_retry_ = 0;
    TryPublishCoverArt();
    next_cover_retry_ = GetTickCount64() + kCoverRetryIntervalMs;

    std::wstring message = L"SMTC metadata updated: ";
    message += raw_title;
    logging::Write(message);
}

void SmtcBridge::PublishPlaybackStatus(bool playing) {
    if (last_raw_title_.empty()) {
        smtc_.PlaybackStatus(
            winrt::Windows::Media::MediaPlaybackStatus::Stopped);
        return;
    }

    smtc_.PlaybackStatus(
        playing ? winrt::Windows::Media::MediaPlaybackStatus::Playing
                : winrt::Windows::Media::MediaPlaybackStatus::Paused);
    logging::Write(playing ? L"SMTC status: Playing"
                           : L"SMTC status: Paused");
}

void SmtcBridge::TryPublishCoverArt() noexcept {
    try {
        const std::optional<CoverArt> cover = cover_art_resolver_.Resolve(
            current_artist_, current_title_);
        if (!cover.has_value() || cover->path == published_cover_path_) {
            return;
        }

        const auto file = winrt::Windows::Storage::StorageFile::
            GetFileFromPathAsync(cover->path).get();
        const auto reference = winrt::Windows::Storage::Streams::
            RandomAccessStreamReference::CreateFromFile(file);
        auto updater = smtc_.DisplayUpdater();
        updater.Thumbnail(reference);
        updater.Update();

        published_cover_path_ = cover->path;
        logging::Write(L"SMTC cover updated");
    } catch (const winrt::hresult_error& error) {
        logging::WriteError(L"SMTC cover update", error.code().value);
    } catch (...) {
        logging::Write(L"SMTC cover update failed with an unknown error");
    }
}

void SmtcBridge::OnButtonPressed(
    winrt::Windows::Media::SystemMediaTransportControlsButton button)
    noexcept {
    std::optional<MediaControlAction> action;
    switch (button) {
        case winrt::Windows::Media::SystemMediaTransportControlsButton::Play:
            action = MediaControlAction::play;
            break;
        case winrt::Windows::Media::SystemMediaTransportControlsButton::Pause:
            action = MediaControlAction::pause;
            break;
        case winrt::Windows::Media::SystemMediaTransportControlsButton::Previous:
            action = MediaControlAction::previous;
            break;
        case winrt::Windows::Media::SystemMediaTransportControlsButton::Next:
            action = MediaControlAction::next;
            break;
        default:
            return;
    }

    if (window_ == nullptr ||
        !PostMessageW(window_, kMediaControlMessage,
                      static_cast<WPARAM>(*action), 0)) {
        logging::WriteError(L"Queueing SMTC media control",
                            static_cast<long>(GetLastError()));
    }
}

void SmtcBridge::HandleMediaControl(MediaControlAction action) noexcept {
    if ((action == MediaControlAction::play && playback_initialized_ &&
         last_playing_) ||
        (action == MediaControlAction::pause && playback_initialized_ &&
         !last_playing_)) {
        return;
    }

    if (!media_control_echo_guard_.TryEnter(GetTickCount64())) {
        return;
    }

    const ProcessRole current_role =
        primary_process_ ? ProcessRole::publisher
                         : ProcessRole::metadata_provider;
    if (!media_controller_.Dispatch(action, current_role, window_)) {
        logging::Write(L"KuGou UI-thread media control was unavailable");
        return;
    }

    logging::Write(L"SMTC control queued to KuGou UI thread");
}

void SmtcBridge::HandleMediaControlCompletion(
    MediaControlAction action, bool executed) noexcept {
    if (!executed) {
        logging::Write(L"KuGou UI-thread media control failed");
        return;
    }

    const ULONGLONG now = GetTickCount64();
    playback_command_status_guard_.RecordSuccessfulCommand(action, now);
    const std::optional<bool> requested = RequestedPlaybackState(action);
    if (requested.has_value()) {
        PublishPlaybackStatus(*requested);
        last_playing_ = *requested;
        playback_initialized_ = true;
    }

    switch (action) {
        case MediaControlAction::play:
            logging::Write(L"SMTC control executed: Play");
            break;
        case MediaControlAction::pause:
            logging::Write(L"SMTC control executed: Pause");
            break;
        case MediaControlAction::previous:
            logging::Write(L"SMTC control executed: Previous");
            break;
        case MediaControlAction::next:
            logging::Write(L"SMTC control executed: Next");
            break;
    }
}

LRESULT CALLBACK SmtcBridge::WindowProc(HWND window, UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam) noexcept {
    SmtcBridge* self = reinterpret_cast<SmtcBridge*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SmtcBridge*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (message == WM_TIMER && wparam == kPollTimerId && self != nullptr) {
        self->Poll();
        return 0;
    }
    if (message == kMediaControlMessage && self != nullptr) {
        self->HandleMediaControl(static_cast<MediaControlAction>(wparam));
        return 0;
    }
    const UINT completion_message = MediaControlCompletionMessage();
    if (completion_message != 0 && message == completion_message &&
        self != nullptr) {
        self->HandleMediaControlCompletion(
            static_cast<MediaControlAction>(wparam), lparam != 0);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, kPollTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI PluginThreadEntry(void* module) noexcept {
    try {
        SmtcBridge bridge(static_cast<HMODULE>(module));
        return static_cast<DWORD>(bridge.Run(true));
    } catch (...) {
        logging::Initialize();
        logging::Write(L"Plugin worker terminated by an unhandled error");
        return 0xFFFFFFFF;
    }
}

}  // namespace plugin
