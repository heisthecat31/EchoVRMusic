// Windows audio capture into a stereo float ring that every speaker voice reads from.
#pragma once
#include <cstdint>
#include <string>

namespace capture {

// Source strings (EchoVRMusic.txt `Source =`):
//   system            everything Windows plays except Echo itself
//   process:<exe>     only that program's audio, e.g. process:spotify.exe
void Start(const std::string& source, void (*log)(const char*, ...));
void Stop();

// 0 until capture is running.
uint32_t SampleRate();

// Monotonic count of frames written. Frame i lives at Ring()[(i & kMask) * 2].
static const uint32_t kFrames = 1u << 16;
static const uint32_t kMask = kFrames - 1;
uint64_t WriteCursor();
const float* Ring();

}  // namespace capture
