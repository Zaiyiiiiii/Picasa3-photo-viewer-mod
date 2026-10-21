#pragma once

#include <cstddef>
#include <cstdint>

struct IAudioClient;
struct IAudioRenderClient;

// Shared-mode WASAPI output on the default device: interleaved 32-bit float stereo at the device's mix
// rate (Windows converts to the device format).
class AudioOut {
public:
    AudioOut() = default;
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    ~AudioOut();

    bool Open();
    int Rate() const { return rate_; }
    static constexpr int kChannels = 2;

    // Queues as many of `frames` as fit in the device buffer; returns how many were taken.
    size_t Write(const float* samples, size_t frames);

    // Seconds of audio that have actually been played since the first Write.
    double PlayedSeconds() const;

private:
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    uint32_t bufferFrames_ = 0;
    uint64_t written_ = 0;
    int rate_ = 0;
    bool started_ = false;
    bool comInit_ = false;
};
