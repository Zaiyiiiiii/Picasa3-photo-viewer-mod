// JPEG XL stills and animations. libjxl applies the orientation by default.

#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>

#include <vector>

#include "../patch.h"
#include "codec.h"

namespace {

class JxlSource : public ImageSource {
public:
    ~JxlSource() override {
        JxlDecoderDestroy(dec_);
        JxlThreadParallelRunnerDestroy(runner_);
    }

    bool Open(const uint8_t* data, size_t size) {
        data_ = data;
        size_ = size;
        runner_ = JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
        if (!Start()) return false;
        // Run to the basic info.
        for (;;) {
            JxlDecoderStatus s = JxlDecoderProcessInput(dec_);
            if (s == JXL_DEC_BASIC_INFO) break;
            if (s != JXL_DEC_COLOR_ENCODING) {
                Log("jxl: no basic info (status %d)", s);
                return false;
            }
        }
        JxlBasicInfo info;
        if (JxlDecoderGetBasicInfo(dec_, &info) != JXL_DEC_SUCCESS) return false;
        bool swap = info.orientation >= JXL_ORIENT_TRANSPOSE;
        width = swap ? info.ysize : info.xsize;
        height = swap ? info.xsize : info.ysize;
        animated_ = info.have_animation;
        if (animated_ && info.animation.tps_numerator)
            msPerTick_ = 1000.0 * info.animation.tps_denominator / info.animation.tps_numerator;
        frameCount = animated_ ? 0 : 1;
        // The color encoding follows the basic info; it describes the pixels the decoder will output.
        if (JxlDecoderProcessInput(dec_) == JXL_DEC_COLOR_ENCODING) {
            size_t iccSize = 0;
            if (JxlDecoderGetICCProfileSize(dec_, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize) == JXL_DEC_SUCCESS) {
                std::vector<uint8_t> icc(iccSize);
                if (JxlDecoderGetColorAsICCProfile(dec_, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), icc.size()) ==
                    JXL_DEC_SUCCESS)
                    color_ = codec::ConverterFromIcc(icc.data(), icc.size());
            }
        }
        return width && height;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        int durationMs = 0;
        for (;;) {
            JxlDecoderStatus s = JxlDecoderProcessInput(dec_);
            switch (s) {
                case JXL_DEC_FRAME: {
                    JxlFrameHeader fh;
                    if (JxlDecoderGetFrameHeader(dec_, &fh) == JXL_DEC_SUCCESS && animated_)
                        durationMs = static_cast<int>(fh.duration * msPerTick_ + 0.5);
                    break;
                }
                case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                    JxlPixelFormat fmt{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
                    scratch_.resize(size_t(width) * height * 4);
                    if (JxlDecoderSetImageOutBuffer(dec_, &fmt, scratch_.data(), scratch_.size()) != JXL_DEC_SUCCESS)
                        return -1;
                    break;
                }
                case JXL_DEC_FULL_IMAGE:
                    codec::CopyPixels(scratch_.data(), width * 4, width, height, true, out, stride);
                    ApplyColor(out, stride);
                    return animated_ ? durationMs : 0;
                case JXL_DEC_SUCCESS:
                    return -1;  // no more frames
                case JXL_DEC_COLOR_ENCODING:
                case JXL_DEC_BASIC_INFO:
                    break;
                default:
                    Log("jxl: decode status %d", s);
                    return -1;
            }
        }
    }

    bool Rewind() override {
        // Restart and skip the header events.
        if (!Start()) return false;
        for (;;) {
            JxlDecoderStatus s = JxlDecoderProcessInput(dec_);
            if (s == JXL_DEC_BASIC_INFO) return true;
            if (s != JXL_DEC_COLOR_ENCODING) return false;
        }
    }

    const char* Name() const override { return "jxl"; }

private:
    bool Start() {
        JxlDecoderDestroy(dec_);
        dec_ = JxlDecoderCreate(nullptr);
        if (!dec_) return false;
        if (runner_) JxlDecoderSetParallelRunner(dec_, JxlThreadParallelRunner, runner_);
        if (JxlDecoderSubscribeEvents(dec_, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
            return false;
        if (JxlDecoderSetInput(dec_, data_, size_) != JXL_DEC_SUCCESS) return false;
        JxlDecoderCloseInput(dec_);
        return true;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    JxlDecoder* dec_ = nullptr;
    void* runner_ = nullptr;
    bool animated_ = false;
    double msPerTick_ = 0;
    std::vector<uint8_t> scratch_;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenJxl(const uint8_t* data, size_t size) {
    auto src = std::make_unique<JxlSource>();
    return src->Open(data, size) ? std::move(src) : nullptr;
}
