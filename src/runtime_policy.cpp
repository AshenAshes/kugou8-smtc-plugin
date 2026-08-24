#include "runtime_policy.h"

#include <cwchar>

namespace plugin {
namespace {

std::wstring FileName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring ParentPath(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{}
                                           : path.substr(0, separator);
}

}  // namespace

ProcessRole ClassifyProcessPath(const std::wstring& executable_path) {
    if (executable_path.empty() ||
        _wcsicmp(FileName(executable_path).c_str(), L"KuGou.exe") != 0) {
        return ProcessRole::disabled;
    }

    const std::wstring parent_path = ParentPath(executable_path);
    const std::wstring parent_name = FileName(parent_path);
    if (_wcsicmp(parent_name.c_str(), L"KGMusic") == 0) {
        return ProcessRole::publisher;
    }
    if (_wcsicmp(parent_name.c_str(), L"8.3.97.21592") == 0 &&
        _wcsicmp(FileName(ParentPath(parent_path)).c_str(), L"KGMusic") == 0) {
        return ProcessRole::metadata_provider;
    }
    return ProcessRole::disabled;
}

std::optional<bool> PlaybackStateForActivity(AudioActivity activity) {
    if (activity == AudioActivity::unavailable) {
        return std::nullopt;
    }
    return activity == AudioActivity::active;
}

std::uint32_t MetadataScanBackoff::RecordFailure() noexcept {
    ++failure_count_;
    if (failure_count_ <= 3) {
        return 2000;
    }
    if (failure_count_ == 4) {
        return 5000;
    }
    if (failure_count_ == 5) {
        return 10000;
    }
    return 30000;
}

void MetadataScanBackoff::Reset() noexcept {
    failure_count_ = 0;
}

}  // namespace plugin
