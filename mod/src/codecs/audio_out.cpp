#include "audio_out.h"

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <cstring>

#include "../patch.h"

namespace {

constexpr REFERENCE_TIME kBufferDuration = 2000000;  // 200 ms, in 100 ns units

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

AudioOut::~AudioOut() {
    if (client_ && started_) client_->Stop();
    Release(render_);
    Release(client_);
    if (comInit_) CoUninitialize();
}

bool AudioOut::Open() {
    comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    WAVEFORMATEX* mix = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix);
    if (SUCCEEDED(hr)) {
        rate_ = static_cast<int>(mix->nSamplesPerSec);
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = kChannels;
        fmt.Format.nSamplesPerSec = mix->nSamplesPerSec;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = kChannels * 4;
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 kBufferDuration, 0, &fmt.Format, nullptr);
    }
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&bufferFrames_);
    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&render_));
    CoTaskMemFree(mix);
    Release(device);
    Release(enumerator);
    if (FAILED(hr)) {
        Log("audio: WASAPI init failed hr=%08lX", hr);
        Release(render_);
        Release(client_);
        return false;
    }
    return true;
}

size_t AudioOut::Write(const float* samples, size_t frames) {
    UINT32 padding = 0;
    if (FAILED(client_->GetCurrentPadding(&padding))) return 0;
    size_t space = bufferFrames_ - padding;
    if (frames > space) frames = space;
    if (frames == 0) return 0;
    BYTE* buf = nullptr;
    if (FAILED(render_->GetBuffer(static_cast<UINT32>(frames), &buf))) return 0;
    memcpy(buf, samples, frames * kChannels * sizeof(float));
    render_->ReleaseBuffer(static_cast<UINT32>(frames), 0);
    written_ += frames;
    if (!started_) started_ = SUCCEEDED(client_->Start());
    return frames;
}

double AudioOut::PlayedSeconds() const {
    UINT32 padding = 0;
    if (!client_ || !started_ || FAILED(client_->GetCurrentPadding(&padding))) return 0;
    return double(written_ - padding) / rate_;
}
