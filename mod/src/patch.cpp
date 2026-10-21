#include "patch.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

CRITICAL_SECTION g_logLock;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

struct LogInit {
    LogInit() {
        InitializeCriticalSection(&g_logLock);
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (n > 0 && path[n - 1] != L'\\') --n;
        wcscpy_s(path + n, MAX_PATH - n, L"picasa_mod.log");
        g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
} g_logInit;

}  // namespace

void Log(const char* fmt, ...) {
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(buf, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                      GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    n += m < 0 ? 0 : (m > (int)sizeof(buf) - n - 3 ? (int)sizeof(buf) - n - 3 : m);
    buf[n++] = '\r';
    buf[n++] = '\n';
    OutputDebugStringA(buf);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_logLock);
    DWORD written;
    WriteFile(g_logFile, buf, n, &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

bool PatchJump(uintptr_t target, const void* replacement, const uint8_t* expected, size_t expectedLen,
               const char* name) {
    auto* p = reinterpret_cast<uint8_t*>(target);
    if (memcmp(p, expected, expectedLen) != 0) {
        Log("patch %s @%08X: original bytes mismatch, skipped", name, (unsigned)target);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("patch %s @%08X: VirtualProtect failed (%lu)", name, (unsigned)target, GetLastError());
        return false;
    }
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - (target + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("patch %s @%08X -> %p", name, (unsigned)target, replacement);
    return true;
}

bool PatchByte(uintptr_t target, uint8_t expected, uint8_t value, const char* name) {
    auto* p = reinterpret_cast<uint8_t*>(target);
    if (*p != expected) {
        Log("patch %s @%08X: original bytes mismatch, skipped", name, (unsigned)target);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
        Log("patch %s @%08X: VirtualProtect failed (%lu)", name, (unsigned)target, GetLastError());
        return false;
    }
    *p = value;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return true;
}

bool PatchDword(uintptr_t target, uint32_t expected, uint32_t value, const char* name) {
    auto* p = reinterpret_cast<uint8_t*>(target);
    if (memcmp(p, &expected, 4) != 0) {
        Log("patch %s @%08X: original bytes mismatch, skipped", name, (unsigned)target);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("patch %s @%08X: VirtualProtect failed (%lu)", name, (unsigned)target, GetLastError());
        return false;
    }
    memcpy(p, &value, 4);
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    return true;
}
