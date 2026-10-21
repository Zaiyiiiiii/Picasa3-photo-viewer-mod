#include "hooks.h"

#include <MinHook.h>

#include "patch.h"

// Hooks are created right away but enabled together by ApplyHooks(): every enable suspends and resumes
// all threads, which cost ~35 ms per hook at startup.
bool Hook(uintptr_t target, void* detour, void** original, const char* name) {
    static MH_STATUS init = MH_Initialize();
    MH_STATUS s = init == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : init;
    if (s == MH_OK) s = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (s == MH_OK) s = MH_QueueEnableHook(reinterpret_cast<void*>(target));
    if (s != MH_OK) Log("hook %s @%08X: %s", name, (unsigned)target, MH_StatusToString(s));
    return s == MH_OK;
}

void ApplyHooks() {
    MH_STATUS s = MH_ApplyQueued();
    Log("hooks applied: %s", MH_StatusToString(s));
}
