// The viewer's UI resource pack (runtime\slingshot\respack.yt) with the mod's changes, built at run time:
// the installed files stay as they are. The first time the viewer opens the pack, a patched copy is
// written to %LOCALAPPDATA%\PicasaPhotoViewerMod and every open of the pack is redirected to it
// (picasa_features.cpp, CreateFileW). The copy is rebuilt when the original or kPatchVersion changes.
//
// Pack layout: u32 offset of the index at 0; index = u32 count, then {name\0, u32 offset}. Layer record:
// int16 l, t, r, b; u32 flags; u8 type (0 raw premultiplied BGRA, 1 runs of {u8 count, BGRA}, 2 solid
// color u32). Layout scripts ("tre:") are plain text. Everything before the index keeps its byte length.
//
// Changes:
//  - action bar: the Picasa-library buttons lose their hover tip line ("Property setvisible 0" padded);
//  - ss/#fader, the dark fade behind the filmstrip, becomes transparent (the fullscreen background shows
//    through; the viewer shows and fades ss/skirt in code, so its pixels are cleared instead);
//  - "More" becomes the settings button: renamed to an icon button with the old list's settings icon,
//    a 28 x 19 solid placeholder like the rotate buttons, the icon downscaled to 14 x 13, tooltip text.

#include "respack.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "patch.h"

namespace {

constexpr int kPatchVersion = 1;  // bump when the changes below change

struct TextEdit {
    const char* head;
    const char* old;
    const char* replacement;  // null: hide the line ("Property setvisible 0" + padded comment)
};

const char kHoverTip[] = "SharedHandler ss/tip hottip ss/statustext";
const TextEdit kTextEdits[] = {
    {"ss/launch_picasa: ss/controlbar_container\r\n", kHoverTip, nullptr},
    {"ss/to_web: ss/controlbar_container\r\n", kHoverTip, nullptr},
    {"ss/star: ss/controlbar_container\r\n", kHoverTip, nullptr},
    {"Tooltip ss/more\r\n", "More Options", "Configure..."},
};

const char kFader[] = "layer:ss/#fader";
const char kMore[] = "layer:ss/popuplist: more";
const char kMoreRenamed[] = "layer:ss/superbutton(buttonicon,,ss/#config_icon): more";
const int16_t kMoreRect[4] = {740, 744, 768, 763};
const char kConfigIcon[] = "layer:ss/#config_icon";
const int kIconW = 14, kIconH = 13;

using Bytes = std::vector<uint8_t>;

template <typename T>
T Get(const Bytes& b, size_t at) {
    T v;
    memcpy(&v, b.data() + at, sizeof(T));
    return v;
}
template <typename T>
void Put(Bytes& b, size_t at, T v) {
    memcpy(b.data() + at, &v, sizeof(T));
}

struct Entry {
    std::string name;
    uint32_t offset;
};

bool ApplyTextEdit(Bytes& b, size_t limit, const TextEdit& e) {
    std::string old = std::string(e.head) + e.old;
    std::string rep = e.head;
    if (e.replacement) {
        rep += e.replacement;
    } else {
        std::string hide = "Property setvisible 0\r\n#";
        rep += hide + std::string(strlen(e.old) - hide.size(), ' ');
    }
    if (rep.size() != old.size()) return false;
    auto it = std::search(b.begin(), b.begin() + limit, old.begin(), old.end());
    if (it == b.begin() + limit) return false;
    std::copy(rep.begin(), rep.end(), it);
    return true;
}

// Pixels of a layer as premultiplied BGRA; false if it has none.
bool ReadLayer(const Bytes& b, size_t q, int& w, int& h, std::vector<uint32_t>& px) {
    w = Get<int16_t>(b, q + 4) - Get<int16_t>(b, q);
    h = Get<int16_t>(b, q + 6) - Get<int16_t>(b, q + 2);
    if (w <= 0 || h <= 0) return false;
    px.assign(size_t(w) * h, 0);
    uint8_t type = b[q + 12];
    size_t at = q + 13;
    if (type == 0) {
        for (size_t i = 0; i < px.size(); ++i) px[i] = Get<uint32_t>(b, at + 4 * i);
        return true;
    }
    if (type != 1) return false;
    for (size_t i = 0; i < px.size(); at += 5) {
        uint32_t v = Get<uint32_t>(b, at + 1);
        for (int n = 0; n < b[at] && i < px.size(); ++n) px[i++] = v;
    }
    return true;
}

void ClearLayer(Bytes& b, size_t q) {
    int w = Get<int16_t>(b, q + 4) - Get<int16_t>(b, q), h = Get<int16_t>(b, q + 6) - Get<int16_t>(b, q + 2);
    size_t at = q + 13;
    if (b[q + 12] == 0) {
        memset(b.data() + at, 0, size_t(w) * h * 4);
    } else if (b[q + 12] == 1) {
        for (int done = 0; done < w * h && b[at]; at += 5) {
            done += b[at];
            memset(b.data() + at + 1, 0, 4);
        }
    }
}

// Area-average downscale of a layer, written back in place as runs if it fits before `room`.
bool ShrinkLayer(Bytes& b, size_t q, size_t room, int tw, int th) {
    int sw, sh;
    std::vector<uint32_t> src;
    if (!ReadLayer(b, q, sw, sh, src)) return false;
    double sx = double(sw) / tw, sy = double(sh) / th;
    Bytes runs;
    uint32_t last = 0;
    int run = 0;
    auto flush = [&] {
        runs.push_back(uint8_t(run));
        for (int c = 0; c < 4; ++c) runs.push_back(uint8_t(last >> (8 * c)));
    };
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            double acc[4] = {}, area = 0;
            for (int v = int(std::floor(y * sy)); v < int(std::ceil((y + 1) * sy)); ++v) {
                double wy = std::min(v + 1.0, (y + 1) * sy) - std::max(double(v), y * sy);
                for (int u = int(std::floor(x * sx)); u < int(std::ceil((x + 1) * sx)); ++u) {
                    double wgt = wy * (std::min(u + 1.0, (x + 1) * sx) - std::max(double(u), x * sx));
                    uint32_t p = src[size_t(v) * sw + u];
                    for (int c = 0; c < 4; ++c) acc[c] += wgt * ((p >> (8 * c)) & 255);
                    area += wgt;
                }
            }
            uint32_t value = 0;
            for (int c = 0; c < 4; ++c) value |= uint32_t(std::lround(acc[c] / area)) << (8 * c);
            if (run > 0 && value == last && run < 255) {
                ++run;
                continue;
            }
            if (run > 0) flush();
            last = value;
            run = 1;
        }
    flush();
    if (13 + runs.size() > room) return false;
    Put<int16_t>(b, q + 4, int16_t(Get<int16_t>(b, q) + tw));
    Put<int16_t>(b, q + 6, int16_t(Get<int16_t>(b, q + 2) + th));
    b[q + 12] = 1;
    memcpy(b.data() + q + 13, runs.data(), runs.size());
    return true;
}

