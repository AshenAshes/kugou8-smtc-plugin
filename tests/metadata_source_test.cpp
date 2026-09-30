#include "../src/metadata_source.h"
#include "../src/metadata_transition_filter.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct LegacyWString {
    union {
        wchar_t inline_buffer[8];
        const wchar_t* pointer;
    } storage;
    std::uint32_t size;
    std::uint32_t capacity;
};

}  // namespace

int wmain() {
    using plugin::metadata::ParsePlaybackTime;
    for (const auto& item : std::vector<std::pair<std::wstring, std::int64_t>>{
             {L"00:00", 0}, {L"02:21", 141}, {L"02:36", 156},
             {L"90:01", 5401}, {L"1:02:03", 3723}}) {
        if (ParsePlaybackTime(item.first) != item.second) {
            std::wcerr << L"FAIL: valid playback time rejected\n";
            return 5;
        }
    }
    for (const auto* invalid : {L"", L"正在缓冲", L"--:--", L"01:60",
             L"-1:23", L"1", L"1:", L":23", L"1:2", L"1:2:03",
             L"1:00:00:00", L"999999999:00", L"01:02junk"}) {
        if (ParsePlaybackTime(invalid)) {
            std::wcerr << L"FAIL: invalid playback time accepted\n";
            return 6;
        }
    }

    alignas(4) std::array<std::uint8_t, 0x400> time_control{};
    const std::wstring time_name = L"PlaybackControlPanelTimeStatus";
    LegacyWString time_name_value{};
    time_name_value.storage.pointer = time_name.c_str();
    time_name_value.size = static_cast<std::uint32_t>(time_name.size());
    time_name_value.capacity = 63;
    std::memcpy(time_control.data() + 0x3C, &time_name_value, sizeof(time_name_value));
    const auto set_time = [&](size_t offset, const wchar_t* text) {
        LegacyWString value{};
        wcscpy_s(value.storage.inline_buffer, text);
        value.size = static_cast<std::uint32_t>(wcslen(text));
        value.capacity = 7;
        std::memcpy(time_control.data() + offset, &value, sizeof(value));
    };
    set_time(0x3BC, L"02:21");
    set_time(0x3D8, L"02:36");
    const auto time = plugin::metadata::ReadTimelineFromControl(time_control.data());
    if (!time || time->position_seconds != 141 || time->duration_seconds != 156) {
        std::wcerr << L"FAIL: time control fields decoded incorrectly\n";
        return 7;
    }
    // A manual backward seek must immediately replace the previous position.
    set_time(0x3BC, L"00:05");
    const auto seek = plugin::metadata::ReadTimelineFromControl(time_control.data());
    if (!seek || seek->position_seconds != 5) {
        return 8;
    }
    set_time(0x3D8, L"00:00");
    if (plugin::metadata::ReadTimelineFromControl(time_control.data())) {
        return 9;
    }
    set_time(0x3D8, L"00:04");
    if (plugin::metadata::ReadTimelineFromControl(time_control.data())) {
        return 10;
    }
    set_time(0x3D8, L"02:36");
    time_control[0x3C + 16] = 0;  // Destroyed/reused control.
    if (plugin::metadata::ReadTimelineFromControl(time_control.data()) ||
        plugin::metadata::ReadTimelineFromControl(nullptr)) {
        return 11;
    }

    const std::wstring expected = L"SNoW - 逆さまの蝶 (倒逆之蝶)";

    LegacyWString inline_value{};
    wcscpy_s(inline_value.storage.inline_buffer, L"SNoW");
    inline_value.size = 4;
    inline_value.capacity = 7;
    if (plugin::metadata::DecodeLegacyMsvcWstring(&inline_value) != L"SNoW") {
        std::wcerr << L"FAIL: legacy inline wstring was not decoded\n";
        return 1;
    }

    LegacyWString heap_value{};
    heap_value.storage.pointer = expected.c_str();
    heap_value.size = static_cast<std::uint32_t>(expected.size());
    heap_value.capacity = 63;
    if (plugin::metadata::DecodeLegacyMsvcWstring(&heap_value) != expected) {
        std::wcerr << L"FAIL: legacy heap wstring was not decoded\n";
        return 2;
    }

    alignas(4) std::array<std::uint8_t, 256> control{};
    const std::uintptr_t fake_vtable = 0x12345678;
    std::memcpy(control.data(), &fake_vtable, sizeof(fake_vtable));

    const std::wstring control_name = L"PlaybackControlPanelSongStatus";
    LegacyWString name_value{};
    name_value.storage.pointer = control_name.c_str();
    name_value.size = static_cast<std::uint32_t>(control_name.size());
    name_value.capacity = 63;
    std::memcpy(control.data() + 0x3C, &name_value, sizeof(name_value));
    std::memcpy(control.data() + 0x54, &heap_value, sizeof(heap_value));

    const std::wstring control_title =
        plugin::metadata::ReadNamedControlTextInRange(
            control.data(), control.size(), 0x12340000, 0x12350000,
            control_name);
    if (control_title != expected) {
        std::wcerr << L"FAIL: named playback control text was not selected\n";
        return 3;
    }

    plugin::metadata::PlaybackMetadataFilter transition_filter;
    std::vector<std::wstring> published;
    const std::array<std::wstring, 5> switch_sequence = {
        L"SNoW - 逆さまの蝶 (倒逆之蝶)", L"", L"正在缓冲36%...",
        L"正在缓冲...", L"米津玄師 - 春雷"};
    for (const auto& observed : switch_sequence) {
        const std::optional<std::wstring> update =
            transition_filter.Observe(true, observed);
        if (update.has_value()) {
            published.push_back(*update);
        }
    }
    if (published.size() != 2 || published[0] != switch_sequence[0] ||
        published[1] != switch_sequence[4]) {
        std::wcerr
            << L"FAIL: transient switch states reached the SMTC publisher\n";
        return 4;
    }

    std::wcout << L"PASS: playback metadata was decoded and stabilized\n";
    return 0;
}
