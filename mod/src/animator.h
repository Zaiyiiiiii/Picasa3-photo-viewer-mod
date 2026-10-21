#pragma once

#include <cstddef>
#include <cstdint>

// Called by the decode hook (any thread) after frame 0 of a multi-frame image was decoded for display.
// The animation is bound to the display buffer that the following ResampleToTarget call produces.
void AnimNoteDecoded(const uint8_t* data, size_t size);

void InstallAnimatorHooks();
