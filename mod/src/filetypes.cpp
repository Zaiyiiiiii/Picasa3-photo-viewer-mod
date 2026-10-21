// File type detection by extension.
//
// 0x513990 TypeFromPath(path in EDX) -> type id in EAX.  The original walks the {ext, type} table at
// 0x770a18 (terminated by a null ext).  Type ids seen there:
//   2 jpeg  3 gif  6 bmp  7 psd  8 avi  9 quicktime/mp4  0xa mpeg/ts/mkv  0xd tiff  0xe png  0xf mp3
//   0x12 camera raw  0x15 url  0x16 tga  0x1b txt  0x1c source  0x1d ogg  0x1f webp
// Images (0x513940 IsImageFile): 2 3 6 7 0xd 0xe 0x12 0x15 0x16 0x1f.
//
// New image formats (and GIF, for animation) are mapped to 0x1f: the WebP load path (0x4131e0) reads the
// whole file and decodes through 0x5fbb40 / 0x5fba60, which decoder.cpp replaces with a generic decoder.
// Videos are mapped to 0x1f as well (originally they were only handed to the shell's player); decoder.cpp
// keeps them from being read into memory.
//
// The same {ext, type} table also feeds two lists the user sees:
//   0x513f80 ExtensionsOfType(type, &list) collects every ext of a type. The "default viewer" page of
//            the settings walks {type, "setup/cb_*", label} at 0x7717c0 and registers each type's exts
//            under Software\Classes. Its four reads of the table are pointed at kAssocTypes (original
//            entries plus the new image formats, which register with the WebP checkbox).
//   Open (Ctrl+O, 0x424090) filters on a single "*.jpg" pattern; GetOpenFileName gets the full list.

#include <windows.h>
#include <commdlg.h>

#include <cstring>
#include <string>

#include "filetypes.h"
#include "hooks.h"
#include "patch.h"

