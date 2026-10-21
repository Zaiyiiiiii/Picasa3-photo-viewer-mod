// Enlarges the original UI by UiScale() once the viewer runs DPI aware (dpi.cpp).
//
// The UI is a Photoshop design exported to respack.yt (ytPSDPack): every layer is a rect on the design
// canvas plus its pixels, and the layout script places nodes from those rects with constraints.
//   0x53b410  ReadLayer(pack, index)       reads {int16 l, t, r, b; flags; type} and the pixels (raw,
//             run-length or a solid color) into an entry {l, t, r, b, flags, color, ytBitmap, ...}, then
//   0x53ce60  CacheLayer(index, entry)     EAX = cache; called at 0x53b509 (solid) and 0x53b593.
//             Every rect and bitmap of the pack, the canvas bounds ("docbounds") included, is served
//             from that cache.
//   0x4cd050  ytBitmap::Resize(w, h, bmp)  __fastcall; reallocates, old pixels are not kept.
//   ytBitmap: +4 stride (pixels), +8 width, +0xc height, +0x10 pixels (premultiplied BGRA),
//             +0x18 / +0x1c origin.
// Layers are scaled on their way into the cache, so rects, hit areas and MaintainOffset distances all
// follow. The layout script (one line at a time through
//   0x4de4e0  ParseLayoutLine(ECX = line, EDX, [esp+4])   caller cleans the stack)
// also holds pixel values: the offset of "X/YConstraint parentFraction, selfFraction, offset" and the
// fontsize / fonttrack / fontleading / round properties.
//
// The filmstrip (C3DFilmstrip, vtable 0x720a54) has its own pixel constants in code:
//   0x41a860  SetCellThumbnail   fits the thumbnail into a 40 x 40 box   (mov eax, 0x28 at 0x41a9be)
//   0x41b390  LayoutCells        cell pitch 42.0 (fmul [0x74b840] at 0x41b747) and vertical offset
//                                -16.0f (fld [0x74b8f8] at 0x41b778)
//   0x41a380  ClickHandler       accepts clicks within 8.0 of the strip (fld [0x71fe78] at 0x41a3a4)
// Those constants are shared with other code, so the instructions are pointed at scaled copies.
//
// Text nodes (0x508090 TextBounds) add 4 pixels to the right of their measured text (add esi, 4 at
// 0x5085d2). Button labels are placed with "XConstraint 0.5, 0.5, 2" to make up for it; that offset is
// scaled with the script, so the padding is scaled too or the labels sit off centre.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dpi.h"
#include "hooks.h"
#include "patch.h"

