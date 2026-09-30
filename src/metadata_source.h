#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace plugin::metadata {

std::wstring DecodeLegacyMsvcWstring(const void* string_object);
std::wstring ReadNamedControlTextInRange(const void* range_start,
                                         size_t range_size,
                                         std::uintptr_t vtable_start,
                                         std::uintptr_t vtable_end,
                                         const std::wstring& control_name);
std::uintptr_t FindNamedControlAddress(const std::wstring& control_name);
std::wstring ReadPlaybackControlTitle(bool* found = nullptr);

struct PlaybackTimeline {
    std::int64_t position_seconds;
    std::int64_t duration_seconds;
};

// The fixed-version time control stores elapsed and total time separately.
std::optional<std::int64_t> ParsePlaybackTime(std::wstring_view text);
std::optional<PlaybackTimeline> ReadTimelineFromControl(const void* control);
std::optional<PlaybackTimeline> ReadPlaybackControlTimeline(
    const std::wstring& expected_title);

}  // namespace plugin::metadata
