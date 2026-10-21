// No network at all. The viewer still carries Picasa's upload, sign-in, crash-report, usage-stats and
// update code; it reaches the network through WinINet, urlmon's URLDownloadToFile, WinHTTP (loaded on
// demand) and gethostbyname. Sessions fail to open and the connection state reads "offline", so the
// callers give up instead of retrying.

#include <winsock2.h>
#include <windows.h>
#include <urlmon.h>
#include <wininet.h>

#include "hooks.h"
#include "patch.h"

namespace {

void Note(const char* api) {
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 20) Log("network: blocked %s", api);
}

using InternetOpenAFn = HINTERNET(WINAPI*)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
InternetOpenAFn g_internetOpenA;
HINTERNET WINAPI Hook_InternetOpenA(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD) {
    Note("InternetOpenA");
    SetLastError(ERROR_INTERNET_DISCONNECTED);
    return nullptr;
}

using InternetGetConnectedStateFn = BOOL(WINAPI*)(LPDWORD, DWORD);
InternetGetConnectedStateFn g_getConnectedState;
BOOL WINAPI Hook_InternetGetConnectedState(LPDWORD flags, DWORD) {
    if (flags) *flags = INTERNET_CONNECTION_OFFLINE;
    return FALSE;
}

using InternetGetConnectedStateExFn = BOOL(WINAPI*)(LPDWORD, LPSTR, DWORD, DWORD);
InternetGetConnectedStateExFn g_getConnectedStateEx;
BOOL WINAPI Hook_InternetGetConnectedStateEx(LPDWORD flags, LPSTR name, DWORD nameLen, DWORD) {
    if (flags) *flags = INTERNET_CONNECTION_OFFLINE;
    if (name && nameLen) name[0] = 0;
    return FALSE;
}

using InternetGoOnlineFn = BOOL(WINAPI*)(LPSTR, HWND, DWORD);
InternetGoOnlineFn g_goOnline;
BOOL WINAPI Hook_InternetGoOnline(LPSTR, HWND, DWORD) {
    Note("InternetGoOnline");
    return FALSE;
}

using URLDownloadToFileAFn = HRESULT(WINAPI*)(LPUNKNOWN, LPCSTR, LPCSTR, DWORD, LPBINDSTATUSCALLBACK);
using URLDownloadToFileWFn = HRESULT(WINAPI*)(LPUNKNOWN, LPCWSTR, LPCWSTR, DWORD, LPBINDSTATUSCALLBACK);
URLDownloadToFileAFn g_downloadA;
URLDownloadToFileWFn g_downloadW;
HRESULT WINAPI Hook_URLDownloadToFileA(LPUNKNOWN, LPCSTR, LPCSTR, DWORD, LPBINDSTATUSCALLBACK) {
    Note("URLDownloadToFileA");
    return INET_E_CONNECTION_TIMEOUT;
}
HRESULT WINAPI Hook_URLDownloadToFileW(LPUNKNOWN, LPCWSTR, LPCWSTR, DWORD, LPBINDSTATUSCALLBACK) {
    Note("URLDownloadToFileW");
    return INET_E_CONNECTION_TIMEOUT;
}

using WinHttpOpenFn = void*(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
WinHttpOpenFn g_winHttpOpen;
void* WINAPI Hook_WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) {
    Note("WinHttpOpen");
    SetLastError(ERROR_INTERNET_DISCONNECTED);
    return nullptr;
}

using GetHostByNameFn = hostent*(WSAAPI*)(const char*);
GetHostByNameFn g_getHostByName;
hostent* WSAAPI Hook_gethostbyname(const char*) {
    Note("gethostbyname");
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}

void HookExport(const wchar_t* dll, const char* name, void* detour, void** original) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);  // loaded on demand by the viewer: load it now so it is covered
    if (FARPROC fn = m ? GetProcAddress(m, name) : nullptr)
        Hook(reinterpret_cast<uintptr_t>(fn), detour, original, name);
    else
        Log("network: %S!%s not found", dll, name);
}

}  // namespace

void BlockNetwork() {
    HookExport(L"wininet.dll", "InternetOpenA", &Hook_InternetOpenA, reinterpret_cast<void**>(&g_internetOpenA));
    HookExport(L"wininet.dll", "InternetGetConnectedState", &Hook_InternetGetConnectedState,
               reinterpret_cast<void**>(&g_getConnectedState));
    HookExport(L"wininet.dll", "InternetGetConnectedStateEx", &Hook_InternetGetConnectedStateEx,
               reinterpret_cast<void**>(&g_getConnectedStateEx));
    HookExport(L"wininet.dll", "InternetGoOnline", &Hook_InternetGoOnline, reinterpret_cast<void**>(&g_goOnline));
    HookExport(L"urlmon.dll", "URLDownloadToFileA", &Hook_URLDownloadToFileA, reinterpret_cast<void**>(&g_downloadA));
    HookExport(L"urlmon.dll", "URLDownloadToFileW", &Hook_URLDownloadToFileW, reinterpret_cast<void**>(&g_downloadW));
    HookExport(L"winhttp.dll", "WinHttpOpen", &Hook_WinHttpOpen, reinterpret_cast<void**>(&g_winHttpOpen));
    HookExport(L"ws2_32.dll", "gethostbyname", &Hook_gethostbyname, reinterpret_cast<void**>(&g_getHostByName));
}
