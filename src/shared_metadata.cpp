#include "shared_metadata.h"

#include "logging.h"

#include <algorithm>
#include <array>
#include <cwchar>

namespace plugin {
namespace {

constexpr wchar_t kMappingName[] =
    L"Local\\KuGouSmtcPlugin.Metadata.v1";
constexpr wchar_t kWriterMutexName[] =
    L"Local\\KuGouSmtcPlugin.MetadataWriter.v1";
constexpr size_t kTitleCapacity = 512;
constexpr DWORD kWriterLockTimeoutMs = 1000;

}  // namespace

struct SharedMetadata::Block {
    volatile LONG sequence;
    DWORD writer_process_id;
    wchar_t title[kTitleCapacity];
};

SharedMetadata::SharedMetadata(const wchar_t* object_name_suffix) noexcept {
    const wchar_t* suffix =
        object_name_suffix == nullptr ? L"" : object_name_suffix;
    swprintf_s(mapping_name_.data(), mapping_name_.size(), L"%s%s",
               kMappingName, suffix);
    swprintf_s(writer_mutex_name_.data(), writer_mutex_name_.size(), L"%s%s",
               kWriterMutexName, suffix);
}

SharedMetadata::~SharedMetadata() {
    if (block_ != nullptr) {
        UnmapViewOfFile(block_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
    }
    if (writer_mutex_ != nullptr) {
        CloseHandle(writer_mutex_);
    }
}

bool SharedMetadata::Open() noexcept {
    if (block_ != nullptr) {
        return true;
    }

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                  PAGE_READWRITE, 0, sizeof(Block),
                                  mapping_name_.data());
    if (mapping_ == nullptr) {
        logging::WriteError(L"CreateFileMappingW", GetLastError());
        return false;
    }

    block_ = static_cast<Block*>(MapViewOfFile(
        mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Block)));
    if (block_ == nullptr) {
        logging::WriteError(L"MapViewOfFile", GetLastError());
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    writer_mutex_ = CreateMutexW(nullptr, FALSE, writer_mutex_name_.data());
    if (writer_mutex_ == nullptr) {
        logging::WriteError(L"CreateMutexW", GetLastError());
        UnmapViewOfFile(block_);
        block_ = nullptr;
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    return true;
}

void SharedMetadata::Publish(const std::wstring& title) noexcept {
    if (block_ == nullptr || writer_mutex_ == nullptr) {
        return;
    }

    const DWORD wait_result =
        WaitForSingleObject(writer_mutex_, kWriterLockTimeoutMs);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        return;
    }

    const LONG sequence =
        InterlockedCompareExchange(&block_->sequence, 0, 0);
    if ((sequence & 1) != 0) {
        InterlockedIncrement(&block_->sequence);
    }

    InterlockedIncrement(&block_->sequence);
    block_->writer_process_id = GetCurrentProcessId();
    const size_t count = (std::min)(title.size(), kTitleCapacity - 1);
    if (count != 0) {
        std::wmemcpy(block_->title, title.data(), count);
    }
    block_->title[count] = L'\0';
    MemoryBarrier();
    InterlockedIncrement(&block_->sequence);
    ReleaseMutex(writer_mutex_);
}

std::optional<std::wstring> SharedMetadata::Read() const {
    if (block_ == nullptr) {
        return std::nullopt;
    }

    std::array<wchar_t, kTitleCapacity> title{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before =
            InterlockedCompareExchange(&block_->sequence, 0, 0);
        if ((before & 1) != 0) {
            SwitchToThread();
            continue;
        }

        std::wmemcpy(title.data(), block_->title, title.size());
        MemoryBarrier();
        const LONG after =
            InterlockedCompareExchange(&block_->sequence, 0, 0);
        if (before == after && (after & 1) == 0) {
            title.back() = L'\0';
            return std::wstring(title.data());
        }
    }
    return std::nullopt;
}

}  // namespace plugin
