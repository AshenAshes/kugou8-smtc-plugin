#pragma once

#include <optional>
#include <string>

namespace plugin::metadata {

class PlaybackMetadataFilter final {
public:
    std::optional<std::wstring> Observe(bool control_found,
                                        const std::wstring& title);

private:
    std::wstring last_observed_title_;
};

}  // namespace plugin::metadata
