#include "../src/shared_metadata.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<
              decltype(std::declval<const plugin::SharedMetadata&>().Read()),
              std::optional<std::wstring>>);

int wmain() {
    const std::wstring suffix =
        L".Test." + std::to_wstring(GetCurrentProcessId());
    plugin::SharedMetadata channel(suffix.c_str());
    if (!channel.Open()) {
        std::wcerr << L"FAIL: isolated shared metadata did not open\n";
        return 1;
    }

    const auto initial = channel.Read();
    if (!initial.has_value() || !initial->empty()) {
        std::wcerr << L"FAIL: initial snapshot was not a valid empty value\n";
        return 2;
    }

    constexpr size_t kWriterCount = 6;
    std::array<std::wstring, kWriterCount> values;
    for (size_t index = 0; index < values.size(); ++index) {
        values[index] = L"writer-" + std::to_wstring(index) + L":" +
                        std::wstring(480, static_cast<wchar_t>(L'A' + index));
    }

    std::atomic<bool> start{false};
    std::atomic<unsigned> running{kWriterCount};
    std::vector<std::thread> writers;
    for (size_t index = 0; index < values.size(); ++index) {
        writers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                SwitchToThread();
            }
            for (int iteration = 0; iteration < 3000; ++iteration) {
                channel.Publish(values[index]);
            }
            running.fetch_sub(1, std::memory_order_release);
        });
    }

    start.store(true, std::memory_order_release);
    bool torn_value = false;
    while (running.load(std::memory_order_acquire) != 0) {
        const auto snapshot = channel.Read();
        if (!snapshot.has_value() || snapshot->empty()) {
            continue;
        }
        bool valid = false;
        for (const auto& expected : values) {
            if (*snapshot == expected) {
                valid = true;
                break;
            }
        }
        if (!valid) {
            torn_value = true;
            break;
        }
    }
    for (auto& writer : writers) {
        writer.join();
    }
    if (torn_value) {
        std::wcerr << L"FAIL: reader observed a torn multi-writer value\n";
        return 3;
    }

    const auto final_snapshot = channel.Read();
    if (!final_snapshot.has_value() || final_snapshot->empty()) {
        std::wcerr << L"FAIL: final shared snapshot was unavailable\n";
        return 4;
    }

    std::wcout << L"PASS: shared metadata serializes writers and preserves read failures\n";
    return 0;
}
