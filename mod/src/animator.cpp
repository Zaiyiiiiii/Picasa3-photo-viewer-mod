// Plays animated images inside the original viewer.
//
// How a picture reaches the screen (see analysis/):
//   DecodeImageInto (decoder.cpp) fills a full-size ytBitmap, then
//   0x527400 ResampleToTarget(ctx, size, callback, ytBitmap* out)            stdcall (ret 0x10)
//     scales it to the display size. The main ytBitmapNode shares that ytBitmap at node+0x28c, and
//   0x4f0570 ytBitmapNode::Draw(ctx)                                         thiscall, ret 4
//     reads its pixels on every paint, driven by
//   0x50e230 ytDrawUI::Render(ui, flush, maySkip, fadeSeconds, checkVisible, l, t, r, b)   stdcall
//     which repaints a rect (-1 = whole client area). fadeSeconds != 0 blocks in a cross-fade loop.
// ytBitmap: +4 stride (pixels), +8 width, +0xc height, +0x10 pixels (32-bit BGRA).
//
// So an animation is remembered by the pixel buffer ResampleToTarget produced; later frames are written
// into that buffer from inside Draw (the node is only touched while it is being drawn), and a UI-thread
// timer asks for repaints. A sampled fingerprint of what the buffer should contain guards against the
// buffer being freed and its address reused for another picture.

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "animator.h"
#include "codecs/codec.h"
#include "hooks.h"
#include "patch.h"

namespace {

constexpr uintptr_t kResampleToTarget = 0x527400;
constexpr uintptr_t kBitmapNodeDraw = 0x4f0570;
constexpr uintptr_t kRender = 0x50e230;
constexpr size_t kNodeBitmap = 0x28c;

constexpr size_t kMaxAnimations = 32;
constexpr DWORD kVisibleTimeoutMs = 500;
constexpr UINT kTimerMs = 10;
constexpr int kMinFrameMs = 16;

struct BitmapView {
    uint32_t stride, width, height;
    uint32_t* pixels;
};

BitmapView ViewOf(const uint8_t* ytBitmap) {
    auto field = [&](size_t off) { return *reinterpret_cast<const uint32_t*>(ytBitmap + off); };
    return {field(4), field(8), field(0xc), reinterpret_cast<uint32_t*>(field(0x10))};
}

uint64_t Fingerprint(const BitmapView& b) {
    size_t n = size_t(b.width) * b.height;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; n && i < 61; ++i) {
        size_t k = n * i / 61;
        h = (h ^ b.pixels[(k / b.width) * b.stride + k % b.width]) * 1099511628211ull;
    }
    return h;
}

// A coarse grid of the picture at normalized positions, to recognise it after the viewer rotates or
// rescales it into a new buffer.
constexpr int kGrid = 16;
using Snapshot = std::array<uint32_t, kGrid * kGrid>;

uint32_t SampleAt(const BitmapView& b, double u, double v) {
    uint32_t x = std::min(b.width - 1, static_cast<uint32_t>(u * b.width));
    uint32_t y = std::min(b.height - 1, static_cast<uint32_t>(v * b.height));
    return b.pixels[size_t(y) * b.stride + x];
}

Snapshot TakeSnapshot(const BitmapView& b) {
    Snapshot s;
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) s[j * kGrid + i] = SampleAt(b, (i + 0.5) / kGrid, (j + 0.5) / kGrid);
    return s;
}

// Where the normalized point (u, v) lands after `turns` clockwise quarter turns.
void Turn(int turns, double& u, double& v) {
    double t;
    switch (turns & 3) {
        case 1: t = u; u = 1 - v; v = t; break;
        case 2: u = 1 - u; v = 1 - v; break;
        case 3: t = u; u = v; v = 1 - t; break;
    }
}

int ColorDistance(uint32_t a, uint32_t b) {
    int d = 0;
    for (int shift = 0; shift < 24; shift += 8) d += std::abs(int((a >> shift) & 255) - int((b >> shift) & 255));
    return d;
}

