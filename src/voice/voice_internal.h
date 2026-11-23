#pragma once

#include "../common/server_speaker.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <unknwn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef VB_VOICE_TEST
#include "../../tests/voice/observer.h"
#endif

namespace vb::voice {

using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;
using Sample = std::int16_t;
using Samples = std::vector<Sample>;

constexpr double BufferSeconds = 30;
constexpr int Loud = 327;

inline DWORD WaitTime(Time deadline) {
    if (deadline == Time::max()) return INFINITE;
    auto delay = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    if (delay <= 0ms) return 0;
    if (delay.count() >= INFINITE) return INFINITE - 1;
    return static_cast<DWORD>(delay.count());
}

class Event {
public:
    explicit Event(bool manual = false) : handle_(CreateEventW(nullptr, manual, FALSE, nullptr)) {}
    ~Event() { if (handle_) CloseHandle(handle_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    HANDLE get() const { return handle_; }
    void signal() const { SetEvent(handle_); }
private:
    HANDLE handle_;
};

template<class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void reset() { if (auto* ptr = std::exchange(ptr_, nullptr)) ptr->Release(); }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T** out() { reset(); return &ptr_; }
    void** voidOut() { return reinterpret_cast<void**>(out()); }
private:
    T* ptr_ = nullptr;
};

struct ComApartment {
    explicit ComApartment(DWORD flags) : result(CoInitializeEx(nullptr, flags)) {}
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};

struct Format {
    unsigned rate = 22050;
    unsigned channels = 1;
    bool operator==(const Format& other) const { return rate == other.rate && channels == other.channels; }
    bool operator!=(const Format& other) const { return !(*this == other); }
    size_t frames(unsigned milliseconds) const { return rate * milliseconds / 1000; }
    double seconds(size_t frames) const { return static_cast<double>(frames) / rate; }
    WAVEFORMATEX wave() const {
        auto align = static_cast<WORD>(channels * sizeof(Sample));
        return {WAVE_FORMAT_PCM, static_cast<WORD>(channels), rate, rate * align, align, 16, 0};
    }
};

struct Settings {
    std::wstring narratorVoice;
    int neuralSpeed = -1;
    int speechSpeed = -1;
    bool operator==(const Settings& other) const {
        return narratorVoice == other.narratorVoice && neuralSpeed == other.neuralSpeed &&
               speechSpeed == other.speechSpeed;
    }
};

struct Choice {
    std::wstring id;
    std::wstring name;
    Format format;
    int rate = 0;
    bool fallback = false;
    // Lower priorities win when workers have found different usable voices.
    int priority = 4;
};

struct Job {
    Job(std::uint64_t number, std::uint64_t generation, std::wstring text)
        : id(number), generation(generation), text(std::move(text)) {}
    const std::uint64_t id;
    const std::uint64_t generation;
    const std::wstring text;
    std::map<std::wstring, unsigned> failedTokens;
    unsigned timeouts = 0;
    std::uint64_t journalCursor = 0;
    bool recoveredFromJournal = false;
    bool published = false;  // At least one chunk was queued, even if it has since drained.
    size_t bufferedFrames = 0;  // Protected by State::mutex.
    Time submittedUntil{};
};

struct Chunk {
    std::shared_ptr<const Samples> pcm;
    size_t offset = 0;  // In frames, as is count.
    size_t count = 0;
    Format format;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<Job> job;
};

struct State {
    State(const SpeakerConfig& config, ILog& log, SpeechJournal* journal, HANDLE exhausted)
        : config(config), journal(journal), exhausted(exhausted), log(&log) {}
    const SpeakerConfig config;
    SpeechJournal* const journal;
    const HANDLE exhausted;
    void beginReplay(Job& job);  // Caller holds mutex.
    void retire(Job& job);  // Caller holds mutex.

    // The command queue, audio queue and publication of a device packet share a
    // lock. No waits, voice calls or device setup can hold it.
    std::mutex mutex;
    std::condition_variable space;
    Event dispatch;
    Event render;
    Event audioDone{true};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> generation{1};
    std::uint64_t lastJob = 0;
    std::list<std::shared_ptr<Job>> jobs;
    std::list<std::shared_ptr<Job>> retiredJobs;
    std::list<Chunk> audio;
    std::list<Chunk> retiredAudio;
    std::shared_ptr<Job> active;
    std::optional<Format> outputFormat;
    double waiting = 0;
    Time audibleUntil{};

    void report(LogLevel level, const std::wstring& line, const wchar_t* once = nullptr);
    void disconnectLog();
    void queueChunk(Chunk chunk);  // Caller holds mutex.
    void consume(size_t frames);   // Caller holds mutex.
private:
    std::mutex logMutex;
    ILog* log;
    std::set<std::wstring> logged;
};

void StartVoiceThread(const wchar_t* name, std::function<void()> run);
void RunSynthesis(const std::shared_ptr<State>& state);
void RunAudio(const std::shared_ptr<State>& state);

inline std::wstring ErrorCode(HRESULT hr) {
    wchar_t value[12];
    swprintf(value, sizeof(value) / sizeof(*value), L"0x%08lX", static_cast<unsigned long>(hr));
    return value;
}

}  // namespace vb::voice
