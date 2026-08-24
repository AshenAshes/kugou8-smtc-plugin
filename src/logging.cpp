#include "logging.h"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <cwchar>
#include <mutex>
#include <string>

namespace plugin::logging {
namespace {

std::wstring g_log_path;
std::mutex g_log_mutex;
HANDLE g_named_log_mutex = nullptr;

constexpr LONGLONG kMaximumLogSize = 2LL * 1024 * 1024;
constexpr wchar_t kLogMutexName[] =
    L"Local\\KuGouSmtcPlugin.LogWriter.v1";

class NamedMutexLock final {
public:
    explicit NamedMutexLock(HANDLE mutex) noexcept : mutex_(mutex) {
        if (mutex_ == nullptr) {
            return;
        }
        const DWORD wait_result = WaitForSingleObject(mutex_, 1000);
        acquired_ = wait_result == WAIT_OBJECT_0 ||
                    wait_result == WAIT_ABANDONED;
    }

    ~NamedMutexLock() {
        if (acquired_) {
            ReleaseMutex(mutex_);
        }
    }

    bool acquired() const noexcept { return acquired_; }

private:
    HANDLE mutex_ = nullptr;
    bool acquired_ = false;
};

std::wstring BuildLogPath() {
    std::array<wchar_t, MAX_PATH> app_data{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                               app_data.data()))) {
        return {};
    }

    std::wstring path(app_data.data());
    path += L"\\KuGou8\\SmtcPlugin.log";
    return path;
}

HANDLE OpenLogFile() noexcept {
    return CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE RotateLogIfNeeded(HANDLE file) {
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart < kMaximumLogSize) {
        return file;
    }

    CloseHandle(file);
    const std::wstring archive_path = g_log_path + L".old";
    MoveFileExW(g_log_path.c_str(), archive_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return OpenLogFile();
}

}  // namespace

void Initialize() noexcept {
    try {
        const std::wstring log_path = BuildLogPath();
        std::lock_guard lock(g_log_mutex);
        g_log_path = log_path;
        if (g_named_log_mutex == nullptr) {
            g_named_log_mutex = CreateMutexW(nullptr, FALSE, kLogMutexName);
        }
    } catch (...) {
        try {
            std::lock_guard lock(g_log_mutex);
            g_log_path.clear();
        } catch (...) {
        }
    }
}

void Write(std::wstring_view message) noexcept {
    try {
        SYSTEMTIME time{};
        GetLocalTime(&time);

        wchar_t prefix[64]{};
        const int prefix_length = swprintf_s(
            prefix, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", time.wYear,
            time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds);
        if (prefix_length <= 0) {
            return;
        }

        std::wstring line(prefix, static_cast<size_t>(prefix_length));
        line.append(message);
        line.append(L"\r\n");

        std::lock_guard lock(g_log_mutex);
        if (g_log_path.empty()) {
            return;
        }
        NamedMutexLock named_lock(g_named_log_mutex);
        if (g_named_log_mutex != nullptr && !named_lock.acquired()) {
            return;
        }

        HANDLE file = OpenLogFile();
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        file = RotateLogIfNeeded(file);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }

        DWORD bytes_written = 0;
        const auto byte_count =
            static_cast<DWORD>(line.size() * sizeof(wchar_t));
        WriteFile(file, line.data(), byte_count, &bytes_written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

void WriteError(std::wstring_view context, long error) noexcept {
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"%.*s failed: 0x%08lX",
               static_cast<int>(context.size()), context.data(),
               static_cast<unsigned long>(error));
    Write(buffer);
}

}  // namespace plugin::logging
