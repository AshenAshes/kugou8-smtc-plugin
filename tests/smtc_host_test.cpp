#include "../src/smtc_bridge.h"
#include "../src/audio_session.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int wmain() {
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        std::cerr << "COM initialization failed for audio benchmark.\n";
        return 10;
    }

    constexpr int kAudioQueries = 200;
    const auto benchmark_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kAudioQueries; ++index) {
        (void)plugin::GetCurrentProcessAudioActivity();
    }
    const auto benchmark_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - benchmark_start);
    std::cout << "Audio activity queries: " << kAudioQueries << " in "
              << benchmark_elapsed.count() << " ms.\n";

    std::atomic<DWORD> bridge_thread_id{0};
    int bridge_result = -1;

    std::thread bridge_thread([&] {
        bridge_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
        plugin::SmtcBridge bridge(GetModuleHandleW(nullptr));
        bridge_result = bridge.Run(false);
    });

    while (bridge_thread_id.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "SMTC test host is active for 12 seconds.\n";
    std::this_thread::sleep_for(std::chrono::seconds(12));
    PostThreadMessageW(bridge_thread_id.load(), WM_QUIT, 0, 0);
    bridge_thread.join();

    std::cout << "SMTC test host result: " << bridge_result << '\n';
    CoUninitialize();
    return bridge_result;
}
