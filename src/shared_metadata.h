#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <string>

namespace plugin {

class SharedMetadata final {
public:
    explicit SharedMetadata(const wchar_t* object_name_suffix = nullptr) noexcept;
    ~SharedMetadata();

    SharedMetadata(const SharedMetadata&) = delete;
    SharedMetadata& operator=(const SharedMetadata&) = delete;

    bool Open() noexcept;
    void Publish(const std::wstring& title) noexcept;
    std::optional<std::wstring> Read() const;

private:
    struct Block;

    HANDLE mapping_ = nullptr;
    HANDLE writer_mutex_ = nullptr;
    Block* block_ = nullptr;
    std::array<wchar_t, 128> mapping_name_{};
    std::array<wchar_t, 128> writer_mutex_name_{};
};

}  // namespace plugin
