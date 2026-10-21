// AVIF stills and image sequences. libavif leaves irot/imir to the caller.

#include <avif/avif.h>

#include <vector>

#include "../patch.h"
#include "codec.h"

namespace {

class AvifSource : public ImageSource {
public:
    ~AvifSource() override { avifDecoderDestroy(dec_); }

    bool Open(const uint8_t* data, size_t size) {
        dec_ = avifDecoderCreate();
        if (!dec_) return false;
        dec_->maxThreads = 4;
        avifResult r = avifDecoderSetIOMemory(dec_, data, size);
        if (r == AVIF_RESULT_OK) r = avifDecoderParse(dec_);
        if (r != AVIF_RESULT_OK) {
            Log("avif: parse failed: %s", avifResultToString(r));
            return false;
        }
        const avifImage* img = dec_->image;
        if (img->transformFlags & AVIF_TRANSFORM_IROT) turnsCW_ = (4 - (img->irot.angle & 3)) & 3;  // irot is CCW
        if (img->transformFlags & AVIF_TRANSFORM_IMIR) mirror_ = img->imir.axis == 0 ? 1 : 2;
        width = (turnsCW_ & 1) ? img->height : img->width;
        height = (turnsCW_ & 1) ? img->width : img->height;
        frameCount = dec_->imageCount > 0 ? static_cast<uint32_t>(dec_->imageCount) : 0;
        if (img->icc.size)
            color_ = codec::ConverterFromIcc(img->icc.data, img->icc.size);
        else
            color_ = codec::ConverterFromCicp(img->colorPrimaries, img->transferCharacteristics);
        return width && height;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        avifResult r = avifDecoderNextImage(dec_);
        if (r != AVIF_RESULT_OK) {
            if (r != AVIF_RESULT_NO_IMAGES_REMAINING) Log("avif: decode failed: %s", avifResultToString(r));
            return -1;
        }
        const avifImage* img = dec_->image;
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, img);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        if (turnsCW_ == 0) {
            rgb.pixels = out;
            rgb.rowBytes = stride;
        } else {
            scratch_.resize(size_t(img->width) * img->height * 4);
            rgb.pixels = scratch_.data();
            rgb.rowBytes = img->width * 4;
        }
        r = avifImageYUVToRGB(img, &rgb);
        if (r != AVIF_RESULT_OK) {
            Log("avif: yuv->rgb failed: %s", avifResultToString(r));
            return -1;
        }
        if (turnsCW_) codec::Rotate(rgb.pixels, rgb.rowBytes, img->width, img->height, turnsCW_, out, stride);
        if (mirror_) codec::Mirror(out, stride, width, height, mirror_ == 1);
        ApplyColor(out, stride);
        return frameCount > 1 ? static_cast<int>(dec_->imageTiming.duration * 1000.0 + 0.5) : 0;
    }

    bool Rewind() override { return avifDecoderReset(dec_) == AVIF_RESULT_OK; }

    const char* Name() const override { return "avif"; }

private:
    avifDecoder* dec_ = nullptr;
    int turnsCW_ = 0;
    int mirror_ = 0;  // 1 = left-right, 2 = top-bottom
    std::vector<uint8_t> scratch_;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenAvif(const uint8_t* data, size_t size) {
    auto src = std::make_unique<AvifSource>();
    return src->Open(data, size) ? std::move(src) : nullptr;
}
