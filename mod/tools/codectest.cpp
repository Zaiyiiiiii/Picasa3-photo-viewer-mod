// Offline check of the codec layer, no viewer involved.
//   codectest [--out DIR] FILE...     decode every frame, print size/frames/durations; with --out, save
//                                     frame 0 of each file as PNG in DIR
//   codectest --make-heic IN OUT [--p3 | --p3-nclx]
//                                     encode any WIC-readable image to HEIC with libheif (test samples);
//                                     --p3 / --p3-nclx tag the pixels as Display P3 (ICC profile / nclx box);
//                                     needs libheif's "hevc" feature (x265), which mod\vcpkg.json leaves
//                                     out because the viewer only decodes

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <lcms2.h>
#include <libheif/heif.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/codecs/codec.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IWICImagingFactory> g_wic;

bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

bool SavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h, UINT stride) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(g_wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
           SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
           SUCCEEDED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

bool IsVideo(const wchar_t* path) {
    static const wchar_t* const kExt[] = {L".mp4", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".m2ts", L".ts"};
    const wchar_t* ext = wcsrchr(path, L'.');
    for (const wchar_t* e : kExt)
        if (ext && _wcsicmp(ext, e) == 0) return true;
    return false;
}

int Decode(const wchar_t* path, const std::wstring& outDir) {
    std::vector<uint8_t> data;
    if (IsVideo(path)) {  // the viewer passes videos as a blob naming the file
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
        data = codec::MakeVideoBlob(utf8);
    } else if (!ReadAll(path, data)) {
        wprintf(L"%s: cannot read\n", path);
        return 1;
    }
    auto src = OpenImageSource(data.data(), data.size());
    if (!src) {
        wprintf(L"%s: no decoder\n", path);
        return 1;
    }
    UINT stride = src->width * 4;
    std::vector<uint8_t> px(size_t(stride) * src->height);
    int frames = 0, totalMs = 0, minMs = INT_MAX, maxMs = 0;
    DWORD t0 = GetTickCount();
    for (int d; frames < 10000 && (d = src->NextFrame(px.data(), stride)) >= 0;) {
        if (frames++ == 0 && !outDir.empty()) {
            const wchar_t* name = wcsrchr(path, L'\\');
            std::wstring png = outDir + L"\\" + (name ? name + 1 : path) + L".png";
            if (!SavePng(png, px.data(), src->width, src->height, stride)) wprintf(L"  cannot write %s\n", png.c_str());
        }
        totalMs += d;
        if (d < minMs) minMs = d;
        if (d > maxMs) maxMs = d;
        if (src->frameCount == 1 || src->Live()) break;  // live sources only have a poster here
    }
    bool rewound = frames > 1 && src->Rewind() && src->NextFrame(px.data(), stride) >= 0;
    wprintf(L"%-28s %-5S %5ux%-5u declared=%u decoded=%d duration=%dms (%d..%d) rewind=%s  %lums\n",
            wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path, src->Name(), src->width, src->height,
            src->frameCount, frames, totalMs, frames ? minMs : 0, maxMs, frames > 1 ? (rewound ? L"ok" : L"FAIL") : L"-",
            GetTickCount() - t0);
    return frames > 0 ? 0 : 1;
}

// Display P3 (D65, sRGB curve) as an ICC profile, built with Little CMS.
std::vector<uint8_t> DisplayP3Icc() {
    cmsCIExyY white{0.3127, 0.3290, 1.0};
    cmsCIExyYTRIPLE prim{{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
    double srgb[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
    cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, srgb);
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &prim, curves);
    cmsFreeToneCurve(curve);
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(p, nullptr, &size);
    std::vector<uint8_t> icc(size);
    cmsSaveProfileToMem(p, icc.data(), &size);
    cmsCloseProfile(p);
    return icc;
}

// p3: 0 untagged, 1 Display P3 ICC profile, 2 Display P3 nclx (CICP 12/13).
int MakeHeic(const wchar_t* in, const wchar_t* out, int p3) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w, h;
    if (FAILED(g_wic->CreateDecoderFromFilename(in, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) ||
        FAILED(g_wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) {
        wprintf(L"cannot read %s\n", in);
        return 1;
    }
    heif_image* img = nullptr;
    heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &img);
    heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
    size_t stride = 0;
    uint8_t* plane = heif_image_get_plane2(img, heif_channel_interleaved, &stride);
    conv->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(stride * h), plane);
    if (p3 == 1) {  // the pixels are taken as Display P3 values
        std::vector<uint8_t> icc = DisplayP3Icc();
        heif_image_set_raw_color_profile(img, "prof", icc.data(), icc.size());
    }
    heif_color_profile_nclx* nclx = nullptr;
    if (p3 == 2) {
        nclx = heif_nclx_color_profile_alloc();
        nclx->color_primaries = heif_color_primaries_SMPTE_EG_432_1;
        nclx->transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1;
        nclx->matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_601_6;
        nclx->full_range_flag = 1;
    }

    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    heif_error err = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &enc);
    if (err.code == heif_error_Ok) {
        heif_encoder_set_lossy_quality(enc, 85);
        heif_encoding_options* opt = heif_encoding_options_alloc();
        opt->output_nclx_profile = nclx;
        err = heif_context_encode_image(ctx, img, enc, opt, nullptr);
        heif_encoding_options_free(opt);
        heif_nclx_color_profile_free(nclx);
        heif_encoder_release(enc);
    }
    if (err.code == heif_error_Ok) {
        char path[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, out, -1, path, sizeof(path), nullptr, nullptr);
        err = heif_context_write_to_file(ctx, path);
    }
    heif_context_free(ctx);
    heif_image_release(img);
    if (err.code != heif_error_Ok) {
        wprintf(L"heif encode failed: %S\n", err.message);
        return 1;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    heif_init(nullptr);
    if ((argc == 4 || argc == 5) && wcscmp(argv[1], L"--make-heic") == 0)
        return MakeHeic(argv[2], argv[3],
                        argc < 5 ? 0 : wcscmp(argv[4], L"--p3") == 0 ? 1 : wcscmp(argv[4], L"--p3-nclx") == 0 ? 2 : 0);

    // --out applies wherever it appears; without it nothing is written.
    std::wstring outDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--out") == 0) outDir = argv[i + 1];
    if (!outDir.empty()) CreateDirectoryW(outDir.c_str(), nullptr);
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--out") == 0) {
            ++i;
            continue;
        }
        failures += Decode(argv[i], outDir);
    }
    return failures;
}
