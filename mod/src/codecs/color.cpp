// Embedded-profile color conversion for the new formats (Little CMS).
//
// The viewer's own pipeline only color-manages when "EnableColorManagement" is on, and then converts to
// the display profile. Pictures from phones are mostly Display P3; shown as if they were sRGB they look
// washed out, so the new formats are always converted: to the display profile when the host's color
// management is on, to sRGB otherwise.

#include "color.h"

#include <windows.h>

#include <lcms2.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "../patch.h"

namespace codec {

bool (*g_useDisplayProfile)() = nullptr;

namespace {

// The display's ICC profile, read once (empty if the system has none).
const std::vector<uint8_t>& DisplayProfileBytes() {
    static std::vector<uint8_t> bytes;
    static std::once_flag once;
    std::call_once(once, [] {
        HDC dc = GetDC(nullptr);
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        BOOL ok = GetICMProfileW(dc, &n, path);
        ReleaseDC(nullptr, dc);
        if (!ok) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (16 << 20)) {
            bytes.resize(size);
            if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        fclose(f);
        Log("color: display profile %S (%u bytes)", path, (unsigned)bytes.size());
    });
    return bytes;
}

cmsHPROFILE OpenOutput(bool display) {
    if (display) {
        const auto& b = DisplayProfileBytes();
        if (!b.empty())
            if (cmsHPROFILE p = cmsOpenProfileFromMem(b.data(), static_cast<cmsUInt32Number>(b.size()))) return p;
    }
    return cmsCreate_sRGBProfile();
}

// True when the transform leaves a grid of colors unchanged (within rounding).
bool IsIdentity(cmsHTRANSFORM t) {
    std::vector<uint8_t> in, out;
    for (int b = 0; b <= 256; b += 64)
        for (int g = 0; g <= 256; g += 64)
            for (int r = 0; r <= 256; r += 64) {
                uint8_t px[4] = {uint8_t(std::min(b, 255)), uint8_t(std::min(g, 255)), uint8_t(std::min(r, 255)), 255};
                in.insert(in.end(), px, px + 4);
            }
    out.resize(in.size());
    cmsDoTransform(t, in.data(), out.data(), static_cast<cmsUInt32Number>(in.size() / 4));
    for (size_t i = 0; i < in.size(); ++i)
        if (std::abs(int(in[i]) - int(out[i])) > 1) return false;
    return true;
}

class LcmsConverter : public ColorConverter {
public:
    explicit LcmsConverter(cmsHPROFILE source) : source_(source) {}
    ~LcmsConverter() override {
        for (cmsHTRANSFORM t : transforms_)
            if (t) cmsDeleteTransform(t);
        cmsCloseProfile(source_);
    }

    // False when converting to sRGB would change nothing.
    bool Useful() { return Transform(false) != nullptr; }

    void Apply(uint8_t* pixels, uint32_t stride, uint32_t width, uint32_t height) const override {
        bool display = g_useDisplayProfile && g_useDisplayProfile();
        cmsHTRANSFORM t = const_cast<LcmsConverter*>(this)->Transform(display);
        if (!t) return;
        // NOCACHE transforms can run on several threads at once.
        unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        if (uint64_t(width) * height < (1u << 20)) threads = 1;
        uint32_t rowsPer = (height + threads - 1) / threads;
        auto run = [=](uint32_t y0) {
            uint32_t rows = std::min(rowsPer, height - y0);
            uint8_t* p = pixels + size_t(y0) * stride;
            cmsDoTransformLineStride(t, p, p, width, rows, stride, stride, 0, 0);
        };
        std::vector<std::thread> pool;
        for (uint32_t y = rowsPer; y < height; y += rowsPer) pool.emplace_back(run, y);
        run(0);
        for (auto& th : pool) th.join();
    }

private:
    cmsHTRANSFORM Transform(bool display) {
        std::lock_guard<std::mutex> lock(mutex_);
        int slot = display ? 1 : 0;
        if (!built_[slot]) {
            built_[slot] = true;
            cmsHPROFILE out = OpenOutput(display);
            cmsHTRANSFORM t = cmsCreateTransform(source_, TYPE_BGRA_8, out, TYPE_BGRA_8, INTENT_PERCEPTUAL,
                                                 cmsFLAGS_COPY_ALPHA | cmsFLAGS_NOCACHE);
            cmsCloseProfile(out);
            if (t && IsIdentity(t)) {
                cmsDeleteTransform(t);
                t = nullptr;
            }
            transforms_[slot] = t;
        }
        return transforms_[slot];
    }