// Clockwise quarter turns that map `last` (a picture of lastW x lastH) onto `view`, or -1 if `view`
// shows something else.
int MatchTurns(const Snapshot& last, uint32_t lastW, uint32_t lastH, const BitmapView& view) {
    int best = -1;
    double bestErr = 24;  // mean per-pixel distance over B, G, R
    for (int k = 0; k < 4; ++k) {
        double aspect = (k & 1) ? double(lastH) / lastW : double(lastW) / lastH;
        if (std::abs(std::log(aspect * view.height / view.width)) > 0.05) continue;
        long total = 0;
        for (int j = 0; j < kGrid; ++j)
            for (int i = 0; i < kGrid; ++i) {
                double u = (i + 0.5) / kGrid, v = (j + 0.5) / kGrid;
                Turn(k, u, v);
                total += ColorDistance(last[j * kGrid + i], SampleAt(view, u, v));
            }
        double err = double(total) / (kGrid * kGrid);
        if (err < bestErr) {
            bestErr = err;
            best = k;
        }
    }
    return best;
}

// Packed lerp of two BGRA pixels, w in [0, 256].
inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t rb = (((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    uint32_t ag = (((a >> 8) & 0xFF00FF) * (256 - w) + ((b >> 8) & 0xFF00FF) * w) & 0xFF00FF00;
    return rb | ag;
}

// Bilinear scale of a tightly packed sw x sh BGRA frame into `dst`.
void ScaleInto(const uint32_t* src, uint32_t sw, uint32_t sh, const BitmapView& dst) {
    if (sw == dst.width && sh == dst.height) {
        for (uint32_t y = 0; y < sh; ++y) memcpy(dst.pixels + size_t(y) * dst.stride, src + size_t(y) * sw, sw * 4);
        return;
    }
    auto coord = [](uint32_t i, uint32_t srcLen, uint32_t dstLen, uint32_t& i0, uint32_t& i1, uint32_t& w) {
        int64_t f = (int64_t(2 * i + 1) * srcLen * 256) / (2 * dstLen) - 128;  // 24.8 sample centre
        if (f < 0) f = 0;
        i0 = uint32_t(f >> 8);
        if (i0 >= srcLen - 1) {
            i0 = i1 = srcLen - 1;
            w = 0;
        } else {
            i1 = i0 + 1;
            w = uint32_t(f & 255);
        }
    };
    std::vector<uint32_t> x0(dst.width), x1(dst.width), wx(dst.width);
    for (uint32_t x = 0; x < dst.width; ++x) coord(x, sw, dst.width, x0[x], x1[x], wx[x]);
    for (uint32_t y = 0; y < dst.height; ++y) {
        uint32_t y0, y1, wy;
        coord(y, sh, dst.height, y0, y1, wy);
        const uint32_t* r0 = src + size_t(y0) * sw;
        const uint32_t* r1 = src + size_t(y1) * sw;
        uint32_t* out = dst.pixels + size_t(y) * dst.stride;
        for (uint32_t x = 0; x < dst.width; ++x)
            out[x] = Lerp(Lerp(r0[x0[x]], r0[x1[x]], wx[x]), Lerp(r1[x0[x]], r1[x1[x]], wx[x]), wy);
    }
}

struct Animation {
    std::vector<uint8_t> data;              // encoded file, owned
    std::unique_ptr<ImageSource> source;    // opened on first draw
    std::vector<uint32_t> frame;            // decoded frame at source size
    uint32_t* pixels = nullptr;             // display buffer this animation is bound to
    uint32_t width = 0, height = 0;
    uint64_t fingerprint = 0;
    DWORD due = 0;
    DWORD lastDrawn = 0;
    void* ui = nullptr;
    uint8_t* node = nullptr;  // last ytBitmapNode seen drawing this buffer
    bool awaitingDraw = false;  // a repaint was requested for it and it has not been drawn yet
    int frames = 0;
    Snapshot snapshot{};        // what the buffer showed after our last write
    int turns = 0;              // clockwise quarter turns the viewer applied (its rotate buttons)
    std::vector<uint32_t> unrotated;  // frame at display size before applying `turns`

    // Record what `view` now shows, so the buffer can be recognised later.
    void Remember(const BitmapView& view) {
        pixels = view.pixels;
        width = view.width;
        height = view.height;
        fingerprint = Fingerprint(view);
        snapshot = TakeSnapshot(view);
    }
};

// Average of four BGRA pixels, per channel.
inline uint32_t Average4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t rb = (((a & 0xFF00FF) + (b & 0xFF00FF) + (c & 0xFF00FF) + (d & 0xFF00FF) + 0x020002) >> 2) & 0xFF00FF;
    uint32_t ag = ((((a >> 8) & 0xFF00FF) + ((b >> 8) & 0xFF00FF) + ((c >> 8) & 0xFF00FF) + ((d >> 8) & 0xFF00FF) +
                    0x020002) << 6) & 0xFF00FF00;
    return rb | ag;
}

// ytBitmapNode draws scaled-down pictures from a mip pyramid of its bitmap: node+0x2b4 points to levels
// 1..n back to back (each level ceil(half) of the previous in both directions, tightly packed BGRA) and
// node+0x2c0 holds the end offset of each level. It is built once, so after writing a frame it must be
// rebuilt, or the picture freezes whenever it is shown smaller than its bitmap (e.g. rotated to portrait).
void UpdateMipmaps(const uint8_t* node, const BitmapView& view) {
    auto* base = *reinterpret_cast<uint8_t* const*>(node + 0x2b4);
    if (!base) return;
    const auto* ends = reinterpret_cast<const uint32_t*>(node + 0x2c0);
    const uint32_t* src = view.pixels;
    uint32_t srcStride = view.stride, w = view.width, h = view.height, offset = 0;
    for (int level = 0; level < 16 && (w > 1 || h > 1); ++level) {
        uint32_t nw = (w + 1) / 2, nh = (h + 1) / 2;
        uint32_t end = offset + nw * nh * 4;
        if (ends[level] != end) break;  // not the layout we know: leave it alone
        auto* dst = reinterpret_cast<uint32_t*>(base + offset);
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t* r0 = src + size_t(2 * y) * srcStride;
            const uint32_t* r1 = src + size_t(std::min(2 * y + 1, h - 1)) * srcStride;
            for (uint32_t x = 0; x < nw; ++x) {
                uint32_t x0 = 2 * x, x1 = std::min(2 * x + 1, w - 1);
                dst[size_t(y) * nw + x] = Average4(r0[x0], r0[x1], r1[x0], r1[x1]);
            }
        }
        src = dst;
        srcStride = w = nw;
        h = nh;
        offset = end;
        static bool logged;
        if (level >= 2 && !logged) {
            logged = true;
            Log("anim: mip pyramid of %ux%u refreshed (layout matches)", view.width, view.height);
        }
    }
}