namespace {

constexpr uintptr_t kCacheLayer = 0x53ce60;
constexpr uintptr_t kResizeBitmap = 0x4cd050;
constexpr uintptr_t kParseLayoutLine = 0x4de4e0;

using ResizeFn = int(__fastcall*)(int w, int h, void* bitmap);

// Catmull-Rom weights for the 4 taps around a sample position.
struct Taps {
    int first;
    float w[4];
};

// The layer is stretched over exactly dstLen pixels (not scale * srcLen, which is rarely whole): what is
// symmetric stays symmetric, which matters for the stretched button frames (decrect: the middle row and
// column of the bitmap are repeated between its halves).
std::vector<Taps> MakeTaps(int dstLen, int srcLen) {
    std::vector<Taps> taps(dstLen);
    for (int i = 0; i < dstLen; ++i) {
        double pos = (i + 0.5) * srcLen / dstLen - 0.5;  // in source pixels
        int base = static_cast<int>(std::floor(pos));
        double t = pos - base;
        double t2 = t * t, t3 = t2 * t;
        double w[4] = {(-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2,
                       (t3 - t2) / 2};
        taps[i].first = base - 1;
        for (int k = 0; k < 4; ++k) taps[i].w[k] = static_cast<float>(w[k]);
    }
    return taps;
}

// Resamples a premultiplied BGRA layer to dw x dh. Samples outside the layer repeat its edge.
void ScaleLayer(const uint32_t* src, int sw, int sh, int srcStride, uint32_t* dst, int dw, int dh, int dstStride) {
    std::vector<Taps> tx = MakeTaps(dw, sw), ty = MakeTaps(dh, sh);
    std::vector<float> rows(size_t(sh) * dw * 4);  // horizontal pass
    for (int y = 0; y < sh; ++y) {
        const uint32_t* in = src + size_t(y) * srcStride;
        float* out = rows.data() + size_t(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {};
            for (int k = 0; k < 4; ++k) {
                uint32_t p = in[std::clamp(tx[x].first + k, 0, sw - 1)];
                for (int c = 0; c < 4; ++c) acc[c] += tx[x].w[k] * float((p >> (8 * c)) & 255);
            }
            memcpy(out + x * 4, acc, sizeof(acc));
        }
    }
    for (int y = 0; y < dh; ++y) {
        uint32_t* out = dst + size_t(y) * dstStride;
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {};
            for (int k = 0; k < 4; ++k) {
                const float* p = rows.data() + (size_t(std::clamp(ty[y].first + k, 0, sh - 1)) * dw + x) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += ty[y].w[k] * p[c];
            }
            int a = std::clamp(int(acc[3] + 0.5f), 0, 255);
            uint32_t px = uint32_t(a) << 24;
            for (int c = 0; c < 3; ++c) px |= uint32_t(std::clamp(int(acc[c] + 0.5f), 0, a)) << (8 * c);
            out[x] = px;
        }
    }
}

int ScaleEdge(int v, double s) { return static_cast<int>(std::lround(v * s)); }

extern "C" void __cdecl ScaleLayerEntry(int* entry) {
    double s = UiScale();
    if (s == 1.0 || !entry) return;
    int l = entry[0], t = entry[1], r = entry[2], b = entry[3];
    if (l == -1 && t == -1 && r == -1 && b == -1) return;
    // Size first, so layers of equal size stay equal wherever they sit.
    int nl = ScaleEdge(l, s), nt = ScaleEdge(t, s), nr = nl + ScaleEdge(r - l, s), nb = nt + ScaleEdge(b - t, s);
    entry[0] = nl;
    entry[1] = nt;
    entry[2] = nr;
    entry[3] = nb;

    auto* bitmap = reinterpret_cast<uint8_t*>(entry + 6);
    auto field = [bitmap](size_t off) -> int& { return *reinterpret_cast<int*>(bitmap + off); };
    int sw = field(8), sh = field(0xc), stride = field(4);
    auto* pixels = reinterpret_cast<uint32_t*>(field(0x10));
    if (!pixels || sw <= 0 || sh <= 0 || nr <= nl || nb <= nt) return;
    std::vector<uint32_t> copy(size_t(sw) * sh);
    for (int y = 0; y < sh; ++y) memcpy(copy.data() + size_t(y) * sw, pixels + size_t(y) * stride, sw * 4);
    int ox = field(0x18), oy = field(0x1c);
    if (reinterpret_cast<ResizeFn>(kResizeBitmap)(nr - nl, nb - nt, bitmap) != 0 || !field(0x10)) {
        Log("uiscale: cannot resize a %dx%d layer", sw, sh);
        return;
    }
    ScaleLayer(copy.data(), sw, sh, sw, reinterpret_cast<uint32_t*>(field(0x10)), field(8), field(0xc), field(4));
    field(0x18) = ScaleEdge(ox, s);
    field(0x1c) = ScaleEdge(oy, s);
}

uintptr_t g_cacheLayer = kCacheLayer;

// Replaces `call CacheLayer`: scales the entry ([esp+8] at entry), then continues into CacheLayer with
// its EAX.
extern "C" __declspec(naked) void Thunk_CacheLayer() {
    __asm {
        push eax
        push dword ptr [esp + 12]
        call ScaleLayerEntry
        add esp, 4
        pop eax
        jmp dword ptr [g_cacheLayer]
    }
}

// --- layout script --------------------------------------------------------------------------------

void* g_parseLine;
thread_local std::string t_line;

extern "C" __declspec(naked) void __cdecl CallParseLine(const char* /*line*/, void* /*edx*/, void* /*arg*/) {
    __asm {
        mov ecx, [esp + 4]
        mov edx, [esp + 8]
        push dword ptr [esp + 12]
        call dword ptr [g_parseLine]
        add esp, 4
        ret
    }
}

std::string Number(double v, bool integer) {
    char buf[32];
    if (integer)
        sprintf_s(buf, "%ld", std::lround(v));
    else
        sprintf_s(buf, "%g", v);
    return buf;
}

// The line with its pixel values scaled, or empty if it has none.
std::string ScaleLine(const char* line, double s) {
    const char* p = line;
    if (strncmp(p, "Abs", 3) == 0) p += 3;
    if ((p[0] == 'X' || p[0] == 'Y') && strncmp(p + 1, "Constraint ", 11) == 0) {
        // Third comma-separated value is the offset in pixels.
        const char* c1 = strchr(p, ',');
        const char* c2 = c1 ? strchr(c1 + 1, ',') : nullptr;
        if (!c2) return {};
        char* end;
        double offset = strtod(c2 + 1, &end);
        if (end == c2 + 1 || offset == 0) return {};
        return std::string(line, c2 + 1) + " " + Number(offset * s, false) + end;
    }
    static const char* const kPixelProperties[] = {"fontsize", "fonttrack", "fontleading", "round",
                                                   "virtualfontsize", "underlineoffset"};
    if (strncmp(line, "Property ", 9) != 0) return {};
    const char* name = line + 9;
    for (const char* prop : kPixelProperties) {
        size_t n = strlen(prop);
        if (strncmp(name, prop, n) != 0 || (name[n] != ' ' && name[n] != '\t')) continue;
        char* end;
        double v = strtod(name + n, &end);
        if (end == name + n) return {};
        return std::string(line, name + n) + " " + Number(v * s, true) + end;
    }
    return {};
}

extern "C" void __cdecl ParseLayoutLine(const char* line, void* edx, void* arg) {
    double s = UiScale();
    if (s != 1.0 && line) {
        std::string scaled = ScaleLine(line, s);
        if (!scaled.empty()) {
            t_line = std::move(scaled);
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 8) Log("uiscale: \"%s\" -> \"%s\"", line, t_line.c_str());
            CallParseLine(t_line.c_str(), edx, arg);
            return;
        }
    }
    CallParseLine(line, edx, arg);
}

