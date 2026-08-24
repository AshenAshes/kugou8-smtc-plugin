#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace plugin {

struct CoverArt final {
    std::wstring path;
    std::uint64_t last_access_ticks = 0;
};

class CoverArtResolver final {
public:
    CoverArtResolver();
    CoverArtResolver(std::wstring album_cache_root,
                     std::wstring singer_cache_root,
                     std::wstring prepared_cache_root);

    std::optional<CoverArt> Resolve(std::wstring_view artist,
                                    std::wstring_view title) const noexcept;

private:
    std::wstring album_cache_root_;
    std::wstring singer_cache_root_;
    std::wstring prepared_cache_root_;
};

}  // namespace plugin
