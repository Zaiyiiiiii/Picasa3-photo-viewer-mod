// Proxy for VERSION.dll: PicasaPhotoViewer.exe imports it and it is not a KnownDLL, so a copy next to
// the exe is loaded instead of the system one. Every export jumps to the real system DLL.

#include <windows.h>

#include "patch.h"

#define VERSION_EXPORTS(X)            \
    X(0, GetFileVersionInfoA)         \
    X(1, GetFileVersionInfoByHandle)  \
    X(2, GetFileVersionInfoExA)       \
    X(3, GetFileVersionInfoExW)       \
    X(4, GetFileVersionInfoSizeA)     \
    X(5, GetFileVersionInfoSizeExA)   \
    X(6, GetFileVersionInfoSizeExW)   \
    X(7, GetFileVersionInfoSizeW)     \
    X(8, GetFileVersionInfoW)         \
    X(9, VerFindFileA)                \
    X(10, VerFindFileW)               \
    X(11, VerInstallFileA)            \
    X(12, VerInstallFileW)            \
    X(13, VerQueryValueA)             \
    X(14, VerQueryValueW)

static FARPROC g_real[15];

#define DEFINE_STUB(i, name) \
    extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_real + i * 4] } }
VERSION_EXPORTS(DEFINE_STUB)

bool LoadRealVersionDll() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 via WOW64 redirection
    if (n == 0 || n > MAX_PATH - 16) return false;
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) {
        Log("cannot load real version.dll (%lu)", GetLastError());
        return false;
    }
#define RESOLVE(i, name) g_real[i] = GetProcAddress(real, #name);
    VERSION_EXPORTS(RESOLVE)
    return true;
}
