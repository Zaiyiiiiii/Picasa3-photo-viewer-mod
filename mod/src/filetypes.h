#pragma once

// Type id of the original WebP path, reused for every format decoded by decoder.cpp.
constexpr int kTypeModernImage = 0x1f;

#include <string>
#include <vector>

bool IsVideoPath(const char* path);
void InstallFileTypeHooks();

// Formats offered for file association, as in the original settings (plus the new formats and video).
enum class FileGroup { Jpeg, Bmp, Gif, Png, Tiff, Tga, Modern, Raw, Video, Count };
// Lower-case extensions with the dot (".jpg").
std::vector<std::string> ExtensionsOf(FileGroup group);