extern "C" __declspec(naked) void Hook_ParseLayoutLine() {
    __asm {
        push dword ptr [esp + 4]
        push edx
        push ecx
        call ParseLayoutLine
        add esp, 12
        ret
    }
}

void RetargetCall(uintptr_t site, uintptr_t expectedTarget, const void* target, const char* name) {
    auto rel = [site](uintptr_t to) { return static_cast<uint32_t>(to - (site + 5)); };
    if (*reinterpret_cast<const uint8_t*>(site) != 0xE8) {
        Log("patch %s @%08X: not a call, skipped", name, (unsigned)site);
        return;
    }
    PatchDword(site + 1, rel(expectedTarget), rel(reinterpret_cast<uintptr_t>(target)), name);
}

double g_cellPitch, g_clickSlack;
float g_cellOffsetY;

void ScaleFilmstrip(double s) {
    PatchDword(0x41a9bf, 0x28, static_cast<uint32_t>(std::lround(40 * s)), "Filmstrip.thumbBox");
    g_cellPitch = 42.0 * s;
    g_cellOffsetY = static_cast<float>(-16.0 * s);
    g_clickSlack = 8.0 * s;
    PatchDword(0x41b749, 0x74b840, reinterpret_cast<uint32_t>(&g_cellPitch), "Filmstrip.pitch");
    PatchDword(0x41b77a, 0x74b8f8, reinterpret_cast<uint32_t>(&g_cellOffsetY), "Filmstrip.offsetY");
    PatchDword(0x41a3a6, 0x71fe78, reinterpret_cast<uint32_t>(&g_clickSlack), "Filmstrip.clickSlack");
}

}  // namespace

void InstallUiScaleHooks() {
    double s = UiScale();
    if (s == 1.0) return;
    RetargetCall(0x53b509, kCacheLayer, Thunk_CacheLayer, "CacheLayer.solid");
    RetargetCall(0x53b593, kCacheLayer, Thunk_CacheLayer, "CacheLayer.pixels");
    Hook(kParseLayoutLine, &Hook_ParseLayoutLine, &g_parseLine, "ParseLayoutLine");
    ScaleFilmstrip(s);
    PatchByte(0x5085d4, 4, static_cast<uint8_t>(std::lround(4 * s)), "TextBounds.padding");
}
