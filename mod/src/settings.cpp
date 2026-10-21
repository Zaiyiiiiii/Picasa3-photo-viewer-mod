// Replacement for the viewer's settings window.
//
// The original (0x425820 ShowOptions(slingshot, mode), stdcall) builds a slingshot UI from the "setup"
// tree of the resource pack: file types, a Google sign-in tab and five viewer options. It draws black
// blocks over its controls (in the unmodified viewer too). It is replaced by a plain Win32 dialog built
// from an in-memory template: no resources, system-drawn controls that follow the DPI and theme.
//
// Viewer options live where the original keeps them, HKCU\Software\Google\Picasa\Picasa2\Preferences
// (DWORD). After its window closes the original applies three of them right away (0x425916..0x4259a8):
//   transparent background -> slingshot +0x259, auto-hide action bar -> slingshot +0x25a,
//   mouse wheel zoom       -> C3DFilmstrip ("ss/filmstrip", kept at slingshot +0x120c) +0x270.
//
// File association, the Windows 10/11 way: applications may not set defaults themselves. The viewer is
// registered as a handler (a ProgID, OpenWithProgids for each extension, Capabilities and
// RegisteredApplications) and the user picks it in Settings > Default apps, opened on its page.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "filetypes.h"
#include "hooks.h"
#include "patch.h"

namespace {

constexpr uintptr_t kShowOptions = 0x425820;
constexpr wchar_t kPrefsKey[] = L"Software\\Google\\Picasa\\Picasa2\\Preferences";
constexpr wchar_t kProgId[] = L"PicasaPhotoViewer.Mod";
constexpr wchar_t kAppKey[] = L"Software\\PicasaPhotoViewerMod";
constexpr wchar_t kCapabilitiesKey[] = L"Software\\PicasaPhotoViewerMod\\Capabilities";
constexpr wchar_t kAppName[] = L"Picasa Photo Viewer";
constexpr wchar_t kDefaultAppsUri[] = L"ms-settings:defaultapps?registeredAppUser=Picasa%20Photo%20Viewer";

struct GroupInfo {
    FileGroup group;
    const wchar_t* label;
};
constexpr GroupInfo kGroups[] = {
    {FileGroup::Jpeg, L"JPEG"},
    {FileGroup::Bmp, L"BMP"},
    {FileGroup::Gif, L"GIF"},
    {FileGroup::Png, L"PNG"},
    {FileGroup::Tiff, L"TIFF"},
    {FileGroup::Tga, L"TGA"},
    {FileGroup::Modern, L"WebP / HEIC / AVIF / JPEG XL"},
    {FileGroup::Raw, L"RAW 相机格式"},
    {FileGroup::Video, L"视频"},
};

struct OptionInfo {
    const wchar_t* pref;
    const wchar_t* label;
};
constexpr OptionInfo kOptions[] = {
    {L"viewermousewheelzoom", L"用鼠标滚轮缩放（按住 Ctrl 切换照片）"},
    {L"ViewerAutoHideActionbar", L"全屏时自动隐藏操作栏"},
    {L"ViewerFullscreenStartup", L"以全屏方式启动（重新打开查看器后生效）"},
    {L"ViewerTransparentBackground", L"透明背景（重新打开查看器后生效）"},
    {L"EnableColorManagement", L"启用色彩管理（重新打开查看器后生效）"},
};

constexpr int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);
constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);
constexpr WORD kIdGroup = 100;   // + index
constexpr WORD kIdDefaultApps = 120;
constexpr WORD kIdOption = 130;  // + index

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}

// --- preferences ---------------------------------------------------------------------------------------

bool ReadPref(const wchar_t* name, bool fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPrefsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value != 0;
}

void WritePref(const wchar_t* name, bool on) {
    DWORD value = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPrefsKey, name, REG_DWORD, &value, sizeof(value));
}

// --- file association ------------------------------------------------------------------------------------

std::wstring ClassesKey(const std::wstring& sub) { return L"Software\\Classes\\" + sub; }

