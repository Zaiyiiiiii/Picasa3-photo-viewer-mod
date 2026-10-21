// HEIC / HEIF stills (primary image). libheif applies irot/imir/clap itself.

#include <libheif/heif.h>

#include <mutex>
#include <vector>

#include "../patch.h"
#include "codec.h"

namespace {

class HeifSource : public ImageSource {
public:
    ~HeifSource() override {
        heif_image_handle_release(handle_);
        heif_context_free(ctx_);
    }

    bool Open(const uint8_t* data, size_t size) {
        static std::once_flag init;
        std::call_once(init, [] { heif_init(nullptr); });
        ctx_ = heif_context_alloc();
        heif_error err = heif_context_read_from_memory_without_copy(ctx_, data, size, nullptr);
        if (err.code != heif_error_Ok) return Fail("read", err);
        err = heif_context_get_primary_image_handle(ctx_, &handle_);
        if (err.code != heif_error_Ok) return Fail("primary", err);
        width = heif_image_handle_get_width(handle_);
        height = heif_image_handle_get_height(handle_);
        ReadColorProfile();
        return width && height;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        if (done_) return -1;
        done_ = true;
        heif_image* img = nullptr;
        heif_error err = heif_decode_image(handle_, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
        if (err.code != heif_error_Ok) {
            Fail("decode", err);
            return -1;
        }
        size_t srcStride = 0;
        const uint8_t* p = heif_image_get_plane_readonly2(img, heif_channel_interleaved, &srcStride);
        int w = heif_image_get_primary_width(img), h = heif_image_get_primary_height(img);
        if (p && uint32_t(w) == width && uint32_t(h) == height) {
            codec::CopyPixels(p, static_cast<uint32_t>(srcStride), width, height, true, out, stride);
            ApplyColor(out, stride);
        } else
            Log("heif: decoded %dx%d, expected %ux%u", w, h, width, height);
        heif_image_release(img);
        return p ? 0 : -1;
    }

    bool Rewind() override {
        done_ = false;
        return true;
    }

    const char* Name() const override { return "heif"; }

private:
    void ReadColorProfile() {
        heif_color_profile_type type = heif_image_handle_get_color_profile_type(handle_);
        if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
            std::vector<uint8_t> icc(heif_image_handle_get_raw_color_profile_size(handle_));
            if (!icc.empty() && heif_image_handle_get_raw_color_profile(handle_, icc.data()).code == heif_error_Ok)
                color_ = codec::ConverterFromIcc(icc.data(), icc.size());
        } else if (type == heif_color_profile_type_nclx) {
            heif_color_profile_nclx* nclx = nullptr;
            if (heif_image_handle_get_nclx_color_profile(handle_, &nclx).code == heif_error_Ok && nclx)
                color_ = codec::ConverterFromCicp(nclx->color_primaries, nclx->transfer_characteristics);
            heif_nclx_color_profile_free(nclx);
        }
    }

    bool Fail(const char* what, const heif_error& err) {
        Log("heif: %s failed: %s", what, err.message ? err.message : "?");
        return false;
    }

    heif_context* ctx_ = nullptr;
    heif_image_handle* handle_ = nullptr;
    bool done_ = false;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenHeif(const uint8_t* data, size_t size) {
    auto src = std::make_unique<HeifSource>();
    return src->Open(data, size) ? std::move(src) : nullptr;
}
