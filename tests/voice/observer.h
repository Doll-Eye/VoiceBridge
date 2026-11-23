#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vb::voice::test {

enum class Kind {
    Spoke, Cancelled, Buffered, RawOnset, Packet, Fade, Complete, CacheHit,
    Abandoned, Warmed, DeviceOpened, DeviceLost, DeviceRestored, IdleStopped,
    DeviceStarted, DeviceWatchdog, HardwareQueued, RenderStarved, CacheBytes, CacheEvicted,
    WorkerStarted, StallEntered, ReleaseStalled, LifecycleEntered, LifecycleLeft
};

struct Observation {
    Kind kind;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    double value = 0;
    const std::int16_t* samples = nullptr;
    size_t count = 0;
    unsigned rate = 0;
    unsigned channels = 0;
    const wchar_t* text = nullptr;
};

// Calls come from the worker threads. A sink must copy data synchronously,
// finish promptly, and never call back into the speaker.
using Sink = void (*)(const Observation&);
extern Sink observer;
bool Flag(const char* name);
bool WaitForVoiceThreads(unsigned long milliseconds);
void ReleaseTestStalls();
void SetTestStalls(unsigned count);

struct NarratorSnapshot {
    std::wstring voice;
    int neuralSpeed = -1;
    int speechSpeed = -1;
};

// Install before creating the speaker and keep the source and its caller-owned
// auto-reset Windows event alive until all voice threads have exited. The reader
// must synchronize with snapshot changes made by the test.
struct NarratorSource {
    void* event;
    NarratorSnapshot (*read)();
};
inline const NarratorSource* narratorSource = nullptr;

inline void Observe(const Observation& observation) {
    if (observer) observer(observation);
}

}  // namespace vb::voice::test
