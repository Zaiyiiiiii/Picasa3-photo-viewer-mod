#include "codec.h"

#include <cstring>

namespace {

bool HasFtypBrand(const uint8_t* data, size_t size, const char* const* brands) {
    if (size < 16 || memcmp(data + 4, "ftyp", 4) != 0) return false;
    uint32_t boxSize = (uint32_t(data[0]) << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    if (boxSize < 16 || boxSize > size) boxSize = static_cast<uint32_t>(size < 64 ? size : 64);
    // major brand at 8, compatible brands from 16
    for (uint32_t off = 8; off + 4 <= boxSize; off += (off == 8 ? 8 : 4))
        for (const char* const* b = brands; *b; ++b)
            if (memcmp(data + off, *b, 4) == 0) return true;
    return false;
}

}  // namespace

std::unique_ptr<ImageSource> OpenImageSource(const uint8_t* data, size_t size) {
    static const char* const kAvifBrands[] = {"avif", "avis", nullptr};
    static const char* const kHeifBrands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", nullptr};
    static const uint8_t kJxlContainer[] = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

    if (!data || size < 12) return nullptr;
    if (codec::IsVideoBlob(data, size)) return codec::OpenVideo(data, size);
    std::unique_ptr<ImageSource> src;
    if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
        src = codec::OpenWebP(data, size);
    else if (HasFtypBrand(data, size, kAvifBrands))
        src = codec::OpenAvif(data, size);
    else if (HasFtypBrand(data, size, kHeifBrands))
        src = codec::OpenHeif(data, size);
    else if ((data[0] == 0xFF && data[1] == 0x0A) || memcmp(data, kJxlContainer, sizeof(kJxlContainer)) == 0)
        src = codec::OpenJxl(data, size);
    else if (memcmp(data, "GIF8", 4) == 0)
        src = codec::OpenGif(data, size);
    if (!src) src = codec::OpenWic(data, size);
    return src;
}

namespace codec {

void CopyPixels(const uint8_t* src, uint32_t srcStride, uint32_t sw, uint32_t sh, bool swapRB, uint8_t* dst,
                uint32_t dstStride) {
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* s = src + size_t(y) * srcStride;
        uint8_t* d = dst + size_t(y) * dstStride;
        if (!swapRB) {
            memcpy(d, s, size_t(sw) * 4);
            continue;
        }
        for (uint32_t x = 0; x < sw; ++x, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
}

void Rotate(const uint8_t* src, uint32_t srcStride, uint32_t sw, uint32_t sh, int quarterTurns, uint8_t* dst,
            uint32_t dstStride) {
    quarterTurns &= 3;
    uint32_t dw = (quarterTurns & 1) ? sh : sw;
    uint32_t dh = (quarterTurns & 1) ? sw : sh;
    for (uint32_t y = 0; y < dh; ++y) {
        auto* d = reinterpret_cast<uint32_t*>(dst + size_t(y) * dstStride);
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t sx, sy;
            switch (quarterTurns) {
                case 1: sx = y; sy = sh - 1 - x; break;            // 90 clockwise
                case 2: sx = sw - 1 - x; sy = sh - 1 - y; break;   // 180
                case 3: sx = sw - 1 - y; sy = x; break;            // 90 counter-clockwise
                default: sx = x; sy = y; break;
            }
            d[x] = reinterpret_cast<const uint32_t*>(src + size_t(sy) * srcStride)[sx];
        }
    }
}

void Mirror(uint8_t* img, uint32_t stride, uint32_t w, uint32_t h, bool horizontal) {
    if (horizontal) {
        for (uint32_t y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(img + size_t(y) * stride);
            for (uint32_t a = 0, b = w - 1; a < b; ++a, --b) {
                uint32_t t = row[a];
                row[a] = row[b];
                row[b] = t;
            }
        }
        return;
    }
    for (uint32_t a = 0, b = h - 1; a < b; ++a, --b) {
        auto* ra = reinterpret_cast<uint32_t*>(img + size_t(a) * stride);
        auto* rb = reinterpret_cast<uint32_t*>(img + size_t(b) * stride);
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t t = ra[x];
            ra[x] = rb[x];
            rb[x] = t;
        }
    }
}

}  // namespace codec
