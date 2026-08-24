#include "metadata_transition_filter.h"

namespace plugin::metadata {
namespace {

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool IsTransientPlaybackTitle(const std::wstring& title) {
    return title.empty() || title == L"酷狗音乐" ||
           StartsWith(title, L"正在连接") ||
           StartsWith(title, L"正在缓冲");
}

}  // namespace

std::optional<std::wstring> PlaybackMetadataFilter::Observe(
    bool control_found, const std::wstring& title) {
    if (!control_found) {
        return std::nullopt;
    }

    if (IsTransientPlaybackTitle(title) || title == last_observed_title_) {
        return std::nullopt;
    }
    last_observed_title_ = title;
    return title;
}

}  // namespace plugin::metadata
