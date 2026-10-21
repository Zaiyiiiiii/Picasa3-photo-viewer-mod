#pragma once

// The viewer runs DPI aware (per-monitor v2): pictures, videos and text are drawn in physical pixels
// instead of being stretched by Windows. Must be called before the viewer creates any window.
void EnableDpiAwareness();

// Factor the original UI is enlarged by to keep its size on screen (system DPI / 96; 1 when not aware).
double UiScale();

// Scales the UI resources and layout by UiScale() (uiscale.cpp). Call after EnableDpiAwareness().
void InstallUiScaleHooks();