// Writes a frame into `view` through the viewer's rotation: `draw` renders the unrotated picture into
// the given (pixels, width, height, stride in bytes) target.
template <typename DrawFn>
bool WriteFrame(Animation& a, const BitmapView& view, DrawFn draw) {
    if (a.turns == 0) return draw(reinterpret_cast<uint8_t*>(view.pixels), view.width, view.height, view.stride * 4);
    uint32_t w = (a.turns & 1) ? view.height : view.width, h = (a.turns & 1) ? view.width : view.height;
    a.unrotated.resize(size_t(w) * h);
    auto* tmp = reinterpret_cast<uint8_t*>(a.unrotated.data());
    if (!draw(tmp, w, h, w * 4)) return false;
    codec::Rotate(tmp, w * 4, w, h, a.turns, reinterpret_cast<uint8_t*>(view.pixels), view.stride * 4);
    return true;
}

// The node now shows another buffer: if it is our picture rotated (or rescaled) by the viewer, move
// the animation onto it and keep playing.
bool Follow(Animation& a, const BitmapView& view) {
    int k = MatchTurns(a.snapshot, a.width, a.height, view);
    if (k < 0) return false;
    a.turns = (a.turns + k) & 3;
    Log("anim: %p follows to %p (%ux%u, +%d turns, now %d)", a.pixels, view.pixels, view.width, view.height, k, a.turns);
    a.Remember(view);
    return true;
}

std::mutex g_mutex;
std::vector<std::unique_ptr<Animation>> g_animations;  // oldest first
thread_local std::unique_ptr<Animation> t_pending;
thread_local void* t_renderingUi;
UINT_PTR g_timer;

using ResampleFn = int(__stdcall*)(int*, int*, int*, uint8_t*);
using DrawFn = int(__fastcall*)(uint8_t*, void*, int*);
using RenderFn = float(__stdcall*)(void*, char, char, float, char, int, int, int, int);
ResampleFn g_resample;
DrawFn g_draw;
RenderFn g_render;

void Drop(std::vector<std::unique_ptr<Animation>>::iterator it, const char* why) {
    Log("anim: drop %p (%s) after %d frames", (*it)->pixels, why, (*it)->frames);
    g_animations.erase(it);
}

constexpr uintptr_t kBitmapNodeVtable = 0x6ed38c;

