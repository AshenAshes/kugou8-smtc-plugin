#include "../src/cover_art.h"
#include "../src/media_control.h"

#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        wchar_t temp_path[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp_path);
        if (length == 0 || length >= MAX_PATH) {
            return;
        }
        path_ = std::filesystem::path(temp_path) /
                (L"KuGouSmtcPluginTest_" +
                 std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

enum class FixtureFormat {
    webp,
    jpeg,
};

bool CreateFixture(const std::filesystem::path& path,
                   std::uint64_t last_access_ticks,
                   FixtureFormat format, bool valid = true) {
    std::filesystem::create_directories(path.parent_path());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const unsigned char webp_payload[] = {
        0x52, 0x49, 0x46, 0x46, 0x22, 0x00, 0x00, 0x00,
        0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x20,
        0x16, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00, 0x9D,
        0x01, 0x2A, 0x01, 0x00, 0x01, 0x00, 0x0E, 0xC0,
        0xFE, 0x25, 0xA4, 0x00, 0x03, 0x70, 0x00, 0x00,
        0x00, 0x00};
    const unsigned char invalid_webp_payload[] = {
        'R', 'I', 'F', 'F', 5, 0, 0, 0, 'W', 'E', 'B', 'P',
        'V', 'P', '8', ' ', 0, 0, 0, 0};
    const unsigned char jpeg_payload[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0, 5, 'J', 'F', 0, 0,
        0xFF, 0xD9};
    const unsigned char* payload = jpeg_payload;
    DWORD payload_size = static_cast<DWORD>(sizeof(jpeg_payload));
    if (format == FixtureFormat::webp) {
        payload = valid ? webp_payload : invalid_webp_payload;
        payload_size = static_cast<DWORD>(valid ? sizeof(webp_payload)
                                                : sizeof(invalid_webp_payload));
    }
    DWORD bytes_written = 0;
    const BOOL wrote = WriteFile(file, payload, payload_size,
                                 &bytes_written, nullptr);
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = last_access_ticks;
    FILETIME access_time{};
    access_time.dwLowDateTime = ticks.LowPart;
    access_time.dwHighDateTime = ticks.HighPart;
    const BOOL time_set = SetFileTime(file, nullptr, &access_time, nullptr);
    CloseHandle(file);
    return wrote && bytes_written == payload_size && time_set;
}

bool HasPngSignature(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        return false;
    }
    const unsigned char expected[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned char actual[sizeof(expected)]{};
    stream.read(reinterpret_cast<char*>(actual), sizeof(actual));
    return stream.gcount() == sizeof(actual) &&
           std::equal(std::begin(expected), std::end(expected),
                      std::begin(actual));
}

}  // namespace

