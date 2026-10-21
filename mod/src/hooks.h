#pragma once

#include <cstdint>

// Detour `target` through MinHook. `original` receives the trampoline. The hook takes effect at the
// next ApplyHooks(); returns false (and logs) if it could not be created.
bool Hook(uintptr_t target, void* detour, void** original, const char* name);

// Enable every hook created so far, in one pass.
void ApplyHooks();