bool ReadFileBytes(const std::wstring& path, Bytes& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const Bytes& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// The patched pack, or empty on any mismatch with the expected original (then the original is used).
Bytes Patch(Bytes b) {
    if (b.size() < 8) return {};
    uint32_t index = Get<uint32_t>(b, 0);
    if (index + 4 > b.size()) return {};
    uint32_t count = Get<uint32_t>(b, index);
    std::vector<Entry> entries;
    size_t at = index + 4;
    for (uint32_t i = 0; i < count; ++i) {
        auto end = std::find(b.begin() + at, b.end(), uint8_t(0));
        if (end == b.end() || size_t(end - b.begin()) + 5 > b.size()) return {};
        entries.push_back({std::string(b.begin() + at, end), Get<uint32_t>(b, end - b.begin() + 1)});
        at = end - b.begin() + 5;
    }
    if (at != b.size()) return {};
    auto find = [&entries](const char* name) -> const Entry* {
        for (const Entry& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    };
    auto room = [&entries, index](uint32_t offset) {
        uint32_t next = index;
        for (const Entry& e : entries)
            if (e.offset > offset && e.offset < next) next = e.offset;
        return size_t(next - offset);
    };

    for (const TextEdit& e : kTextEdits)
        if (!ApplyTextEdit(b, index, e)) {
            Log("respack: text edit after '%s' not applicable", e.head);
            return {};
        }
    const Entry* fader = find(kFader);
    const Entry* more = find(kMore);
    const Entry* icon = find(kConfigIcon);
    if (!fader || !more || !icon) {
        Log("respack: expected layers missing");
        return {};
    }
    ClearLayer(b, fader->offset);
    for (int i = 0; i < 4; ++i) Put<int16_t>(b, more->offset + 2 * i, kMoreRect[i]);
    b[more->offset + 12] = 2;  // solid color
    Put<uint32_t>(b, more->offset + 13, 0xff000000u);
    if (!ShrinkLayer(b, icon->offset, room(icon->offset), kIconW, kIconH)) {
        Log("respack: cannot shrink the settings icon");
        return {};
    }

    // The index anew, with the renamed layer.
    b.resize(index + 4);
    for (const Entry& e : entries) {
        const std::string& name = e.name == kMore ? std::string(kMoreRenamed) : e.name;
        b.insert(b.end(), name.begin(), name.end());
        b.push_back(0);
        uint8_t off[4];
        memcpy(off, &e.offset, 4);
        b.insert(b.end(), off, off + 4);
    }
    return b;
}

std::wstring CacheDir() {
    wchar_t* local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
    CoTaskMemFree(local);
    return dir.empty() ? dir : dir + L"\\PicasaPhotoViewerMod";
}

std::mutex g_mutex;
std::wstring g_original, g_patched;  // last original seen and its patched copy
thread_local bool t_busy;

}  // namespace

bool IsRespackPath(const wchar_t* path) {
    static const wchar_t kTail[] = L"\\runtime\\slingshot\\respack.yt";
    size_t n = path ? wcslen(path) : 0, k = wcslen(kTail);
    return n >= k && _wcsicmp(path + n - k, kTail) == 0;
}

std::wstring PatchedRespack(const wchar_t* original) {
    if (t_busy) return {};  // our own reads and writes
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_original == original) return g_patched;
    t_busy = true;
    std::wstring result;
    WIN32_FILE_ATTRIBUTE_DATA info;
    std::wstring dir = CacheDir();
    if (!dir.empty() && GetFileAttributesExW(original, GetFileExInfoStandard, &info)) {
        wchar_t name[96];
        swprintf_s(name, L"\\respack-%lu-%08lx%08lx-v%d.yt", info.nFileSizeLow, info.ftLastWriteTime.dwHighDateTime,
                   info.ftLastWriteTime.dwLowDateTime, kPatchVersion);
        std::wstring path = dir + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            result = path;
        } else {
            Bytes data;
            if (ReadFileBytes(original, data)) {
                Bytes patched = Patch(std::move(data));
                CreateDirectoryW(dir.c_str(), nullptr);
                if (!patched.empty() && WriteFileBytes(path, patched)) {
                    result = path;
                    Log("respack: patched copy written to %S", path.c_str());
                } else {
                    Log("respack: patching failed, using the original");
                }
            }
        }
    }
    g_original = original;
    g_patched = result;
    t_busy = false;
    return result;
}