int wmain() {
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        std::wcerr << L"FAIL: COM could not be initialized\n";
        return 1;
    }

    using plugin::MediaControlAction;
    if (plugin::MediaControlExecutionRole() !=
        plugin::ProcessRole::publisher) {
        std::wcerr << L"FAIL: media controls were routed to the process "
                      L"without the playback panel\n";
        return 2;
    }
    if (plugin::MediaControlRequiredContext() !=
        plugin::MediaControlExecutionContext::kugou_ui_thread) {
        std::wcerr << L"FAIL: KuGou callbacks were invoked from the plugin "
                      L"worker instead of the UI thread\n";
        return 3;
    }
    if (plugin::MediaControlHookType() != WH_GETMESSAGE) {
        std::wcerr << L"FAIL: posted UI commands were not observed through "
                      L"the target thread's message queue\n";
        return 4;
    }
    if (plugin::InternalCommandVtableOffset(MediaControlAction::play) !=
            0x44 ||
        plugin::InternalCommandVtableOffset(MediaControlAction::pause) !=
            0x44 ||
        plugin::InternalCommandVtableOffset(MediaControlAction::previous) !=
            0x48 ||
        plugin::InternalCommandVtableOffset(MediaControlAction::next) !=
            0x40 ||
        plugin::InternalCommandCallbackRva(
            MediaControlAction::previous) != 0x63247D ||
        plugin::InternalCommandCallbackRva(
            MediaControlAction::next) != 0x632491 ||
        plugin::InternalCommandCallbackRva(
            MediaControlAction::play) != 0x6324A5) {
        std::wcerr << L"FAIL: internal KuGou commands were mapped incorrectly\n";
        return 5;
    }

    if (plugin::RequestedPlaybackState(MediaControlAction::pause) != false ||
        plugin::RequestedPlaybackState(MediaControlAction::play) != true ||
        plugin::RequestedPlaybackState(MediaControlAction::previous) != true ||
        plugin::RequestedPlaybackState(MediaControlAction::next) != true) {
        std::wcerr << L"FAIL: completed commands did not map to SMTC status\n";
        return 6;
    }

    plugin::MediaControlEchoGuard echo_guard;
    if (!echo_guard.TryEnter(1000) || echo_guard.TryEnter(1001) ||
        echo_guard.TryEnter(1249) || !echo_guard.TryEnter(1250)) {
        std::wcerr << L"FAIL: echoed SMTC commands were not suppressed\n";
        return 6;
    }

    plugin::PlaybackCommandStatusGuard status_guard;
    status_guard.RecordSuccessfulCommand(MediaControlAction::pause, 1000);
    if (status_guard.Resolve(true, 1001) != false ||
        status_guard.Resolve(true, 30999) != false ||
        status_guard.Resolve(true, 31000) != true) {
        std::wcerr << L"FAIL: stale audio activity overrode a Pause command\n";
        return 7;
    }
    status_guard.RecordSuccessfulCommand(MediaControlAction::play, 40000);
    if (status_guard.Resolve(true, 40001) != true ||
        status_guard.Resolve(false, 40002) != false) {
        std::wcerr << L"FAIL: confirmed playback status was not released\n";
        return 8;
    }

    TemporaryDirectory temporary;
    if (temporary.path().empty()) {
        std::wcerr << L"FAIL: test cache directory was not created\n";
        return 9;
    }

    constexpr std::uint64_t kBaseTicks = 132000000000000000ULL;
    const auto album_root = temporary.path() / L"AlbumImg";
    const auto singer_root = temporary.path() / L"SingerRes";
    const auto prepared_root = temporary.path() / L"Prepared";
    const auto target = album_root / L"Target Song_2" / L"120" /
                        L"target.jpg";
    const auto unrelated = album_root / L"Unrelated Album_3" / L"480" /
                           L"unrelated.jpg";
    const auto singer = singer_root / L"Fallback Artist_5" / L"120" /
                        L"singer.jpg";
    if (!CreateFixture(target, kBaseTicks + 100, FixtureFormat::webp) ||
        !CreateFixture(unrelated, kBaseTicks + 300,
                       FixtureFormat::webp, false) ||
        !CreateFixture(singer, kBaseTicks + 200, FixtureFormat::jpeg)) {
        std::wcerr << L"FAIL: cover fixtures were not created\n";
        return 10;
    }

    plugin::CoverArtResolver resolver(album_root.wstring(),
                                      singer_root.wstring(),
                                      prepared_root.wstring());
    const auto resolved = resolver.Resolve(L"Example Artist",
                                           L"Target Song (Translated)");
    if (!resolved.has_value() || !HasPngSignature(resolved->path)) {
        std::wcerr << L"FAIL: title-matched album cover was not selected\n";
        return 11;
    }
    if (_wcsicmp(std::filesystem::path(resolved->path)
                     .extension()
                     .c_str(),
                 L".png") != 0 ||
        std::filesystem::path(resolved->path).parent_path() !=
            prepared_root) {
        std::wcerr << L"FAIL: WebP cover was not converted to a prepared PNG\n";
        return 12;
    }

    const auto singer_fallback =
        resolver.Resolve(L"Fallback Artist", L"Missing Song");
    if (!singer_fallback.has_value() ||
        singer_fallback->path != singer.wstring()) {
        std::wcerr << L"FAIL: matching singer image was not used as fallback\n";
        return 13;
    }
    if (resolver.Resolve(L"Unknown Artist", L"Missing Song").has_value()) {
        std::wcerr << L"FAIL: unrelated stale cover was published\n";
        return 14;
    }

    std::wcout << L"PASS: media controls and cover selection are valid\n";
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return 0;
}
