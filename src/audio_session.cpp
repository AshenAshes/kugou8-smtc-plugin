#include "audio_session.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <utility>

namespace plugin {

using Microsoft::WRL::ComPtr;

namespace {

constexpr ULONGLONG kMissingSessionRetryMs = 5000;
constexpr ULONGLONG kInactiveSessionRefreshMs = 10000;

AudioActivity SessionActivity(IAudioSessionControl* session) noexcept {
    if (session == nullptr) {
        return AudioActivity::unavailable;
    }
    AudioSessionState state = AudioSessionStateExpired;
    if (FAILED(session->GetState(&state)) ||
        state == AudioSessionStateExpired) {
        return AudioActivity::unavailable;
    }
    return state == AudioSessionStateActive ? AudioActivity::active
                                             : AudioActivity::inactive;
}

}  // namespace

AudioActivity AudioActivityMonitor::Query() noexcept {
    const ULONGLONG now = GetTickCount64();
    if (session_) {
        const AudioActivity activity = SessionActivity(session_.Get());
        if (activity == AudioActivity::active) {
            return activity;
        }
        if (activity == AudioActivity::inactive &&
            now < next_discovery_tick_) {
            return activity;
        }

        ComPtr<IAudioSessionControl> previous = session_;
        session_.Reset();
        const AudioActivity discovered = Discover(now);
        if (discovered == AudioActivity::unavailable &&
            activity == AudioActivity::inactive) {
            session_ = std::move(previous);
            next_discovery_tick_ = now + kInactiveSessionRefreshMs;
            return AudioActivity::inactive;
        }
        return discovered;
    }

    if (now < next_discovery_tick_) {
        return AudioActivity::unavailable;
    }
    return Discover(now);
}

AudioActivity AudioActivityMonitor::Discover(ULONGLONG now) noexcept {
    if (!device_enumerator_) {
        const HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&device_enumerator_));
        if (FAILED(result)) {
            next_discovery_tick_ = now + kMissingSessionRetryMs;
            return AudioActivity::unavailable;
        }
    }

    ComPtr<IMMDeviceCollection> devices;
    HRESULT result = device_enumerator_->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(result)) {
        device_enumerator_.Reset();
        next_discovery_tick_ = now + kMissingSessionRetryMs;
        return AudioActivity::unavailable;
    }

    UINT device_count = 0;
    if (FAILED(devices->GetCount(&device_count))) {
        return AudioActivity::unavailable;
    }

    const DWORD current_pid = GetCurrentProcessId();
    ComPtr<IAudioSessionControl> inactive_session;

    for (UINT device_index = 0; device_index < device_count; ++device_index) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(device_index, &device))) {
            continue;
        }

        ComPtr<IAudioSessionManager2> session_manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2),
                                    CLSCTX_INPROC_SERVER, nullptr,
                                    reinterpret_cast<void**>(
                                        session_manager.GetAddressOf())))) {
            continue;
        }

        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(session_manager->GetSessionEnumerator(&sessions))) {
            continue;
        }

        int session_count = 0;
        if (FAILED(sessions->GetCount(&session_count))) {
            continue;
        }

        for (int session_index = 0; session_index < session_count;
             ++session_index) {
            ComPtr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(session_index, &session))) {
                continue;
            }

            ComPtr<IAudioSessionControl2> session2;
            if (FAILED(session.As(&session2))) {
                continue;
            }

            DWORD session_pid = 0;
            if (FAILED(session2->GetProcessId(&session_pid)) ||
                session_pid != current_pid) {
                continue;
            }

            const AudioActivity activity = SessionActivity(session.Get());
            if (activity == AudioActivity::active) {
                session_ = session;
                next_discovery_tick_ = 0;
                return AudioActivity::active;
            }
            if (activity == AudioActivity::inactive && !inactive_session) {
                inactive_session = session;
            }
        }
    }

    if (inactive_session) {
        session_ = std::move(inactive_session);
        next_discovery_tick_ = now + kInactiveSessionRefreshMs;
        return AudioActivity::inactive;
    }

    next_discovery_tick_ = now + kMissingSessionRetryMs;
    return AudioActivity::unavailable;
}

AudioActivity GetCurrentProcessAudioActivity() noexcept {
    static thread_local AudioActivityMonitor monitor;
    return monitor.Query();
}

}  // namespace plugin