bool IsRegistered(FileGroup group) {
    std::vector<std::string> exts = ExtensionsOf(group);
    if (exts.empty()) return false;
    std::wstring key = ClassesKey(Widen(exts[0]) + L"\\OpenWithProgids");
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), kProgId, RRF_RT_ANY, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void DeleteValue(const std::wstring& key, const wchar_t* name) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name);
}

void ApplyAssociations(const bool (&wanted)[kGroupCount]) {
    std::wstring exe = ExePath();
    std::wstring exeName = exe.substr(exe.find_last_of(L'\\') + 1);
    bool any = false;
    for (bool w : wanted) any |= w;

    if (any) {
        std::wstring progId = ClassesKey(kProgId);
        SetString(progId, nullptr, L"Picasa 照片查看器");
        SetString(progId + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
        SetString(progId + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
        SetString(ClassesKey(L"Applications\\" + exeName + L"\\shell\\open\\command"), nullptr,
                  L"\"" + exe + L"\" \"%1\"");
        SetString(kCapabilitiesKey, L"ApplicationName", kAppName);
        SetString(kCapabilitiesKey, L"ApplicationDescription", L"Picasa 照片查看器");
        SetString(L"Software\\RegisteredApplications", kAppName, kCapabilitiesKey);
    }
    for (int i = 0; i < kGroupCount; ++i)
        for (const std::string& ext : ExtensionsOf(kGroups[i].group)) {
            std::wstring e = Widen(ext);
            std::wstring openWith = ClassesKey(e + L"\\OpenWithProgids");
            std::wstring supported = ClassesKey(L"Applications\\" + exeName + L"\\SupportedTypes");
            std::wstring caps = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";
            if (wanted[i]) {
                RegSetKeyValueW(HKEY_CURRENT_USER, openWith.c_str(), kProgId, REG_NONE, nullptr, 0);
                SetString(supported, e.c_str(), L"");
                SetString(caps, e.c_str(), kProgId);
            } else {
                DeleteValue(openWith, kProgId);
                DeleteValue(supported, e.c_str());
                DeleteValue(caps, e.c_str());
            }
        }
    if (!any) {
        DeleteValue(L"Software\\RegisteredApplications", kAppName);
        RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
        RegDeleteTreeW(HKEY_CURRENT_USER, ClassesKey(kProgId).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, ClassesKey(L"Applications\\" + exeName).c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Log("settings: file associations updated (%s)", any ? "registered" : "removed");
}

// --- dialog template --------------------------------------------------------------------------------------

class Template {
public:
    Template(const wchar_t* title, short cx, short cy) {
        Align(4);
        DLGTEMPLATE t{};
        t.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        t.cx = cx;
        t.cy = cy;
        count_ = Append(&t, sizeof(t)) + offsetof(DLGTEMPLATE, cdit);
        Word(0);  // no menu
        Word(0);  // default class
        Text(title);
        Word(9);  // point size
        Text(L"Segoe UI");
    }

    void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        Align(4);
        DLGITEMTEMPLATE it{};
        it.style = style | WS_CHILD | WS_VISIBLE;
        it.x = x;
        it.y = y;
        it.cx = cx;
        it.cy = cy;
        it.id = id;
        Append(&it, sizeof(it));
        Word(0xFFFF);
        Word(cls);
        Text(text);
        Word(0);  // no creation data
        ++*reinterpret_cast<WORD*>(data_.data() + count_);
    }

    const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(data_.data()); }

private:
    size_t Append(const void* p, size_t n) {
        size_t at = data_.size();
        data_.insert(data_.end(), static_cast<const BYTE*>(p), static_cast<const BYTE*>(p) + n);
        return at;
    }
    void Align(size_t a) { data_.resize((data_.size() + a - 1) & ~(a - 1)); }
    void Word(WORD w) { Append(&w, sizeof(w)); }
    void Text(const wchar_t* s) { Append(s, (wcslen(s) + 1) * sizeof(wchar_t)); }

    std::vector<BYTE> data_;
    size_t count_ = 0;
};

constexpr WORD kButton = 0x0080, kStatic = 0x0082;

Template BuildTemplate() {
    Template t(L"照片查看器设置", 300, 244);
    t.Item(BS_GROUPBOX, 7, 5, 286, 122, 0xFFFF, kButton, L"文件关联");
    t.Item(SS_LEFT, 15, 18, 270, 18, 0xFFFF, kStatic,
           L"勾选的格式会把本查看器登记为可选的打开方式。Windows 11 的默认应用需要在系统设置中选定。");
    for (int i = 0; i < kGroupCount; ++i)
        t.Item(BS_AUTOCHECKBOX | WS_TABSTOP, i % 2 ? 150 : 15, static_cast<short>(40 + (i / 2) * 13),
               i % 2 ? 135 : 130, 11, static_cast<WORD>(kIdGroup + i), kButton, kGroups[i].label);
    t.Item(BS_PUSHBUTTON | WS_TABSTOP, 15, 106, 130, 14, kIdDefaultApps, kButton, L"打开 Windows 默认应用设置");
    t.Item(BS_GROUPBOX, 7, 131, 286, 86, 0xFFFF, kButton, L"界面");
    for (int i = 0; i < kOptionCount; ++i)
        t.Item(BS_AUTOCHECKBOX | WS_TABSTOP, 15, static_cast<short>(144 + i * 14), 270, 11,
               static_cast<WORD>(kIdOption + i), kButton, kOptions[i].label);
    t.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, 186, 224, 50, 14, IDOK, kButton, L"确定");
    t.Item(BS_PUSHBUTTON | WS_TABSTOP, 243, 224, 50, 14, IDCANCEL, kButton, L"取消");
    return t;
}

// --- dialog --------------------------------------------------------------------------------------------------

struct Result {
    bool groups[kGroupCount];
    bool options[kOptionCount];
};

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            for (int i = 0; i < kGroupCount; ++i)
                CheckDlgButton(dlg, kIdGroup + i, IsRegistered(kGroups[i].group) ? BST_CHECKED : BST_UNCHECKED);
            for (int i = 0; i < kOptionCount; ++i)
                CheckDlgButton(dlg, kIdOption + i, ReadPref(kOptions[i].pref, false) ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdDefaultApps:
                    ShellExecuteW(dlg, nullptr, kDefaultAppsUri, nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                case IDOK: {
                    auto* r = reinterpret_cast<Result*>(GetWindowLongPtrW(dlg, DWLP_USER));
                    for (int i = 0; i < kGroupCount; ++i) r->groups[i] = IsDlgButtonChecked(dlg, kIdGroup + i) != 0;
                    for (int i = 0; i < kOptionCount; ++i)
                        r->options[i] = IsDlgButtonChecked(dlg, kIdOption + i) != 0;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Applies the options the original applies without a restart.
void ApplyRunning(uint8_t* slingshot) {
    slingshot[0x259] = ReadPref(L"ViewerTransparentBackground", false);
    slingshot[0x25a] = ReadPref(L"ViewerAutoHideActionbar", false);
    if (auto* filmstrip = *reinterpret_cast<uint8_t**>(slingshot + 0x120c))
        filmstrip[0x270] = ReadPref(L"viewermousewheelzoom", false);
}

int __stdcall ShowSettings(uint8_t* slingshot, int mode) {
    Log("settings: opened (mode %d)", mode);
    HWND owner = nullptr;
    if (slingshot)
        if (auto* window = *reinterpret_cast<uint8_t**>(slingshot + 0x12ac))
            owner = *reinterpret_cast<HWND*>(window + 0x6c);
    Template t = BuildTemplate();
    Result r{};
    INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, DialogProc,
                                         reinterpret_cast<LPARAM>(&r));
    if (rc == IDOK) {
        bool before[kGroupCount];
        for (int i = 0; i < kGroupCount; ++i) before[i] = IsRegistered(kGroups[i].group);
        if (memcmp(before, r.groups, sizeof(before)) != 0) ApplyAssociations(r.groups);
        for (int i = 0; i < kOptionCount; ++i) WritePref(kOptions[i].pref, r.options[i]);
        if (slingshot) ApplyRunning(slingshot);
    }
    return 1000000;  // handled
}

}  // namespace

void InstallSettingsDialog() {
    static const uint8_t kShowOptionsStart[] = {0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x18};
    PatchJump(kShowOptions, ShowSettings, kShowOptionsStart, sizeof(kShowOptionsStart), "ShowOptions");
}