    cmsHPROFILE source_;
    std::mutex mutex_;
    bool built_[2] = {};
    cmsHTRANSFORM transforms_[2] = {};
};

std::unique_ptr<ColorConverter> FromProfile(cmsHPROFILE p, const char* what) {
    if (!p) return nullptr;
    if (cmsGetColorSpace(p) != cmsSigRgbData) {
        cmsCloseProfile(p);
        return nullptr;
    }
    auto c = std::make_unique<LcmsConverter>(p);
    if (!c->Useful()) return nullptr;
    char desc[128] = "";
    cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", desc, sizeof(desc));
    Log("color: converting from %s \"%s\"", what, desc);
    return c;
}

}  // namespace

std::unique_ptr<ColorConverter> ConverterFromIcc(const void* icc, size_t size) {
    if (!icc || size < 128) return nullptr;
    return FromProfile(cmsOpenProfileFromMem(icc, static_cast<cmsUInt32Number>(size)), "ICC");
}

std::unique_ptr<ColorConverter> ConverterFromCicp(int primaries, int transfer) {
    bool sdrCurve = transfer == 1 || transfer == 2 || transfer == 6 || transfer == 13 || transfer == 14 ||
                    transfer == 15;
    if ((primaries == 1 || primaries == 2) && sdrCurve) return nullptr;  // BT.709 / sRGB as is

    const cmsCIExyY d65{0.3127, 0.3290, 1.0}, illumC{0.310, 0.316, 1.0}, dci{0.314, 0.351, 1.0};
    cmsCIExyY white = d65;
    cmsCIExyYTRIPLE prim;
    auto set = [&prim](double rx, double ry, double gx, double gy, double bx, double by) {
        prim = {{rx, ry, 1.0}, {gx, gy, 1.0}, {bx, by, 1.0}};
    };
    switch (primaries) {
        case 1: case 2: set(0.64, 0.33, 0.30, 0.60, 0.15, 0.06); break;
        case 4: set(0.67, 0.33, 0.21, 0.71, 0.14, 0.08); white = illumC; break;
        case 5: set(0.64, 0.33, 0.29, 0.60, 0.15, 0.06); break;
        case 6: case 7: set(0.630, 0.340, 0.310, 0.595, 0.155, 0.070); break;
        case 8: set(0.681, 0.319, 0.243, 0.692, 0.145, 0.049); white = illumC; break;
        case 9: set(0.708, 0.292, 0.170, 0.797, 0.131, 0.046); break;
        case 11: set(0.680, 0.320, 0.265, 0.690, 0.150, 0.060); white = dci; break;
        case 12: set(0.680, 0.320, 0.265, 0.690, 0.150, 0.060); break;
        case 22: set(0.630, 0.340, 0.295, 0.605, 0.155, 0.077); break;
        default: return nullptr;
    }

    // Parametric type 4: Y = (aX + b)^g for X >= d, Y = cX below.
    double bt709[5] = {1 / 0.45, 1 / 1.099, 0.099 / 1.099, 1 / 4.5, 0.081};
    double srgb[5] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
    cmsToneCurve* curve;
    switch (transfer) {
        case 1: case 6: case 7: case 14: case 15: curve = cmsBuildParametricToneCurve(nullptr, 4, bt709); break;
        case 2: case 13: curve = cmsBuildParametricToneCurve(nullptr, 4, srgb); break;
        case 4: curve = cmsBuildGamma(nullptr, 2.2); break;
        case 5: curve = cmsBuildGamma(nullptr, 2.8); break;
        case 8: curve = cmsBuildGamma(nullptr, 1.0); break;
        default: return nullptr;  // PQ, HLG and others: no tone mapping here
    }
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &prim, curves);
    cmsFreeToneCurve(curve);
    char what[32];
    sprintf_s(what, "cicp %d/%d", primaries, transfer);
    return FromProfile(p, what);
}

}  // namespace codec