namespace {

struct ExtType {
    const char* ext;
    int type;
};

const ExtType kExtraTypes[] = {
    {".heic", kTypeModernImage}, {".heif", kTypeModernImage}, {".hif", kTypeModernImage},
    {".avif", kTypeModernImage}, {".jxl", kTypeModernImage},
    {".gif", kTypeModernImage},  // was 3: the original loader only decodes the first frame
};

const char* const kVideoExtensions[] = {
    ".mp4", ".m4v", ".mov", ".qt",  ".3gp", ".3g2", ".mkv", ".webm", ".avi", ".divx", ".wmv", ".asf",
    ".flv", ".ts",  ".mts", ".m2t", ".m2ts", ".tp", ".mpg", ".mpeg", ".m2v", ".vob",  ".ogv", ".mod",
    ".tod", ".mxf",
};

const auto* const kOriginalTypes = reinterpret_cast<const ExtType*>(0x770a18);

// Same as 0x513f00: pointer to the last '.', or to the start of the string if there is none.
const char* FindExtension(const char* path) {
    const char* p = path + strlen(path);
    while (p > path && *p != '.') --p;
    return p;
}

extern "C" int __cdecl TypeFromPath(const char* path) {
    if (!path || !*path) return 0;
    const char* ext = FindExtension(path);
    if (*ext != '.') return -1;
    for (const ExtType& e : kExtraTypes)
        if (_stricmp(ext, e.ext) == 0) return e.type;
    if (IsVideoPath(path)) return kTypeModernImage;
    for (const ExtType* e = kOriginalTypes; e->ext; ++e)
        if (_stricmp(ext, e->ext) == 0) return e->type;
    return -1;
}

extern "C" __declspec(naked) void Hook_TypeFromPath() {
    __asm {
        push edx
        call TypeFromPath
        add esp, 4
        ret
    }
}

// Original table with the new image formats in front (GIF keeps type 3 here: it is its own checkbox).
ExtType g_assocTypes[sizeof(kExtraTypes) / sizeof(kExtraTypes[0]) + 128];  // 73 originals

void BuildAssocTypes() {
    size_t n = 0;
    for (const ExtType& e : kExtraTypes)
        if (e.type == kTypeModernImage && _stricmp(e.ext, ".gif") != 0) g_assocTypes[n++] = e;
    for (const ExtType* e = kOriginalTypes; e->ext && n + 1 < _countof(g_assocTypes); ++e) g_assocTypes[n++] = *e;
    g_assocTypes[n] = {};
}

// Settings label next to the WebP checkbox, used in the association messages.
const char kModernLabel[] = ".WEBP, .HEIC, .HEIF, .AVIF, .JXL";

bool IsImageType(int type) {
    switch (type) {
        case 2: case 3: case 6: case 7: case 0xd: case 0xe: case 0x12: case 0x16: case 0x1f: return true;
        default: return false;
    }
}

// "*.jpg;*.jpeg;...": every image and video extension the viewer opens.
std::string OpenPattern() {
    std::string s;
    auto add = [&s](const char* ext) {
        std::string item = std::string("*") + ext;
        for (char& c : item) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if ((";" + s + ";").find(";" + item + ";") != std::string::npos) return;
        if (!s.empty()) s += ';';
        s += item;
    };
    for (const ExtType* e = kOriginalTypes; e->ext; ++e)
        if (IsImageType(e->type)) add(e->ext);
    for (const ExtType& e : kExtraTypes) add(e.ext);
    for (const char* v : kVideoExtensions) add(v);
    return s;
}

// Filter = pairs of NUL-terminated strings ending in an empty one. Replaces the pattern of a lone
// "*.jpg" pair, keeping its (localized) label; returns false for any other dialog.
template <typename Char>
bool WidenFilter(const Char* filter, std::basic_string<Char>& out) {
    if (!filter || !*filter) return false;
    const Char* label = filter;
    const Char* pattern = label;
    while (*pattern) ++pattern;
    ++pattern;
    const Char* rest = pattern;
    while (*rest) ++rest;
    ++rest;
    static const char kJpg[] = "*.jpg";
    for (size_t i = 0; i < sizeof(kJpg); ++i)
        if (pattern[i] != Char(kJpg[i])) return false;
    if (*rest) return false;
    static const std::string all = OpenPattern();
    out.assign(label, pattern);
    out.append(all.begin(), all.end());
    out.push_back(0);
    out.push_back(0);
    return true;
}

using GetOpenFileNameAFn = BOOL(WINAPI*)(LPOPENFILENAMEA);
using GetOpenFileNameWFn = BOOL(WINAPI*)(LPOPENFILENAMEW);
GetOpenFileNameAFn g_getOpenFileNameA;
GetOpenFileNameWFn g_getOpenFileNameW;

BOOL WINAPI Hook_GetOpenFileNameA(LPOPENFILENAMEA ofn) {
    std::string filter;
    if (!ofn || !WidenFilter(ofn->lpstrFilter, filter)) return g_getOpenFileNameA(ofn);
    LPCSTR original = ofn->lpstrFilter;
    ofn->lpstrFilter = filter.c_str();
    BOOL ok = g_getOpenFileNameA(ofn);
    ofn->lpstrFilter = original;
    return ok;
}

BOOL WINAPI Hook_GetOpenFileNameW(LPOPENFILENAMEW ofn) {
    std::wstring filter;
    if (!ofn || !WidenFilter(ofn->lpstrFilter, filter)) return g_getOpenFileNameW(ofn);
    LPCWSTR original = ofn->lpstrFilter;
    ofn->lpstrFilter = filter.c_str();
    BOOL ok = g_getOpenFileNameW(ofn);
    ofn->lpstrFilter = original;
    return ok;
}

void InstallFormatLists() {
    BuildAssocTypes();
    const auto table = reinterpret_cast<uint32_t>(g_assocTypes);
    // ExtensionsOfType: cmp [0x770a18],0 / cmp ecx,[eax+0x770a1c] / mov edx,[eax+0x770a18] / cmp [eax+0x770a18],0
    PatchDword(0x513fc9, 0x770a18, table, "ExtensionsOfType.first");
    PatchDword(0x513fe6, 0x770a1c, table + 4, "ExtensionsOfType.type");
    PatchDword(0x513ff2, 0x770a18, table, "ExtensionsOfType.ext");
    PatchDword(0x5141b2, 0x770a18, table, "ExtensionsOfType.next");
    // {type 0x1f, "setup/cb_webp", ".WEBP"}: label of the 7th settings row.
    PatchDword(0x7717c0 + 6 * 12 + 8, 0x721cd0, reinterpret_cast<uint32_t>(kModernLabel), "WebpLabel");

    HMODULE comdlg = LoadLibraryW(L"comdlg32.dll");
    if (auto* fn = comdlg ? GetProcAddress(comdlg, "GetOpenFileNameA") : nullptr)
        Hook(reinterpret_cast<uintptr_t>(fn), &Hook_GetOpenFileNameA, reinterpret_cast<void**>(&g_getOpenFileNameA),
             "GetOpenFileNameA");
    if (auto* fn = comdlg ? GetProcAddress(comdlg, "GetOpenFileNameW") : nullptr)
        Hook(reinterpret_cast<uintptr_t>(fn), &Hook_GetOpenFileNameW, reinterpret_cast<void**>(&g_getOpenFileNameW),
             "GetOpenFileNameW");
}

}  // namespace

std::vector<std::string> ExtensionsOf(FileGroup group) {
    std::vector<std::string> exts;
    auto add = [&exts](const char* ext) {
        std::string e(ext);
        for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const std::string& have : exts)
            if (have == e) return;
        exts.push_back(e);
    };
    int type = -1;
    switch (group) {
        case FileGroup::Jpeg: type = 2; break;
        case FileGroup::Bmp: type = 6; break;
        case FileGroup::Gif: type = 3; break;
        case FileGroup::Png: type = 0xe; break;
        case FileGroup::Tiff: type = 0xd; break;
        case FileGroup::Tga: type = 0x16; break;
        case FileGroup::Modern: type = kTypeModernImage; break;
        case FileGroup::Raw: type = 0x12; break;
        case FileGroup::Video:
            for (const char* v : kVideoExtensions) add(v);
            return exts;
        default: return exts;
    }
    for (const ExtType* e = kOriginalTypes; e->ext; ++e)
        if (e->type == type) add(e->ext);
    if (group == FileGroup::Modern)
        for (const ExtType& e : kExtraTypes)
            if (_stricmp(e.ext, ".gif") != 0) add(e.ext);
    return exts;
}

bool IsVideoPath(const char* path) {
    const char* ext = path ? FindExtension(path) : nullptr;
    if (!ext || *ext != '.') return false;
    for (const char* v : kVideoExtensions)
        if (_stricmp(ext, v) == 0) return true;
    return false;
}

void InstallFileTypeHooks() {
    static const uint8_t kTypeFromPath[] = {0x85, 0xD2, 0x74, 0x69, 0x80, 0x3A, 0x00, 0x74};
    PatchJump(0x513990, Hook_TypeFromPath, kTypeFromPath, sizeof(kTypeFromPath), "TypeFromPath");
    InstallFormatLists();
}
