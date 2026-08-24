#pragma once

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace plugin {

enum class AudioActivity {
    unavailable,
    inactive,
    active,
};

class AudioActivityMonitor final {
public:
    AudioActivity Query() noexcept;

private:
    AudioActivity Discover(ULONGLONG now) noexcept;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> device_enumerator_;
    Microsoft::WRL::ComPtr<IAudioSessionControl> session_;
    ULONGLONG next_discovery_tick_ = 0;
};

AudioActivity GetCurrentProcessAudioActivity() noexcept;

}  // namespace plugin
