#include <webp/demux.h>

#include "../patch.h"
#include "codec.h"

namespace {

class WebPSource : public ImageSource {
public:
    ~WebPSource() override { WebPAnimDecoderDelete(dec_); }

    bool Open(const uint8_t* data, size_t size) {
        WebPAnimDecoderOptions opt;
        if (!WebPAnimDecoderOptionsInit(&opt)) return false;
        opt.color_mode = MODE_BGRA;
        opt.use_threads = 1;
        WebPData wd{data, size};
        dec_ = WebPAnimDecoderNew(&wd, &opt);
        if (!dec_) return false;
        WebPAnimInfo info;
        if (!WebPAnimDecoderGetInfo(dec_, &info)) return false;
        width = info.canvas_width;
        height = info.canvas_height;
        frameCount = info.frame_count;
        WebPChunkIterator chunk;
        if (WebPDemuxGetChunk(WebPAnimDecoderGetDemuxer(dec_), "ICCP", 1, &chunk)) {
            color_ = codec::ConverterFromIcc(chunk.chunk.bytes, chunk.chunk.size);
            WebPDemuxReleaseChunkIterator(&chunk);
        }
        return width && height;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        uint8_t* buf;
        int timestamp;
        if (!WebPAnimDecoderHasMoreFrames(dec_) || !WebPAnimDecoderGetNext(dec_, &buf, &timestamp)) return -1;
        codec::CopyPixels(buf, width * 4, width, height, false, out, stride);
        ApplyColor(out, stride);
        int duration = frameCount > 1 ? timestamp - lastTimestamp_ : 0;
        lastTimestamp_ = timestamp;
        return duration;
    }

    bool Rewind() override {
        WebPAnimDecoderReset(dec_);
        lastTimestamp_ = 0;
        return true;
    }

    const char* Name() const override { return "webp"; }

private:
    WebPAnimDecoder* dec_ = nullptr;
    int lastTimestamp_ = 0;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenWebP(const uint8_t* data, size_t size) {
    auto src = std::make_unique<WebPSource>();
    if (src->Open(data, size)) return src;
    Log("webp: cannot parse %u bytes", (unsigned)size);
    return nullptr;
}
