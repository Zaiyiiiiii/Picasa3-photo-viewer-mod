// Fallback through Windows Imaging Component: whatever codecs the system has (first frame only).

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <utility>
#include <vector>

#include "../patch.h"
#include "codec.h"

using Microsoft::WRL::ComPtr;

namespace {

WICBitmapTransformOptions TransformForOrientation(USHORT orientation) {
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

// EXIF orientation for containers whose decoder does not already apply it.
USHORT ReadOrientation(IWICBitmapDecoder* decoder, IWICBitmapFrameDecode* frame) {
    GUID container{};
    if (SUCCEEDED(decoder->GetContainerFormat(&container)) && container == GUID_ContainerFormatHeif)
        return 1;  // irot/imir already applied by the HEIF decoder
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    PROPVARIANT v;
    PropVariantInit(&v);
    USHORT result = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) result = v.uiVal;
    PropVariantClear(&v);
    return result;
}

class WicSource : public ImageSource {
public:
    ~WicSource() override {
        frame_.Reset();
        factory_.Reset();
        if (SUCCEEDED(comInit_)) CoUninitialize();
    }

    HRESULT Open(const uint8_t* data, size_t size) {
        comInit_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (size > MAXDWORD) return E_INVALIDARG;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) return hr;
        ComPtr<IWICStream> stream;
        if (FAILED(hr = factory_->CreateStream(&stream))) return hr;
        if (FAILED(hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size)))) return hr;
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = decoder->GetFrame(0, &frame_))) return hr;
        if (FAILED(hr = frame_->GetSize(&width, &height))) return hr;
        transform_ = TransformForOrientation(ReadOrientation(decoder.Get(), frame_.Get()));
        if (transform_ & (WICBitmapTransformRotate90 | WICBitmapTransformRotate270)) std::swap(width, height);
        ReadColorProfile();
        return S_OK;
    }

    int NextFrame(uint8_t* out, uint32_t stride) override {
        if (done_) return -1;
        done_ = true;
        HRESULT hr;
        ComPtr<IWICBitmapSource> source = frame_;
        if (transform_ != WICBitmapTransformRotate0) {
            ComPtr<IWICBitmapFlipRotator> rotator;
            if (SUCCEEDED(hr = factory_->CreateBitmapFlipRotator(&rotator)) &&
                SUCCEEDED(hr = rotator->Initialize(source.Get(), transform_)))
                source = rotator;
            else
                Log("wic: flip/rotate hr=%08lX", hr);
        }
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(hr = factory_->CreateFormatConverter(&converter)) ||
            FAILED(hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                              nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(hr = converter->CopyPixels(nullptr, stride, stride * height, out))) {
            Log("wic: decode hr=%08lX", hr);
            return -1;
        }
        ApplyColor(out, stride);
        return 0;
    }

    bool Rewind() override {
        done_ = false;
        return true;
    }

    const char* Name() const override { return "wic"; }

private:
    // First embedded ICC profile of the frame, if any.
    void ReadColorProfile() {
        UINT count = 0;
        if (FAILED(frame_->GetColorContexts(0, nullptr, &count)) || count == 0) return;
        std::vector<ComPtr<IWICColorContext>> contexts(count);
        std::vector<IWICColorContext*> raw(count);
        for (UINT i = 0; i < count; ++i) {
            if (FAILED(factory_->CreateColorContext(&contexts[i]))) return;
            raw[i] = contexts[i].Get();
        }
        if (FAILED(frame_->GetColorContexts(count, raw.data(), &count))) return;
        for (UINT i = 0; i < count; ++i) {
            WICColorContextType type;
            UINT size = 0;
            if (FAILED(raw[i]->GetType(&type)) || type != WICColorContextProfile ||
                FAILED(raw[i]->GetProfileBytes(0, nullptr, &size)) || size == 0)
                continue;
            std::vector<BYTE> icc(size);
            if (SUCCEEDED(raw[i]->GetProfileBytes(size, icc.data(), &size))) {
                color_ = codec::ConverterFromIcc(icc.data(), size);
                return;
            }
        }
    }

    HRESULT comInit_ = E_FAIL;
    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IWICBitmapFrameDecode> frame_;
    WICBitmapTransformOptions transform_ = WICBitmapTransformRotate0;
    bool done_ = false;
};

}  // namespace

std::unique_ptr<ImageSource> codec::OpenWic(const uint8_t* data, size_t size) {
    auto src = std::make_unique<WicSource>();
    HRESULT hr = src->Open(data, size);
    if (SUCCEEDED(hr)) return src;
    Log("wic: cannot open %u bytes, hr=%08lX", (unsigned)size, hr);
    return nullptr;
}
