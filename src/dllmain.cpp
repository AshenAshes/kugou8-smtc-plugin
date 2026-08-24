#include "forward_stubs.h"
#include "smtc_bridge.h"

#include <windows.h>

namespace {

bool PinProxyModule() noexcept {
    HMODULE pinned_module = nullptr;
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&PinProxyModule),
               &pinned_module) != FALSE;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DisableThreadLibraryCalls(instance);
    if (!plugin::InitializeVersionForwarding()) {
        return FALSE;
    }
    if (!PinProxyModule()) {
        return FALSE;
    }

    HANDLE thread = CreateThread(nullptr, 0, &plugin::PluginThreadEntry,
                                 instance, 0, nullptr);
    if (thread == nullptr) {
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}
