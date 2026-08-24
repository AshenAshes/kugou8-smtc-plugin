#pragma once

#include "audio_session.h"

#include <cstdint>
#include <optional>
#include <string>

namespace plugin {

enum class ProcessRole {
    publisher,
    metadata_provider,
    disabled,
};

ProcessRole ClassifyProcessPath(const std::wstring& executable_path);
std::optional<bool> PlaybackStateForActivity(AudioActivity activity);

class MetadataScanBackoff final {
public:
    std::uint32_t RecordFailure() noexcept;
    void Reset() noexcept;

private:
    unsigned failure_count_ = 0;
};

}  // namespace plugin
