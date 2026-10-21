#pragma once

#include <string>

// True for ...\runtime\slingshot\respack.yt, the viewer's UI resource pack.
bool IsRespackPath(const wchar_t* path);

// Path of the pack with the mod's changes, built from `original` on first use (respack.cpp); empty if it
// cannot be made, then the original is used as is.
std::wstring PatchedRespack(const wchar_t* original);
