// Replacement for the bundled libwebp (2011) used by the type-0x1f load path.
//
// 0x4131e0 LoadModern(path)  EAX = loader, ret 4: opens the file and calls 0x527710, which runs
//   0x527190  ReadFile(ctx, file, info)                                                 (stdcall)
//             reads the whole file into ctx {w, h, size, data, ytBitmap*} and parses the header;
//             info -> {int* w, int* h, int* size, ...} outputs.
// then 0x5272f0 allocates a w*h ytBitmap from the size reported by
//   0x5fbb40  int WebPGetInfo(const uint8_t* data, size_t size, int* w, int* h)          (cdecl)
// and fills its pixels with
//   0x5fba60  DecodeInto(const uint8_t* data, size_t size, size_t bufSize)               (cdecl)
//             EDI -> params: [0] = output buffer, [6] = stride in bytes; returns non-zero on success.
// Output is 32-bit BGRA, not premultiplied (libwebp MODE_BGRA).
//
// Videos must not be read into memory, so for them ReadFile is replaced by a small blob naming the file
// (codec::MakeVideoBlob); the decode hooks recognise it and use FFmpeg.
//
// Color management follows the viewer's "EnableColorManagement" setting: 0x5204e0 builds the manager at
// [0x795d00] (sRGB and display profiles, transforms) and stores the setting in its byte +0x5c.

#include <cstring>

#include "animator.h"
#include "codecs/codec.h"
#include "filetypes.h"
#include "hooks.h"
#include "patch.h"

namespace {

constexpr uintptr_t kLoadModern = 0x4131e0;
constexpr uintptr_t kReadFile = 0x527190;
constexpr uintptr_t kPicasaMalloc = 0x4bae20;  // cdecl void* (size_t); freed by the load path
constexpr uintptr_t kColorManager = 0x795d00;

bool ColorManagementOn() {
    auto* manager = *reinterpret_cast<const uint8_t* const*>(kColorManager);
    return manager && manager[0x5c];
}

using ReadFileFn = unsigned(__stdcall*)(int* ctx, void* file, int** info);
ReadFileFn g_readFile;
void* g_loadModern;

thread_local const char* t_loadPath;  // valid during LoadModern

// GetImageInfo and DecodeInto are called back to back on the same ctx data: keep the parsed source.
struct Opened {
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::unique_ptr<ImageSource> source;
};
thread_local Opened t_opened;

std::unique_ptr<ImageSource> Take(const uint8_t* data, size_t size) {
    if (t_opened.source && t_opened.data == data && t_opened.size == size) {
        t_opened.data = nullptr;
        return std::move(t_opened.source);
    }
    return OpenImageSource(data, size);
}

extern "C" int __cdecl GetImageInfo(const uint8_t* data, size_t size, int* width, int* height) {
    auto src = Take(data, size);
    if (!src) return 0;
    if (width) *width = static_cast<int>(src->width);
    if (height) *height = static_cast<int>(src->height);
    t_opened = {data, size, std::move(src)};
    return 1;
}

extern "C" uint8_t* __cdecl DecodeImageInto(uint32_t* params, const uint8_t* data, size_t size, size_t bufSize) {
    auto* output = reinterpret_cast<uint8_t*>(params[0]);
    uint32_t stride = params[6];
    if (!output) return nullptr;
    auto src = Take(data, size);
    if (!src) return nullptr;
    if (static_cast<uint64_t>(stride) * src->height > bufSize || stride < src->width * 4) {
        Log("DecodeImageInto: %ux%u does not fit buffer (%u bytes, stride %u)", src->width, src->height,
            (unsigned)bufSize, stride);
        return nullptr;
    }
    if (src->NextFrame(output, stride) < 0) return nullptr;
    if (src->frameCount != 1) AnimNoteDecoded(data, size);
    Log("decoded %ux%u via %s (%u frames) into %p", src->width, src->height, src->Name(), src->frameCount, output);
    return output;
}

extern "C" __declspec(naked) void Hook_DecodeInto() {
    __asm {
        push dword ptr [esp + 0Ch]  // bufSize
        push dword ptr [esp + 0Ch]  // size
        push dword ptr [esp + 0Ch]  // data
        push edi                    // params
        call DecodeImageInto
        add esp, 10h
        ret
    }
}

extern "C" void __cdecl NoteLoadPath(const char* path) {
    t_loadPath = path;
    t_opened = {};  // never let a cached source outlive the load that opened it
}

// Register-transparent: record the path argument, then continue into the original.
extern "C" __declspec(naked) void Hook_LoadModern() {
    __asm {
        pushad
        push dword ptr [esp + 24h]  // path (above the 8 saved registers and the return address)
        call NoteLoadPath
        add esp, 4
        popad
        jmp dword ptr [g_loadModern]
    }
}

unsigned __stdcall Hook_ReadFile(int* ctx, void* file, int** info) {
    const char* path = t_loadPath;
    t_loadPath = nullptr;
    if (!path || !IsVideoPath(path)) return g_readFile(ctx, file, info);

    std::vector<uint8_t> blob = codec::MakeVideoBlob(path);
    auto* data = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(kPicasaMalloc)(blob.size()));
    if (!data) return 4;
    memcpy(data, blob.data(), blob.size());
    ctx[2] = static_cast<int>(blob.size());
    ctx[3] = reinterpret_cast<int>(data);
    int w = 0, h = 0;
    if (!GetImageInfo(data, blob.size(), &w, &h)) return 2;
    ctx[0] = w;
    ctx[1] = h;
    if (info) {
        if (info[2]) *info[2] = ctx[2];
        if (info[1]) *info[1] = h;
        if (info[0]) *info[0] = w;
    }
    return 0;
}

}  // namespace

void InstallDecoderHooks() {
    codec::g_useDisplayProfile = ColorManagementOn;
    static const uint8_t kGetInfo[] = {0x53, 0x8B, 0x5C, 0x24, 0x14, 0x55, 0x8B, 0x6C};
    static const uint8_t kDecodeInto[] = {0x83, 0xEC, 0x44, 0x56, 0x68, 0x0C, 0x06, 0x00};
    PatchJump(0x5fbb40, GetImageInfo, kGetInfo, sizeof(kGetInfo), "WebPGetInfo");
    PatchJump(0x5fba60, Hook_DecodeInto, kDecodeInto, sizeof(kDecodeInto), "WebPDecodeInto");
    Hook(kLoadModern, &Hook_LoadModern, &g_loadModern, "LoadModern");
    Hook(kReadFile, &Hook_ReadFile, reinterpret_cast<void**>(&g_readFile), "ReadFile");
}
