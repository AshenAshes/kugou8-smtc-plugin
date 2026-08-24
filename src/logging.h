#pragma once

#include <string_view>

namespace plugin::logging {

void Initialize() noexcept;
void Write(std::wstring_view message) noexcept;
void WriteError(std::wstring_view context, long error) noexcept;

}  // namespace plugin::logging