// The bitmap `node` shows, if it is still a live ytBitmapNode.
bool NodeView(uint8_t* node, BitmapView* view) {
    __try {
        if (*reinterpret_cast<uintptr_t*>(node) != kBitmapNodeVtable) return false;
        *view = ViewOf(node + kNodeBitmap);
        return view->pixels && view->width && view->height;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True if `node` is still a ytBitmapNode showing `pixels`; marks it dirty when `dirty` is set.
bool ShowsBuffer(uint8_t* node, const uint32_t* pixels, bool dirty) {
    __try {
        if (*reinterpret_cast<uintptr_t*>(node) != kBitmapNodeVtable) return false;
        if (ViewOf(node + kNodeBitmap).pixels != pixels) return false;
        if (dirty) *reinterpret_cast<uint32_t*>(node + 8) |= 7;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Stops a live source (and its audio) right away; it restarts if the picture is drawn again.
void Stop(Animation& a, const char* why) {
    Log("anim: stop %p (%s) after %d frames", a.pixels, why, a.frames);
    a.source.reset();
    a.awaitingDraw = false;
}

void CALLBACK OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    void* ui = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DWORD now = GetTickCount();
        for (auto& a : g_animations) {
            if (!a->source) continue;
            bool live = a->source->Live();
            BitmapView shown;
            if (live && !ShowsBuffer(a->node, a->pixels, false) &&
                !(NodeView(a->node, &shown) && Follow(*a, shown))) {  // the viewer moved to another picture
                Stop(*a, "replaced");
                continue;
            }
            if (!live && now - a->lastDrawn >= kVisibleTimeoutMs) continue;
            bool due = live ? a->source->FrameReady() : int(now - a->due) >= 0;
            if (due && a->ui && ShowsBuffer(a->node, a->pixels, true)) {
                ui = a->ui;
                a->awaitingDraw = live;
            }
        }
    }
    // Through the patched entry, so Hook_Render records the UI for the nodes drawn.
    if (!ui) return;
    reinterpret_cast<RenderFn>(kRender)(ui, 1, 0, 0.0f, 1, -1, -1, -1, -1);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& a : g_animations)
        if (a->awaitingDraw && a->source) Stop(*a, "not drawn");  // no longer part of the scene
}

// The viewer swaps the display buffer once the full-size picture has loaded: if the same file is already
// playing on another buffer, continue it here instead of starting over (no restart, no audio gap).
bool TakeOver(Animation& a) {
    for (auto& b : g_animations) {
        if (b.get() == &a || !b->source || b->data != a.data) continue;
        // The source reads b's copy of the file: that buffer moves along with it (the contents are equal),
        // or it would be freed with b while still being decoded.
        a.data.swap(b->data);
        a.source = std::move(b->source);
        a.frame = std::move(b->frame);
        a.due = b->due;
        a.frames = b->frames;
        b->awaitingDraw = false;
        Log("anim: %p continues on %p at frame %d", b->pixels, a.pixels, a.frames);
        return true;
    }
    return false;
}

// Called with g_mutex held, on the UI thread, while `node` is being drawn.
void Advance(Animation& a, const uint8_t* node, const BitmapView& view, DWORD now) {
    if (!a.source && TakeOver(a)) return;
    if (!a.source) {
        a.source = OpenImageSource(a.data.data(), a.data.size());
        if (!a.source) return;
        if (!g_timer) g_timer = SetTimer(nullptr, 0, kTimerMs, OnTimer);
        if (a.source->Live()) {
            if (!a.source->StartLive()) Log("anim: cannot start %s", a.source->Name());
            a.frames = 0;
            Log("anim: live %p (%ux%u %s) on %ux%u", a.pixels, a.source->width, a.source->height, a.source->Name(),
                view.width, view.height);
            return;
        }
        a.frame.resize(size_t(a.source->width) * a.source->height);
        int d = a.source->NextFrame(reinterpret_cast<uint8_t*>(a.frame.data()), a.source->width * 4);  // frame 0 is on screen
        a.due = now + (d < kMinFrameMs ? kMinFrameMs : d);
        a.frames = 1;
        Log("anim: playing %p (%ux%u %s) on %ux%u, ui %p", a.pixels, a.source->width, a.source->height,
            a.source->Name(), view.width, view.height, a.ui);
        return;
    }
    if (a.source->Live()) {
        if (a.source->FrameReady() &&
            WriteFrame(a, view, [&](uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride) {
                return a.source->RenderLive(dst, w, h, stride);
            })) {
            UpdateMipmaps(node, view);
            a.Remember(view);
            if (++a.frames % 100 == 0) Log("anim: %p at video frame %d", a.pixels, a.frames);
        }
        return;
    }
    if (int(now - a.due) < 0) return;
    uint8_t* out = reinterpret_cast<uint8_t*>(a.frame.data());
    int d = a.source->NextFrame(out, a.source->width * 4);
    if (d < 0 && a.source->Rewind()) d = a.source->NextFrame(out, a.source->width * 4);
    if (d < 0) return;
    WriteFrame(a, view, [&](uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride) {
        ScaleInto(a.frame.data(), a.source->width, a.source->height,
                  {stride / 4, w, h, reinterpret_cast<uint32_t*>(dst)});
        return true;
    });
    UpdateMipmaps(node, view);
    a.Remember(view);
    a.due = (now - a.due > 1000 ? now : a.due) + (d < kMinFrameMs ? kMinFrameMs : d);  // keep cadence
    if (++a.frames % 30 == 0) Log("anim: %p at frame %d, %lu ms behind", a.pixels, a.frames, GetTickCount() - now);
}

int __fastcall Hook_Draw(uint8_t* node, void* edx, int* ctx) {
    BitmapView view = ViewOf(node + kNodeBitmap);
    if (view.pixels && view.width && view.height) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = std::find_if(g_animations.begin(), g_animations.end(),
                               [&](const auto& a) { return a->pixels == view.pixels; });
        if (it == g_animations.end())  // the node we were playing on may show our picture rotated
            it = std::find_if(g_animations.begin(), g_animations.end(),
                              [&](const auto& a) { return a->node == node && a->source && Follow(*a, view); });
        if (it != g_animations.end()) {
            Animation& a = **it;
            if (a.width != view.width || a.height != view.height || Fingerprint(view) != a.fingerprint) {
                Drop(it, "buffer reused");
            } else {
                DWORD now = GetTickCount();
                a.lastDrawn = now;
                a.ui = t_renderingUi;
                a.node = node;
                a.awaitingDraw = false;
                Advance(a, node, view, now);
            }
        }
    }
    return g_draw(node, edx, ctx);  // outside the lock: it draws child nodes, which come back here
}

