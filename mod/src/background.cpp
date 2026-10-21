// Desktop capture for the "transparent background" fullscreen mode.
//
// 0x4288d0 moves the viewer off screen and copies the desktop into the background node with
//   BitBlt(memDC, 0, 0, w, h, GetDC(GetDesktopWindow()), left, top, SRCCOPY)        (via 0x4cdaa0)
// in the coordinates of a DPI-unaware process (e.g. 2560x1440 on a 3840x2160 screen at 150%), but the
// desktop DC hands out physical pixels, so only the top-left part of the screen was captured and then
// shown enlarged. The copy from the desktop is redone here from a DPI-aware screen DC, scaling the whole
// physical area down to the size the viewer asked for.

#include <windows.h>

#include "hooks.h"
#include "patch.h"

namespace {

using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
BitBltFn g_bitBlt;

BOOL WINAPI Hook_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    if (rop != SRCCOPY || !src || WindowFromDC(src) != GetDesktopWindow() ||
        GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_UNAWARE)
        return g_bitBlt(dst, x, y, w, h, src, sx, sy, rop);  // already in physical pixels (see dpi.cpp)
    // A DPI-unaware thread is told 96 dpi; ask as a DPI-aware one.
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    UINT dpi = GetDpiForSystem();
    HDC screen = dpi != 96 ? GetDC(nullptr) : nullptr;
    BOOL ok = FALSE;
    if (screen) {
        int prev = SetStretchBltMode(dst, HALFTONE);
        SetBrushOrgEx(dst, 0, 0, nullptr);
        ok = StretchBlt(dst, x, y, w, h, screen, MulDiv(sx, dpi, 96), MulDiv(sy, dpi, 96), MulDiv(w, dpi, 96),
                        MulDiv(h, dpi, 96), SRCCOPY);
        SetStretchBltMode(dst, prev);
        ReleaseDC(nullptr, screen);
    }
    SetThreadDpiAwarenessContext(old);
    if (dpi == 96) return g_bitBlt(dst, x, y, w, h, src, sx, sy, rop);
    Log("background: desktop %dx%d at %d,%d captured at %u dpi: %s", w, h, sx, sy, dpi, ok ? "ok" : "failed");
    return ok ? ok : g_bitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

}  // namespace

void InstallBackgroundHooks() {
    if (auto* fn = GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "BitBlt"))
        Hook(reinterpret_cast<uintptr_t>(fn), &Hook_BitBlt, reinterpret_cast<void**>(&g_bitBlt), "BitBlt");
}
