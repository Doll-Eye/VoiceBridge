#pragma once

#include <windows.h>
#include <string>

namespace restart_test {

constexpr wchar_t MappingVariable[] = L"VOICEBRIDGE_TEST_RESTART_MAPPING";
constexpr DWORD CrashCode = 0xE0425642;  // Customer exception reserved for this fixture.
constexpr LONG OnsetCapacity = 8;

enum class Scenario : LONG { Stalled = 1, Crash, Poison, SlowProbe, RecurringStall, HealthyRun };
enum class Phrase : LONG { Unknown, Cancelled, Interrupted, FirstQueued, SecondQueued, Later, Poison, BeforeReset };

inline constexpr wchar_t Cancelled[] = L"This cancelled marker must stay silent.";
inline constexpr wchar_t FirstQueued[] = L"The first queued request.";
inline constexpr wchar_t SecondQueued[] = L"The second queued request.";
inline constexpr wchar_t Later[] = L"A new request after recovery.";
inline constexpr wchar_t Poison[] = L"This phrase stalls each voice that tries it.";
inline constexpr wchar_t BeforeReset[] = L"This request is pending during the second failure.";

inline const std::wstring& InterruptedText() {
    static const auto text = [] {
        std::wstring result;
        for (unsigned i = 0; i < 16; ++i) {
            result += L"This long uninterrupted speech must stop when the worker crashes. ";
        }
        return result;
    }();
    return text;
}

struct Onset {
    Phrase phrase;
    LONG64 tick;
};

struct Worker {
    volatile LONG pid;
    LONG64 startTick;
    LONG64 speakerTick;
    volatile LONG lifecycleCount;
    volatile LONG warmCount;
    volatile LONG stallCount;
    volatile LONG64 firstStall;
    volatile LONG crashRequested;
    volatile LONG64 crashTick;
    volatile LONG onsetCount;
    Onset onsets[OnsetCapacity];
};

struct Trace {
    Scenario scenario;
    volatile LONG nextWorker;
    Worker workers[3];
};

inline LONG Read(volatile LONG& value) {
    return InterlockedCompareExchange(&value, 0, 0);
}

inline LONG64 Read(volatile LONG64& value) {
    return InterlockedCompareExchange64(&value, 0, 0);
}

inline LONG64 Tick() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

}  // namespace restart_test
