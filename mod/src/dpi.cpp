// High-DPI display.
//
// The viewer has no DPI support (no manifest entry; its only GetDeviceCaps(LOGPIXELS*) calls are in OLE
// picture loading and the ActiveX host). Left DPI unaware, Windows makes it lay out at 2/3 of the screen
// on a 150% display and stretches the whole window, so pictures are downscaled to that size first
// (ResampleToTarget fits them to the screen it is told about) and then blurred up again.
//
// Declared DPI aware, it sees the physical screen and pictures are shown pixel for pixel. Its UI is
// pixel based, so it is enlarged by UiScale() where its sizes come from (see uiscale.cpp).
//
// PICASA_MOD_DPI=0 keeps the original behaviour.

#include "dpi.h"

#include <windows.h>

#include <cstdlib>

#include "patch.h"

namespace {

double g_scale = 1.0;

}  // namespace

void EnableDpiAwareness() {
    char value[8];
    size_t len = 0;
    if (getenv_s(&len, value, "PICASA_MOD_DPI") == 0 && len && value[0] == '0') {
        Log("dpi: left unaware (PICASA_MOD_DPI=0)");
        return;
    }
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)) {
        Log("dpi: cannot become DPI aware (%lu)", GetLastError());
        return;
    }
    g_scale = GetDpiForSystem() / 96.0;
    Log("dpi: aware, system %u dpi, UI scale %.2f", GetDpiForSystem(), g_scale);
}

double UiScale() { return g_scale; }
