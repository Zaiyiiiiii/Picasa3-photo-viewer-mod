#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace codec {

// Converts decoded BGRA pixels (alpha untouched) from an image's own color space to the output space:
// the display's ICC profile when the host says color management is on, sRGB otherwise (what the viewer
// assumes for unmanaged pictures).
class ColorConverter {
public:
    virtual ~ColorConverter() = default;
    virtual void Apply(uint8_t* pixels, uint32_t stride, uint32_t width, uint32_t height) const = 0;
};

// Null when the profile is unusable or already matches the output space.
std::unique_ptr<ColorConverter> ConverterFromIcc(const void* icc, size_t size);
// ITU-T H.273 colour primaries / transfer characteristics (as in nclx boxes and AV1/HEVC headers).
// Null for sRGB-like spaces, HDR transfers (PQ, HLG) and unknown codes.
std::unique_ptr<ColorConverter> ConverterFromCicp(int primaries, int transfer);

// Set by the host: returns true to target the display profile instead of sRGB. Checked per conversion.
extern bool (*g_useDisplayProfile)();

}  // namespace codec