int __stdcall Hook_Resample(int* ctx, int* size, int* callback, uint8_t* out) {
    int result = g_resample(ctx, size, callback, out);
    std::unique_ptr<Animation> a = std::move(t_pending);
    if (result != 0 || !a || !out) return result;
    BitmapView view = ViewOf(out);
    if (!view.pixels || !view.width || !view.height) return result;
    a->Remember(view);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_animations.begin(); it != g_animations.end(); ++it)
        if ((*it)->pixels == view.pixels) {
            Drop(it, "rebound");
            break;
        }
    Log("anim: bound %u bytes to %ux%u buffer %p", (unsigned)a->data.size(), view.width, view.height, view.pixels);
    g_animations.push_back(std::move(a));
    if (g_animations.size() > kMaxAnimations) {
        // Folders full of videos bind one entry per thumbnail: never evict what is playing; prefer the
        // oldest entry that has not been on screen recently.
        DWORD now = GetTickCount();
        auto idle = [&](const auto& e) { return !e->source && now - e->lastDrawn > 2000; };
        auto victim = std::find_if(g_animations.begin(), g_animations.end(), idle);
        if (victim == g_animations.end())
            victim = std::find_if(g_animations.begin(), g_animations.end(), [](const auto& e) { return !e->source; });
        if (victim != g_animations.end()) Drop(victim, "evicted");
    }
    return result;
}

float __stdcall Hook_Render(void* ui, char flush, char maySkip, float fade, char checkVisible, int l, int t, int r,
                            int b) {
    void* outer = t_renderingUi;
    t_renderingUi = ui;
    float result = g_render(ui, flush, maySkip, fade, checkVisible, l, t, r, b);
    t_renderingUi = outer;
    return result;
}

}  // namespace

void AnimNoteDecoded(const uint8_t* data, size_t size) {
    auto a = std::make_unique<Animation>();
    a->data.assign(data, data + size);
    t_pending = std::move(a);
}

void InstallAnimatorHooks() {
    Hook(kResampleToTarget, &Hook_Resample, reinterpret_cast<void**>(&g_resample), "ResampleToTarget");
    Hook(kBitmapNodeDraw, &Hook_Draw, reinterpret_cast<void**>(&g_draw), "ytBitmapNode::Draw");
    Hook(kRender, &Hook_Render, reinterpret_cast<void**>(&g_render), "ytDrawUI::Render");
}
