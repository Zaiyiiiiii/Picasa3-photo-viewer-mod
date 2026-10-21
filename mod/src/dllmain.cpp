#include <windows.h>

#include "animator.h"
#include "dpi.h"
#include "filetypes.h"
#include "hooks.h"
#include "patch.h"

bool LoadRealVersionDll();
void InstallDecoderHooks();
void InstallTracePoints();
void RemovePicasaFeatures();
void InstallBackgroundHooks();
void BlockNetwork();
void InstallSettingsDialog();

namespace {

// PicasaPhotoViewer.exe 3.9.0 build: link timestamp from the PE header.
constexpr DWORD kViewerTimestamp = 0x51441159;

// First-chance log of faults, so crashes inside hooks show up in picasa_mod.log
// (the viewer's own CrashReporter replaces the unhandled-exception filter).
LONG CALLBACK LogFault(EXCEPTION_POINTERS* e) {
    static LONG budget = 20;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) &&
        InterlockedDecrement(&budget) >= 0) {
        const CONTEXT* c = e->ContextRecord;
        Log("FAULT %08lX at %08lX (access %08lX) eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX "
            "ebp=%08lX esp=%08lX [esp]=%08lX",
            code, c->Eip, e->ExceptionRecord->NumberParameters > 1 ? (DWORD)e->ExceptionRecord->ExceptionInformation[1] : 0,
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, *reinterpret_cast<DWORD*>(c->Esp));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool IsSupportedViewer() {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return reinterpret_cast<uintptr_t>(base) == 0x400000 && nt->FileHeader.TimeDateStamp == kViewerTimestamp;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    if (!LoadRealVersionDll()) return FALSE;
    if (!IsSupportedViewer()) {
        Log("host is not PicasaPhotoViewer 3.9.0, hooks not installed");
        return TRUE;
    }
    Log("picasa_mod loaded at %p", module);
    AddVectoredExceptionHandler(1, LogFault);
    EnableDpiAwareness();
    InstallFileTypeHooks();
    InstallDecoderHooks();
    InstallAnimatorHooks();
    RemovePicasaFeatures();
    InstallBackgroundHooks();
    BlockNetwork();
    InstallSettingsDialog();
    InstallUiScaleHooks();
    ApplyHooks();
    InstallTracePoints();
    return TRUE;
}
