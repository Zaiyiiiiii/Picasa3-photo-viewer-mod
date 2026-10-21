// Call tracer for reverse engineering. Reads trace.txt next to the exe:
//   0x00439730 ImageLoaderJob::Run 20                       <- address, name, max hits (default 10)
//   0x004f0570 Draw 5 ecx+0x2a4:0x40 [ecx+0x264]+0:0x10      <- optional memory dumps (up to 4)
// A dump base is a register (eax ecx edx ebx esi edi ebp) or a stack argument (arg0..arg3);
// "[base+off]" dereferences once before adding the second offset.
// Each traced function gets a register-transparent stub (pushfd/pushad, log, restore, jump to the
// MinHook trampoline), so it works with the custom register conventions LTCG produced.

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "patch.h"

namespace {

// Layout pushed by the stub: pushfd then pushad, then the stub's own return slot is the caller's.
struct SavedRegs {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t ret;       // return address into the caller
    uint32_t args[4];   // first stack arguments
};

struct Dump {
    int base = -1;        // index into the uint32 view of SavedRegs
    bool deref = false;
    uint32_t off1 = 0, off2 = 0, len = 0;
    char text[48] = {};
};

struct TracePoint {
    uintptr_t address;
    char name[64];
    LONG remaining;
    void* trampoline;
    Dump dumps[4];
};

std::vector<TracePoint>* g_points;

int BaseIndex(const char* s, size_t n) {
    static const char* const kRegs[] = {"edi", "esi", "ebp", "esp", "ebx", "edx", "ecx", "eax"};
    for (int i = 0; i < 8; ++i)
        if (n == 3 && strncmp(s, kRegs[i], 3) == 0) return i;
    if (n == 4 && strncmp(s, "arg", 3) == 0 && s[3] >= '0' && s[3] <= '3') return 10 + (s[3] - '0');
    return -1;
}

// "ecx+0x2a4:0x40" or "[ecx+0x264]+0x10:0x20"
bool ParseDump(const char* spec, Dump& d) {
    strncpy_s(d.text, spec, _TRUNCATE);
    const char* p = spec;
    if (*p == '[') {
        d.deref = true;
        ++p;
    }
    size_t n = strcspn(p, "+]:");
    d.base = BaseIndex(p, n);
    if (d.base < 0) return false;
    p += n;
    if (*p == '+') d.off1 = strtoul(p + 1, const_cast<char**>(&p), 0);
    if (d.deref) {
        if (*p++ != ']') return false;
        if (*p == '+') d.off2 = strtoul(p + 1, const_cast<char**>(&p), 0);
    }
    if (*p++ != ':') return false;
    d.len = strtoul(p, nullptr, 0);
    return d.len > 0 && d.len <= 0x200;
}

// Format `len` bytes at `addr` as dwords; false if the memory is not readable.
bool FormatMemory(uint32_t addr, uint32_t len, char* out, size_t outSize) {
    __try {
        size_t pos = 0;
        for (uint32_t i = 0; i + 4 <= len && pos + 10 < outSize; i += 4)
            pos += sprintf_s(out + pos, outSize - pos, "%08X ", *reinterpret_cast<const uint32_t*>(addr + i));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadDword(uint32_t addr, uint32_t* value) {
    __try {
        *value = *reinterpret_cast<const uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

extern "C" void __stdcall TraceHit(uint32_t index, SavedRegs* r) {
    TracePoint& p = (*g_points)[index];
    if (InterlockedDecrement(&p.remaining) < 0) return;
    Log("TRACE %s ret=%08X ecx=%08X eax=%08X edx=%08X esi=%08X edi=%08X args=%08X %08X %08X %08X", p.name, r->ret,
        r->ecx, r->eax, r->edx, r->esi, r->edi, r->args[0], r->args[1], r->args[2], r->args[3]);
    const auto* view = reinterpret_cast<const uint32_t*>(r);
    for (const Dump& d : p.dumps) {
        if (d.base < 0) break;
        uint32_t addr = view[d.base] + d.off1;
        if (d.deref && !ReadDword(addr, &addr)) {
            Log("      %s: unreadable", d.text);
            continue;
        }
        addr += d.off2;
        char buf[0x200 / 4 * 9 + 16];
        if (FormatMemory(addr, d.len, buf, sizeof(buf)))
            Log("      %s @%08X: %s", d.text, addr, buf);
        else
            Log("      %s @%08X: unreadable", d.text, addr);
    }
}

// Builds:  pushfd; pushad; push esp; push index; call TraceHit; popad; popfd; jmp [trampoline]
void* BuildStub(uint32_t index, void** trampolineSlot) {
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    uint8_t* p = code;
    *p++ = 0x9C;                                   // pushfd
    *p++ = 0x60;                                   // pushad
    *p++ = 0x54;                                   // push esp
    *p++ = 0x68; memcpy(p, &index, 4); p += 4;     // push index
    *p++ = 0xE8;                                   // call TraceHit
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&TraceHit) - reinterpret_cast<uintptr_t>(p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x61;                                   // popad
    *p++ = 0x9D;                                   // popfd
    *p++ = 0xFF; *p++ = 0x25;                      // jmp dword ptr [slot]
    memcpy(p, &trampolineSlot, 4); p += 4;
    FlushInstructionCache(GetCurrentProcess(), code, p - code);
    return code;
}

}  // namespace

void InstallTracePoints() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\') --n;
    wcscpy_s(path + n, MAX_PATH - n, L"trace.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;

    g_points = new std::vector<TracePoint>();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        TracePoint p{};
        p.remaining = 10;
        char* ctx = nullptr;
        int field = 0, dumps = 0;
        for (char* tok = strtok_s(line, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx), ++field) {
            if (field == 0) p.address = strtoul(tok, nullptr, 16);
            else if (field == 1) strncpy_s(p.name, tok, _TRUNCATE);
            else if (field == 2) p.remaining = strtol(tok, nullptr, 0);
            else if (dumps < 4 && !ParseDump(tok, p.dumps[dumps])) {
                Log("trace: bad dump spec '%s'", tok);
                p.dumps[dumps] = Dump{};
            } else if (dumps < 4) {
                ++dumps;
            }
        }
        if (!p.address) continue;
        if (!p.name[0]) sprintf_s(p.name, "%08X", (unsigned)p.address);
        g_points->push_back(p);
    }
    fclose(f);
    if (g_points->empty()) return;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("trace: MH_Initialize failed (%d)", s);
        return;
    }
    // The vector is not resized after this point, so element addresses stay valid.
    for (uint32_t i = 0; i < g_points->size(); ++i) {
        TracePoint& p = (*g_points)[i];
        void* stub = BuildStub(i, &p.trampoline);
        auto* target = reinterpret_cast<void*>(p.address);
        if (!stub || (s = MH_CreateHook(target, stub, &p.trampoline)) != MH_OK || (s = MH_EnableHook(target)) != MH_OK) {
            Log("trace: cannot hook %s @%08X (%d)", p.name, (unsigned)p.address, s);
            continue;
        }
        Log("trace: %s @%08X", p.name, (unsigned)p.address);
    }
}
