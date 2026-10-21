// Removes everything that depends on the Picasa library, the Picasa application or its web services.
//
//   0x420860  SendToPicasa(path, verb, ...)       cdecl: finds or launches Picasa3.exe "/nosplash verb path";
//             behind Edit/Locate in Picasa, Print, Email and Blog This.
//   0x42d650  QuickUpload()                       EAX = Slingshot; Picasa Web Albums upload.
//   0x43d980  SetStar(index, starred)             EDI = image cache; stars are kept in .picasa.ini.
//   0x42aae0  builds the "More" popup list from {separator "via Picasa", print, email, blogthis, config,
//             help}; the first four only work with Picasa installed. The loop counter is set at 0x42ad29.
//   Context menu (Win32, TrackPopupMenu): ID_FILE_EDIT 0x9c41, ID_FILE_PRINT_PHOTO 0x9c42,
//             ID_FILE_EMAIL 0x9c43, ID_FILE_BLOG 0x9c44, ID_QUICKUPLOAD 0x9de1.
// The toolbar buttons (ss/launch_picasa, ss/to_web, ss/star) are hidden in the resource pack instead:
// opens of runtime\slingshot\respack.yt get a patched copy (respack.cpp).
//
// The "More" button becomes the settings button: the resource pack turns it into an icon button, and
// the click dispatch of 0x424a90, which already opens the settings for a button named "ss/config"
// (rep cmpsb against it at 0x4256ea, length 10 at 0x4256ef; cmp at 0x425707), checks for "ss/more"
// (0x7212a0, length 8) instead. That test comes before the one that pops up the list.
//
// Rotation is view state, like zoom and pan: 0x43cfa0 RotateImage first records rotate(n) through
//   0x438880  RecordRotation(...)    thiscall, ret 4; non-zero aborts the rotation
// and then rotates the cached bitmaps in memory. RecordRotation now does nothing and succeeds.
// The viewer does not use .picasa.ini at all: every open of *picasa.ini fails as "not found".

#include <windows.h>

#include "hooks.h"
#include "patch.h"
#include "respack.h"

namespace {

constexpr UINT kPicasaMenuIds[] = {0x9c41, 0x9c42, 0x9c43, 0x9c44, 0x9de1};

extern "C" __declspec(naked) void ReturnZero() {
    __asm {
        xor eax, eax
        ret
    }
}

extern "C" __declspec(naked) void ReturnZeroPop4() {
    __asm {
        xor eax, eax
        ret 4
    }
}

// Start the "More" list loop at entry 4 (config), skipping the Picasa entries.
extern "C" __declspec(naked) void MoreListFromConfig() {
    __asm {
        mov eax, 4
        mov dword ptr [esp + 20h], eax
        push 42ad2fh
        ret
    }
}

using TrackPopupMenuFn = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
TrackPopupMenuFn g_trackPopupMenu;

// Remove separators that ended up first, last or doubled after deleting items.
void TidySeparators(HMENU menu) {
    bool previousWasSeparator = true;  // treats a leading separator as doubled
    for (int i = 0; i < GetMenuItemCount(menu);) {
        MENUITEMINFOW info{sizeof(info), MIIM_FTYPE};
        GetMenuItemInfoW(menu, i, TRUE, &info);
        bool separator = (info.fType & MFT_SEPARATOR) != 0;
        if (separator && (previousWasSeparator || i == GetMenuItemCount(menu) - 1)) {
            DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        previousWasSeparator = separator;
        ++i;
    }
}

BOOL WINAPI Hook_TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved, HWND wnd, const RECT* rect) {
    bool removed = false;
    for (UINT id : kPicasaMenuIds) removed |= DeleteMenu(menu, id, MF_BYCOMMAND) != FALSE;
    if (removed) TidySeparators(menu);
    return g_trackPopupMenu(menu, flags, x, y, reserved, wnd, rect);
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_createFileW;

bool IsPicasaIni(LPCWSTR path) {
    if (!path) return false;
    size_t n = wcslen(path);
    static const wchar_t kName[] = L"picasa.ini";
    constexpr size_t kLen = sizeof(kName) / sizeof(kName[0]) - 1;
    return n >= kLen && _wcsicmp(path + n - kLen, kName) == 0;  // .picasa.ini and Picasa.ini
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE templ) {
    if (IsPicasaIni(path)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (!(access & (GENERIC_WRITE | FILE_WRITE_DATA)) && IsRespackPath(path)) {
        std::wstring patched = PatchedRespack(path);
        if (!patched.empty()) return g_createFileW(patched.c_str(), access, share, sa, disposition, flags, templ);
    }
    return g_createFileW(path, access, share, sa, disposition, flags, templ);
}

}  // namespace

void RemovePicasaFeatures() {
    static const uint8_t kSendToPicasa[] = {0x83, 0xEC, 0x1C, 0x55, 0x8B, 0x6C, 0x24, 0x24};
    static const uint8_t kQuickUpload[] = {0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x56, 0x57};
    static const uint8_t kSetStar[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x8D};
    static const uint8_t kMoreLoopStart[] = {0x33, 0xC0, 0x89, 0x44, 0x24, 0x20, 0x33, 0xF6};
    PatchJump(0x420860, ReturnZero, kSendToPicasa, sizeof(kSendToPicasa), "SendToPicasa");
    PatchJump(0x42d650, ReturnZero, kQuickUpload, sizeof(kQuickUpload), "QuickUpload");
    PatchJump(0x43d980, ReturnZero, kSetStar, sizeof(kSetStar), "SetStar");
    PatchDword(0x4256eb, 0x7213c0, 0x7212a0, "SettingsButton.name");
    PatchDword(0x4256f0, 10, 8, "SettingsButton.length");
    PatchDword(0x425709, 0x7213c0, 0x7212a0, "SettingsButton.name2");
    PatchJump(0x42ad29, MoreListFromConfig, kMoreLoopStart, sizeof(kMoreLoopStart), "MoreListStart");
    static const uint8_t kRecordRotation[] = {0x83, 0xEC, 0x28, 0x53, 0x55, 0x56, 0x8B, 0xF1};
    PatchJump(0x438880, ReturnZeroPop4, kRecordRotation, sizeof(kRecordRotation), "RecordRotation");
    Hook(reinterpret_cast<uintptr_t>(&TrackPopupMenu), &Hook_TrackPopupMenu,
         reinterpret_cast<void**>(&g_trackPopupMenu), "TrackPopupMenu");
    // KernelBase's CreateFileW also serves CreateFileA, so one hook covers both.
    if (auto* fn = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW"))
        Hook(reinterpret_cast<uintptr_t>(fn), &Hook_CreateFileW, reinterpret_cast<void**>(&g_createFileW),
             "CreateFileW");
}
