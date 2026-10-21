#pragma once

#include <cstddef>
#include <cstdint>

// Addresses are absolute VAs of PicasaPhotoViewer.exe 3.9.0 (image base 0x400000, no ASLR).

// Overwrite the first 5 bytes at `target` with `jmp replacement`, after checking that
// `expected` matches the bytes currently there. Returns false (and patches nothing) on mismatch.
bool PatchJump(uintptr_t target, const void* replacement, const uint8_t* expected, size_t expectedLen,
               const char* name);

// Replace the byte at `target` with `value`, if it currently holds `expected`.
bool PatchByte(uintptr_t target, uint8_t expected, uint8_t value, const char* name);

// Replace the 4 bytes at `target` (code or data) with `value`, if they currently hold `expected`.
bool PatchDword(uintptr_t target, uint32_t expected, uint32_t value, const char* name);

void Log(const char* fmt, ...);
