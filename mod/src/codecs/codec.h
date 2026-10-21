#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "color.h"

// A still image or animation decoded to 32-bit BGRA, not premultiplied, orientation already applied.
// Sources do not own the encoded data: it must outlive the source.
class ImageSource {
public:
    virtual ~ImageSource() = default;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameCount = 1;  // 0 when unknown up front

    // Decode the next frame (frame 0 first) into `out` (height rows of `stride` bytes).
    // Returns the frame duration in milliseconds (0 for a still image), or -1 on error / end of stream.
    virtual int NextFrame(uint8_t* out, uint32_t stride) = 0;

    // Restart from frame 0. Returns false if unsupported.
    virtual bool Rewind() { return false; }

    // Real-time sources (video) run on their own clock once started instead of NextFrame() pacing.
    virtual bool Live() const { return false; }
    virtual bool StartLive() { return false; }
    // True when a new frame is waiting; cheap, does not consume it.
    virtual bool FrameReady() const { return false; }
    // Render the current frame scaled to w x h (32-bit BGRA, stride in bytes). Clears FrameReady().
    virtual bool RenderLive(uint8_t* /*dst*/, uint32_t /*w*/, uint32_t /*h*/, uint32_t /*stride*/) { return false; }

    // Short backend name for logs.
    virtual const char* Name() const = 0;

protected:
    // Embedded color space -> output space, applied by the backend to every decoded frame.
    void ApplyColor(uint8_t* out, uint32_t stride) const {
        if (color_) color_->Apply(out, stride, width, height);
    }
    std::unique_ptr<codec::ColorConverter> color_;
};

// Picks a backend from the file signature, falling back to WIC. Returns null if nothing can decode it.
std::unique_ptr<ImageSource> OpenImageSource(const uint8_t* data, size_t size);

namespace codec {

std::unique_ptr<ImageSource> OpenWebP(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenHeif(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenAvif(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenJxl(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenGif(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenVideo(const uint8_t* data, size_t size);

// Videos are never read into memory: the load path is given a small blob naming the file instead.
std::vector<uint8_t> MakeVideoBlob(const char* utf8Path);
bool IsVideoBlob(const uint8_t* data, size_t size);
std::unique_ptr<ImageSource> OpenWic(const uint8_t* data, size_t size);

// Copy `src` (sw x sh RGBA or BGRA, srcStride bytes per row) to BGRA `dst`, swapping R and B if `swapRB`.
void CopyPixels(const uint8_t* src, uint32_t srcStride, uint32_t sw, uint32_t sh, bool swapRB, uint8_t* dst,
                uint32_t dstStride);

// Rotate a BGRA image clockwise by quarterTurns * 90 degrees into dst (dimensions swap for odd turns).
void Rotate(const uint8_t* src, uint32_t srcStride, uint32_t sw, uint32_t sh, int quarterTurns, uint8_t* dst,
            uint32_t dstStride);

// Mirror a BGRA image in place: left-right if `horizontal`, else top-bottom.
void Mirror(uint8_t* img, uint32_t stride, uint32_t w, uint32_t h, bool horizontal);

}  // namespace codec
