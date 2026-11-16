#include "../src/smtc_bridge.h"

// Exercise the real internal acquisition paths with allocation failures.
#include "../src/cover_art.cpp"
#include "../src/metadata_source.cpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

namespace {
bool fail_all = false;
size_t fail_size = 0;
HRESULT smtc_failure = E_OUTOFMEMORY;
}

void* operator new(size_t size) {
    if (fail_all || (fail_size != 0 && size == fail_size)) {
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size == 0 ? 1 : size)) {
        return value;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

namespace plugin {
struct SmtcBridgeTestAccess {
    static void Configure(SmtcBridge& bridge, void* controls) {
        winrt::attach_abi(bridge.smtc_, controls);
        bridge.last_raw_title_ = L"fault-safety-track";
    }
    static bool Complete(SmtcBridge& bridge) {
        bridge.HandleMediaControlCompletion(MediaControlAction::play, true);
        const bool unchanged = !bridge.last_playing_;
        bridge.smtc_ = nullptr;
        return unchanged;
    }
    static void PrepareRetry(SmtcBridge& bridge) {
        bridge.current_title_ = L"allocation-failure-probe";
        bridge.cover_retry_until_ = GetTickCount64() + 10000;
        bridge.next_cover_retry_ = GetTickCount64() + 1000;
    }
    static void TryCover(SmtcBridge& bridge) { bridge.TryPublishCoverArt(); }
    static bool RetryStopped(const SmtcBridge& bridge) {
        return bridge.cover_retry_until_ == 0 && bridge.next_cover_retry_ == 0;
    }
};
}

namespace {
HRESULT __stdcall Query(void* self, REFIID, void** out) { *out = self; return S_OK; }
ULONG __stdcall Ref(void*) { return 2; }
HRESULT __stdcall Ids(void*, ULONG* count, IID** ids) { *count = 0; *ids = nullptr; return S_OK; }
HRESULT __stdcall Name(void*, HSTRING* out) { *out = nullptr; return S_OK; }
HRESULT __stdcall Trust(void*, int* out) { *out = 0; return S_OK; }
HRESULT __stdcall GetStatus(void*, int* out) { *out = 4; return S_OK; }
HRESULT __stdcall PutStatus(void*, int) { return smtc_failure; }
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::set_terminate([] { ExitProcess(86); });
    if (argc < 2) { return 2; }
    if (wcscmp(argv[1], L"file-leak") == 0) {
        if (argc < 3) { return 2; }
        const std::wstring path = argv[2];
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { return 3; }
        unsigned char data[3072]{};
        DWORD written = 0;
        const bool created = WriteFile(file, data, sizeof(data), &written, nullptr) &&
                             written == sizeof(data);
        CloseHandle(file);
        if (!created) { return 3; }
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        int caught = 0;
        fail_size = sizeof(data);
        for (int index = 0; index < 32; ++index) {
            try { (void)plugin::ReadFileBytes(path); }
            catch (const std::bad_alloc&) { ++caught; }
        }
        fail_size = 0;
        GetProcessHandleCount(GetCurrentProcess(), &after);
        std::printf("File allocation failures: %d; handle delta: %ld\n",
                    caught, static_cast<long>(after) - static_cast<long>(before));
        return caught == 32 && after == before ? 0 : 10;
    }
    if (wcscmp(argv[1], L"module-leak") == 0) {
        if (argc < 3) { return 2; }
        HMODULE fixture = LoadLibraryW(argv[2]);
        if (fixture == nullptr) { return 3; }
        DWORD before = 0, after = 0;
        GetProcessHandleCount(GetCurrentProcess(), &before);
        int caught = 0;
        fail_size = 8;
        for (int index = 0; index < 32; ++index) {
            try { (void)plugin::metadata::FindControlVtableRanges(); }
            catch (const std::bad_alloc&) { ++caught; }
        }
        fail_size = 0;
        GetProcessHandleCount(GetCurrentProcess(), &after);
        FreeLibrary(fixture);
        std::printf("Module allocation failures: %d; handle delta: %ld\n",
                    caught, static_cast<long>(after) - static_cast<long>(before));
        return caught == 32 && after == before ? 0 : 10;
    }
    if (wcscmp(argv[1], L"constructor") == 0) {
        try {
            fail_all = true;
            plugin::SmtcBridge bridge(GetModuleHandleW(nullptr));
            fail_all = false;
        } catch (const std::bad_alloc&) {
            fail_all = false;
            std::puts("PASS: initialization failure propagated without termination");
            return 0;
        }
        return 1;
    }
    if (wcscmp(argv[1], L"cover-retry") == 0) {
        plugin::SmtcBridge bridge(GetModuleHandleW(nullptr));
        plugin::SmtcBridgeTestAccess::PrepareRetry(bridge);
        fail_all = true;
        plugin::SmtcBridgeTestAccess::TryCover(bridge);
        fail_all = false;
        return plugin::SmtcBridgeTestAccess::RetryStopped(bridge) ? 0 : 1;
    }
    if (wcscmp(argv[1], L"completion") == 0 ||
        wcscmp(argv[1], L"completion-memory") == 0) {
        if (wcscmp(argv[1], L"completion-memory") == 0) {
            smtc_failure = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        void* table[] = {reinterpret_cast<void*>(&Query), reinterpret_cast<void*>(&Ref),
            reinterpret_cast<void*>(&Ref), reinterpret_cast<void*>(&Ids),
            reinterpret_cast<void*>(&Name), reinterpret_cast<void*>(&Trust),
            reinterpret_cast<void*>(&GetStatus), reinterpret_cast<void*>(&PutStatus)};
        struct Fake { void** table; } fake{table};
        plugin::SmtcBridge bridge(GetModuleHandleW(nullptr));
        plugin::SmtcBridgeTestAccess::Configure(bridge, &fake);
        const bool safe = plugin::SmtcBridgeTestAccess::Complete(bridge);
        std::printf("PASS: completion failure 0x%08lX was contained\n",
                    static_cast<unsigned long>(smtc_failure));
        CoUninitialize();
        return safe ? 0 : 1;
    }
    return 2;
}
