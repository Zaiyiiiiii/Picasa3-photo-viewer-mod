// GIF stills and animations through giflib, composited per frame (disposal, transparency, delays).

#include <gif_lib.h>

#include <cstring>
#include <vector>

#include "../patch.h"
#include "codec.h"

namespace {

struct MemoryReader {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int ReadFromMemory(GifFileType* gif, GifByteType* buf, int len) {
    auto* r = static_cast<MemoryReader*>(gif->UserData);
    size_t n = r->size - r->pos < size_t(len) ? r->size - r->pos : size_t(len);
    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return static_cast<int>(n);
}

class GifSource : public ImageSource {
public:
    ~GifSource() override {
        int err;
        if (gif_) DGifCloseFile(gif_, &err);
    }

    bool Open(const uint8_t* data, size_t size) {
        reader_ = {data, size, 0};
        int err = 0;
        gif_ = DGifOpen(&reader_, ReadFromMemory, &err);
        if (!gif_) {
            Log("gif: open failed (%d)", err);
            return false;
        }
        // DGifSlurp keeps going on truncated files; use whatever frames were read.
        if (DGifSlurp(gif_) != GIF_OK && gif_->ImageCount == 0) {
            Log("gif: no frames (%d)", gif_->Error);
            return false;
        }
        width = gif_->SWidth;
        height = gif_->SHeight;
        frameCount = gif_->ImageCount;
        canvas_.assign(size_t(width) * height, 0);
        return width && height && frameCount;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        if (next_ >= gif_->ImageCount) return -1;
        // Undo the previous frame per its disposal method.
        if (next_ > 0) Dispose(next_ - 1);

        const SavedImage& img = gif_->SavedImages[next_];
        GraphicsControlBlock gcb{};
        gcb.TransparentColor = NO_TRANSPARENT_COLOR;
        DGifSavedExtensionToGCB(gif_, next_, &gcb);
        if (gcb.DisposalMode == DISPOSE_PREVIOUS) saved_ = canvas_;
        Draw(img, gcb.TransparentColor);

        codec::CopyPixels(reinterpret_cast<const uint8_t*>(canvas_.data()), width * 4, width, height, false, out, stride);
        ++next_;
        if (frameCount == 1) return 0;
        int ms = gcb.DelayTime * 10;
        return ms <= 10 ? 100 : ms;  // what browsers do with 0/1 delays
    }

    bool Rewind() override {
        next_ = 0;
        canvas_.assign(canvas_.size(), 0);
        return true;
    }

    const char* Name() const override { return "gif"; }

private:
    void Draw(const SavedImage& img, int transparent) {
        const ColorMapObject* map = img.ImageDesc.ColorMap ? img.ImageDesc.ColorMap : gif_->SColorMap;
        if (!map) return;
        const GifImageDesc& d = img.ImageDesc;
        for (int y = 0; y < d.Height; ++y) {
            int cy = d.Top + y;
            if (cy < 0 || uint32_t(cy) >= height) continue;
            const GifByteType* src = img.RasterBits + size_t(y) * d.Width;
            uint32_t* row = canvas_.data() + size_t(cy) * width;
            for (int x = 0; x < d.Width; ++x) {
                int cx = d.Left + x;
                int index = src[x];
                if (cx < 0 || uint32_t(cx) >= width || index == transparent || index >= map->ColorCount) continue;
                const GifColorType& c = map->Colors[index];
                row[cx] = 0xFF000000u | (uint32_t(c.Red) << 16) | (uint32_t(c.Green) << 8) | c.Blue;
            }
        }
    }

    void Dispose(int frame) {
        GraphicsControlBlock gcb{};
        DGifSavedExtensionToGCB(gif_, frame, &gcb);
        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            const GifImageDesc& d = gif_->SavedImages[frame].ImageDesc;
            for (int y = d.Top; y < d.Top + d.Height && y < int(height); ++y)
                for (int x = d.Left; x < d.Left + d.Width && x < int(width); ++x)
                    if (x >= 0 && y >= 0) canvas_[size_t(y) * width + x] = 0;
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS && !saved_.empty()) {
            canvas_ = saved_;
        }
    }

    MemoryReader reader_{};
    GifFileType* gif_ = nullptr;
    std::vector<uint32_t> canvas_;
    std::vector<uint32_t> saved_;
    int next_ = 0;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenGif(const uint8_t* data, size_t size) {
    auto src = std::make_unique<GifSource>();
    return src->Open(data, size) ? std::move(src) : nullptr;
}
