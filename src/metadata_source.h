#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

namespace plugin::metadata {

std::wstring DecodeLegacyMsvcWstring(const void* string_object);
std::wstring ReadNamedControlTextInRange(const void* range_start,
                                         size_t range_size,
                                         std::uintptr_t vtable_start,
                                         std::uintptr_t vtable_end,
                                         const std::wstring& control_name);
std::uintptr_t FindNamedControlAddress(const std::wstring& control_name);
std::wstring ReadPlaybackControlTitle(bool* found = nullptr);

}  // namespace plugin::metadata
