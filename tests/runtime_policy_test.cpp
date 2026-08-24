#include "../src/runtime_policy.h"

#include <array>
#include <iostream>

int wmain() {
    using plugin::ProcessRole;

    if (plugin::ClassifyProcessPath(
            L"C:\\Program Files (x86)\\KuGou\\KGMusic\\KuGou.exe") !=
        ProcessRole::publisher) {
        std::wcerr << L"FAIL: verified playback process was not publisher\n";
        return 1;
    }
    if (plugin::ClassifyProcessPath(
            L"C:\\Program Files (x86)\\KuGou\\KGMusic\\8.3.97.21592\\KuGou.exe") !=
        ProcessRole::metadata_provider) {
        std::wcerr << L"FAIL: verified UI process was not metadata provider\n";
        return 2;
    }
    if (plugin::ClassifyProcessPath(
            L"C:\\Program Files (x86)\\KuGou\\KGMusic\\helper.exe") !=
            ProcessRole::disabled ||
        plugin::ClassifyProcessPath(L"") != ProcessRole::disabled ||
        plugin::ClassifyProcessPath(
            L"C:\\Temp\\KuGou.exe") != ProcessRole::disabled) {
        std::wcerr << L"FAIL: unknown process did not fail closed\n";
        return 3;
    }

    if (plugin::PlaybackStateForActivity(
            plugin::AudioActivity::unavailable).has_value()) {
        std::wcerr << L"FAIL: unavailable audio was published as paused\n";
        return 4;
    }
    if (plugin::PlaybackStateForActivity(plugin::AudioActivity::active) !=
            true ||
        plugin::PlaybackStateForActivity(plugin::AudioActivity::inactive) !=
            false) {
        std::wcerr << L"FAIL: known audio states were mapped incorrectly\n";
        return 5;
    }

    plugin::MetadataScanBackoff backoff;
    const std::array<std::uint32_t, 7> expected = {
        2000, 2000, 2000, 5000, 10000, 30000, 30000};
    for (const auto delay : expected) {
        if (backoff.RecordFailure() != delay) {
            std::wcerr << L"FAIL: metadata scan did not back off\n";
            return 6;
        }
    }
    backoff.Reset();
    if (backoff.RecordFailure() != 2000) {
        std::wcerr << L"FAIL: metadata scan backoff did not reset\n";
        return 7;
    }

    std::wcout << L"PASS: runtime failure policies are safe\n";
    return 0;
}
