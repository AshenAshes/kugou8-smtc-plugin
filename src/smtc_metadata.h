#pragma once

#include <string>

namespace plugin::metadata {

// Presentation-only fallback: never use this value for cover lookup.
inline std::wstring ArtistForSmtc(const std::wstring& artist) {
    constexpr wchar_t whitespace[] =
        L" \t\r\n\v\f\u0085\u00A0\u1680\u2000\u2001\u2002\u2003"
        L"\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029"
        L"\u202F\u205F\u3000";
    return artist.find_first_not_of(whitespace) == std::wstring::npos
        ? L"未知歌手" : artist;
}

}  // namespace plugin::metadata
