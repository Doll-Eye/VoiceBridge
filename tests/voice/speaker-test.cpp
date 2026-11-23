#include "../../src/common/speaker.h"
#include "crash-log.h"
#include "observer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using vb::voice::test::Kind;
using vb::voice::test::Observation;

double Milliseconds(Time time = Clock::now()) {
    return std::chrono::duration<double, std::milli>(time.time_since_epoch()).count();
}

std::string Utf8(const std::wstring& input) {
    const auto count = static_cast<int>(input.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), count, result.data(), size, nullptr, nullptr);
    return result;
}

class Log final : public vb::ILog {
public:
    void Write(vb::LogLevel level, const std::wstring& line) override {
        if (level == vb::LogLevel::Debug) return;
        std::lock_guard<std::mutex> guard(mutex_);
        lines_.push_back(Utf8(line));
    }
    vb::LogLevel Level() const override { return vb::LogLevel::Info; }

    std::string matching(const std::string& expected, size_t* count = nullptr) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::string result;
        if (count) *count = 0;
        for (const auto& line : lines_) {
            if (line.find(expected) != std::string::npos) {
                if (count) ++*count;
                if (!result.empty()) result += " | ";
                result += line;
            }
        }
        return result;
    }

    bool onlyVoice(const std::string& expected) {
        const auto normalize = [](const std::string& text) {
            std::string normalized;
            for (unsigned char ch : text) {
                if (!std::isspace(ch)) normalized += static_cast<char>(std::tolower(ch));
            }
            return normalized;
        };
        const auto name = normalize(expected);
        std::lock_guard<std::mutex> guard(mutex_);
        bool found = false;
        for (const auto& line : lines_) {
            if (line.find(". SAPI rate ") == std::string::npos) continue;
            const auto normalized = normalize(line);
            if (normalized.find("voice:" + name) != 0 && normalized.find("fallbackvoice:" + name) != 0) return false;
            found = true;
        }
        return found;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

struct Phrase {
    double spoke = 0;
    double first = 0;
    double firstLoud = 0;
    double firstPacket = 0;
    double scheduledFirst = 0;
    double scheduledLast = 0;
    double scheduledFirstLoud = 0;
    double scheduledLastLoud = 0;
    size_t frames = 0;
    unsigned rate = 0;
    unsigned channels = 0;
    size_t rawLead = 0;
    std::vector<std::int16_t> raw;
    std::vector<std::int16_t> onset;
    bool complete = false;
    bool cache = false;

    bool intact() const {
        const size_t lead = rate * 15 / 1000;
        if (rawLead != lead || onset.size() <= lead * channels || onset.size() > raw.size()) return false;
        const auto fade = rate * 3 / 1000;
        for (size_t i = 0; i < onset.size(); ++i) {
            const size_t frame = i / channels;
            const int expected = frame < fade ? raw[i] * static_cast<int>(frame) / static_cast<int>(fade) : raw[i];
            if (onset[i] != expected) return false;
        }
        return true;
    }
};

class Trace {
public:
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        phrases.clear();
        playOrder.clear();
        generation = 1;
        lastId = 0;
        warmed = stale = fades = lost = restored = opened = abandoned = stopped = 0;
        peak = capacity = badFade = fadeEnd = 0;
        cacheBytes.reset();
        fadeOpen = false;
        lastSample = 0;
    }

    void observe(const Observation& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = Milliseconds();
        switch (event.kind) {
        case Kind::Spoke:
            lastId = event.id;
            phrases[event.id].spoke = now;
            break;
        case Kind::Cancelled:
            generation = event.generation;
            break;
        case Kind::Buffered:
            peak = std::max(peak, event.value);
            return;
        case Kind::RawOnset: {
            auto& phrase = phrases[event.id];
            phrase.rawLead = static_cast<size_t>(event.value);
            phrase.raw.assign(event.samples, event.samples + event.count);
            return;
        }
        case Kind::Packet: {
            if (event.generation != generation) ++stale;
            auto& phrase = phrases[event.id];
            const size_t frames = event.count / event.channels;
            const double frameMs = 1000.0 / event.rate;
            const double start = event.value - frames * frameMs;
            if (!phrase.firstPacket) {
                phrase.firstPacket = now;
                phrase.scheduledFirst = start;
                phrase.rate = event.rate;
                phrase.channels = event.channels;
                playOrder.push_back(event.id);
            }
            const auto onsetSize = std::min(event.count, phrase.raw.size() - phrase.onset.size());
            phrase.onset.insert(phrase.onset.end(), event.samples, event.samples + onsetSize);
            for (size_t frame = 0; frame < frames; ++frame) {
                for (size_t ch = 0; ch < event.channels; ++ch) {
                    const int amplitude = std::abs(event.samples[frame * event.channels + ch]);
                    if (amplitude && !phrase.first) phrase.first = now;
                    if (amplitude > 327) {
                        if (!phrase.firstLoud) {
                            phrase.firstLoud = now;
                            phrase.scheduledFirstLoud = start + frame * frameMs;
                        }
                        phrase.scheduledLastLoud = start + frame * frameMs;
                    }
                }
            }
            phrase.frames += frames;
            phrase.scheduledLast = event.value;
            lastSample = event.samples[event.count - event.channels];
            if (fadeOpen) {
                if (lastFadeSample != 0) ++badFade;
                fadeOpen = false;
            }
            break;
        }
        case Kind::Fade:
            if (event.generation != generation) ++stale;
            if (!fadeOpen || std::abs(lastFadeSample) == 0) {
                ++fades;
                lastFadeSample = lastSample;
                fadeOpen = true;
            }
            for (size_t i = 0; i < event.count; i += event.channels) {
                if (std::abs(event.samples[i]) > std::abs(lastFadeSample)) ++badFade;
                lastFadeSample = event.samples[i];
            }
            fadeEnd = event.value;
            break;
        case Kind::Complete:
            phrases[event.id].complete = true;
            break;
        case Kind::CacheHit:
            phrases[event.id].cache = true;
            break;
        case Kind::Warmed: ++warmed; break;
        case Kind::DeviceOpened:
            ++opened;
            capacity = event.value * 1000 / event.rate;
            break;
        case Kind::DeviceLost: ++lost; break;
        case Kind::DeviceRestored: ++restored; break;
        case Kind::Abandoned: ++abandoned; break;
        case Kind::IdleStopped: ++stopped; break;
        case Kind::CacheBytes: cacheBytes = event.value; return;
        case Kind::DeviceStarted:
        case Kind::DeviceWatchdog:
        case Kind::HardwareQueued:
        case Kind::RenderStarved:
        case Kind::CacheEvicted:
        case Kind::ReleaseStalled:
        case Kind::LifecycleEntered:
        case Kind::LifecycleLeft:
        case Kind::StallEntered:
        case Kind::WorkerStarted: return;
        }
        changed_.notify_all();
    }

    template<typename Condition>
    bool wait(Condition condition, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return condition(*this); });
    }

    template<typename Function>
    auto read(Function function) {
        std::lock_guard<std::mutex> guard(mutex_);
        return function(*this);
    }

    Phrase phrase(std::uint64_t id) {
        return read([&](const Trace& trace) {
            const auto found = trace.phrases.find(id);
            return found == trace.phrases.end() ? Phrase{} : found->second;
        });
    }

    std::map<std::uint64_t, Phrase> phrases;
    std::vector<std::uint64_t> playOrder;
    std::uint64_t generation = 1;
    std::uint64_t lastId = 0;
    size_t warmed = 0, stale = 0, fades = 0, lost = 0, restored = 0, opened = 0, abandoned = 0, stopped = 0;
    double peak = 0, capacity = 0, badFade = 0, fadeEnd = 0;
    std::optional<double> cacheBytes;

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::int16_t lastFadeSample = 0;
    std::int16_t lastSample = 0;
    bool fadeOpen = false;
};

Trace& Tap() {
    // Detached recovery can still be winding down when WinMain returns.
    static auto* trace = new Trace;
    return *trace;
}

void Observe(const Observation& event) { Tap().observe(event); }

std::string Number(double number) {
    std::ostringstream result;
    result << std::fixed << std::setprecision(2) << number;
    return result.str();
}

std::string Stats(std::vector<double> runs) {
    if (runs.empty()) return "no samples";
    std::sort(runs.begin(), runs.end());
    double median = (runs[(runs.size() - 1) / 2] + runs[runs.size() / 2]) / 2;
    return std::to_string(runs.size()) + " runs, median " + Number(median) + " ms, worst " + Number(runs.back()) + " ms";
}

bool Meets(const std::vector<double>& samples, size_t count, double medianMax, double worstMax) {
    if (samples.size() != count) return false;
    auto runs = samples;
    std::sort(runs.begin(), runs.end());
    const double median = (runs[(runs.size() - 1) / 2] + runs[runs.size() / 2]) / 2;
    return median < medianMax && runs.back() < worstMax;
}

enum class SuiteVoice { Default, Jenny, David };

class VoiceModules {
public:
    bool start() {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto subscribe = reinterpret_cast<Subscribe>(reinterpret_cast<void (*)()>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification")));
        unsubscribe_ = reinterpret_cast<Unsubscribe>(reinterpret_cast<void (*)()>(
            GetProcAddress(ntdll, "LdrUnregisterDllNotification")));
        if (!subscribe || !unsubscribe_ || subscribe(0, notify, this, &cookie_) < 0) return false;

        // The notification also catches libraries that unload before a fixture ends.
        HMODULE loaded[256];
        DWORD size = 0;
        if (!EnumProcessModules(GetCurrentProcess(), loaded, sizeof(loaded), &size) || size > sizeof(loaded)) return false;
        for (size_t i = 0; i < size / sizeof(loaded[0]); ++i) {
            wchar_t name[MAX_PATH];
            const auto length = GetModuleBaseNameW(GetCurrentProcess(), loaded[i], name, MAX_PATH);
            if (!length || length >= MAX_PATH) return false;
            record(name, length);
        }
        return true;
    }

    ~VoiceModules() {
        if (cookie_) unsubscribe_(cookie_);
    }

    static constexpr unsigned Adapter = 1;
    static constexpr unsigned Onnx = 2;
    unsigned loaded() const { return modules_.load(); }

private:
    struct LoadedLibrary {
        ULONG flags;
        const UNICODE_STRING* path;
        const UNICODE_STRING* name;
        PVOID base;
        ULONG size;
    };
    using Callback = void (NTAPI*)(ULONG, const LoadedLibrary*, PVOID);
    using Subscribe = NTSTATUS (NTAPI*)(ULONG, Callback, PVOID, PVOID*);
    using Unsubscribe = NTSTATUS (NTAPI*)(PVOID);

    static void NTAPI notify(ULONG reason, const LoadedLibrary* library, PVOID context) {
        // A library unloaded while one of its threads or callbacks is still live
        // would fail in code that no longer exists, so the step log records both.
        vb::test::crash::Library(reason == 1, library->name, library->base, library->size);
        if (reason == 1 && library->name) {
            static_cast<VoiceModules*>(context)->record(library->name->Buffer, library->name->Length / sizeof(wchar_t));
        }
    }

    static bool contains(const wchar_t* name, size_t length, const wchar_t* word, size_t count) {
        for (size_t start = 0; start + count <= length; ++start) {
            size_t offset = 0;
            while (offset < count) {
                wchar_t ch = name[start + offset];
                if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
                if (ch != word[offset]) break;
                ++offset;
            }
            if (offset == count) return true;
        }
        return false;
    }

    void record(const wchar_t* name, size_t length) {
        // The loader holds its lock in the callback. Use only local code and lock-free state.
        static_assert(std::atomic<unsigned>::is_always_lock_free);
        constexpr wchar_t adapter[] = L"naturalvoicesapiadapter";
        constexpr wchar_t onnx[] = L"onnxruntime";
        if (contains(name, length, adapter, std::size(adapter) - 1)) modules_.fetch_or(Adapter);
        if (contains(name, length, onnx, std::size(onnx) - 1)) modules_.fetch_or(Onnx);
    }

    std::atomic<unsigned> modules_{0};
    Unsubscribe unsubscribe_ = nullptr;
    PVOID cookie_ = nullptr;
};

class Checks {
public:
    explicit Checks(const std::string& file, SuiteVoice voice) : file_(file), voice_(voice) {}
    void note(const std::string& line) {
        file_ << line << std::endl;
        vb::test::crash::Step("result: " + line);
    }
    void check(bool passed, const std::string& name, const std::string& detail) {
        if (!passed) ++failed_;
        note(std::string(passed ? "PASS " : "FAIL ") + name + ": " + detail);
    }
    int result() {
        note("Failures: " + std::to_string(failed_));
        return failed_ ? 1 : 0;
    }
    vb::SpeakerConfig config() const {
        vb::SpeakerConfig config;
        if (voice_ == SuiteVoice::Jenny) config.voice = L"Jenny (Natural)";
        if (voice_ == SuiteVoice::David) config.voice = L"Microsoft David";
        return config;
    }
    std::string expectedVoice() const {
        if (voice_ == SuiteVoice::Jenny) return "Microsoft Jenny (Natural)";
        if (voice_ == SuiteVoice::David) return "Microsoft David";
        return {};
    }
    bool startVoiceIsolation() {
        // Library monitoring also feeds the step log, so it runs in every mode.
        if (voice_ == SuiteVoice::Default) {
            if (!modules_.start()) note("Library load monitoring is unavailable.");
            return true;
        }
        const bool david = voice_ == SuiteVoice::David;
        const auto selected = david ? "VOICEBRIDGE_TEST_ONLY_DAVID" : "VOICEBRIDGE_TEST_ONLY_JENNY";
        const auto other = david ? "VOICEBRIDGE_TEST_ONLY_JENNY" : "VOICEBRIDGE_TEST_ONLY_DAVID";
        SetEnvironmentVariableA(other, nullptr);
        char unexpected[2];
        const bool configured = SetEnvironmentVariableA(selected, "1") &&
            GetEnvironmentVariableA(other, unexpected, sizeof(unexpected)) == 0;
        const bool monitoring = configured && modules_.start();
        check(monitoring, "voice isolation setup", expectedVoice() + " and DLL load monitoring.");
        return monitoring;
    }
    void checkVoice(Log& log, const char* fixture) {
        if (voice_ == SuiteVoice::Default) return;
        check(log.onlyVoice(expectedVoice()), std::string(fixture) + " selected voice", log.matching(". SAPI rate "));
        checkModules(fixture);
    }
    void checkModules(const std::string& stage) {
        if (voice_ == SuiteVoice::Default) return;
        const auto modules = modules_.loaded();
        const bool adapter = (modules & VoiceModules::Adapter) != 0;
        const bool onnx = (modules & VoiceModules::Onnx) != 0;
        check(voice_ == SuiteVoice::David ? !adapter && !onnx : adapter && onnx,
              stage + " DLL loads to date", std::string("Adapter: ") + (adapter ? "loaded" : "absent") +
              ", ONNX Runtime: " + (onnx ? "loaded" : "absent") + ".");
    }
private:
    std::ofstream file_;
    SuiteVoice voice_;
    VoiceModules modules_;
    int failed_ = 0;
};

void NoteResources(Checks& checks, const std::string& label) {
    const HANDLE process = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool haveMemory = GetProcessMemoryInfo(process,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    DWORD handles = 0;
    const bool haveHandles = GetProcessHandleCount(process, &handles);
    checks.note(label + ": private memory " +
                (haveMemory ? Number(memory.PrivateUsage / 1048576.0) + " MiB" : "unavailable") +
                ", handles " + (haveHandles ? std::to_string(handles) : "unavailable") + ".");
}

void NoteThreads(const std::string& label);

class Harness {
public:
    Harness(Checks& checks, vb::SpeakerConfig config, bool silent, const char* name)
        : checks_(checks), name_(name) {
        NoteThreads(std::string(name_) + ": before the speaker");
        Tap().reset();
        config.nullOutput = !silent;
        config.silentOutput = silent;
        const auto start = Milliseconds();
        speaker = vb::CreateSpeaker(config, log);
        createMs = Milliseconds() - start;
        step("speaker created");
    }
    ~Harness() {
        if (speaker && !finish(1000)) {
            // A surviving fixture could write its events into the next fixture's trace.
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(checks_.result()));
        }
        step("fixture closed");
    }
    bool finish(unsigned long timeout) {
        step("shutdown");
        speaker->Shutdown();
        step("speaker release");
        speaker.reset();
        vb::voice::test::ReleaseTestStalls();
        step("waiting for voice threads");
        const bool drained = vb::voice::test::WaitForVoiceThreads(timeout);
        if (!drained) checks_.check(false, "voice thread drain", name_);
        checks_.checkVoice(log, name_);
        NoteResources(checks_, std::string(name_) + (drained ? " after worker drain" : " after drain timeout"));
        NoteThreads(std::string(name_) + ": after the drain");
        return drained;
    }
    bool warm(Checks& checks) {
        step("waiting for two warm voices");
        const auto start = Milliseconds();
        auto ready = Tap().wait([](const Trace& t) { return t.warmed >= 2; }, 12s);
        checks.check(ready && createMs < 50, "start and warm",
                     "CreateSpeaker " + Number(createMs) + " ms, two warm voices after " +
                     Number(Milliseconds() - start) + " ms. " + log.matching("SAPI rate") + log.matching("SAPI failed"));
        if (ready) checks.checkModules(std::string(name_) + " warm-up");
        return ready;
    }
    std::uint64_t say(const std::wstring& text) {
        speaker->Speak(text);
        const auto id = Tap().read([](const Trace& t) { return t.lastId; });
        step("said phrase " + std::to_string(id) + " of " + std::to_string(text.size()) + " characters");
        return id;
    }
    bool quiet(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (speaker->IsSpeaking() && Clock::now() < deadline) Sleep(1);
        return !speaker->IsSpeaking();
    }
    bool hear(std::uint64_t id, std::chrono::milliseconds timeout = 1s) {
        const bool heard = Tap().wait([&](const Trace& t) { return t.phrases.at(id).firstLoud != 0; }, timeout);
        step("phrase " + std::to_string(id) + (heard ? " heard" : " not heard"));
        return heard;
    }
    void step(const std::string& text) const { vb::test::crash::Step(std::string(name_) + ": " + text); }
    Log log;
    std::unique_ptr<vb::ISpeaker> speaker;
    double createMs = 0;

private:
    Checks& checks_;
    const char* name_;
};

const std::wstring Sentence = L"The blacksmith opened the wooden door and put the sword on the wooden table. "
    L"Outside, rain ran down the windows as the final riders crossed the bridge, carrying a message for the guard "
    L"who waited by the gate until the sun came up.";
const std::wstring Long = Sentence + L" " + Sentence + L" " + Sentence;

void LatencyAndOrder(Checks& checks, bool silent) {
    auto config = checks.config();
    config.followNarrator = false;
    config.cacheMegabytes = 0;
    config.keepAwakeSeconds = 0;
    Harness h(checks, config, silent, "Latency and order");
    // The first phrase enters the queue while SAPI is still starting.
    h.speaker->Speak(L"  \t\r\n\u2003");
    const bool ignoredWhiteSpace = !h.speaker->IsSpeaking();
    const auto early = h.say(L"Inventory");
    const bool immediatelySpeaking = h.speaker->IsSpeaking();
    if (!h.warm(checks)) return;
    if (silent) {
        Tap().wait([](const Trace& t) { return t.opened > 0; }, 3s);
        const auto endpoints = Tap().read([](const Trace& t) { return t.opened; });
        checks.check(endpoints > 0, "WASAPI endpoint", endpoints ? h.log.matching("Audio device:") +
                     " WASAPI capacity " + Number(Tap().read([](const Trace& t) { return t.capacity; })) + " ms."
                     : "No WASAPI device was opened. " + h.log.matching("Audio device unavailable"));
        if (!endpoints) return;
    }
    checks.check(ignoredWhiteSpace && immediatelySpeaking && h.hear(early), "startup queue and whitespace",
                 "A phrase queued before startup played. Whitespace did not make the speaker busy.");
    h.speaker->Cancel();
    h.quiet(1s);

    size_t intact = 0;
    for (const auto& text : {std::wstring(L"Inventory"), std::wstring(L"Open the map"), Sentence}) {
        std::vector<double> times;
        std::vector<double> loudTimes;
        size_t badLead = 0;
        size_t badSamples = 0;
        size_t shortestLead = std::numeric_limits<size_t>::max();
        for (int run = 0; run < 20; ++run) {
            const auto id = h.say(text);
            if (h.hear(id)) {
                const auto phrase = Tap().phrase(id);
                times.push_back(phrase.first - phrase.spoke);
                loudTimes.push_back(phrase.firstLoud - phrase.spoke);
                if (phrase.intact()) ++intact;
                else if (phrase.rawLead != phrase.rate * 15 / 1000) {
                    ++badLead;
                    shortestLead = std::min(shortestLead, phrase.rawLead);
                } else {
                    ++badSamples;
                }
            }
            h.speaker->Cancel();
            h.quiet(250ms);
        }
        auto name = text == Sentence ? std::to_string(text.size()) + "-character sentence" : Utf8(text);
        checks.check(Meets(times, 20, 40, 80), "speak to first nonzero sample for " + name,
                     Stats(times) + ". Above 1 percent: " + Stats(loudTimes) + ".");
        if (badLead || badSamples) checks.note("Onset differences for " + name +
            ": short raw leads " + std::to_string(badLead) +
            (badLead ? " (shortest " + std::to_string(shortestLead) + " frames)" : "") +
            ", sample mismatches " + std::to_string(badSamples) + ".");
    }

    std::vector<double> interrupt;
    std::vector<double> cancelQuiet;
    std::vector<double> cancelFade;
    for (int run = 0; run < 20; ++run) {
        const auto old = h.say(Long);
        if (!h.hear(old)) break;
        Sleep(30);
        h.speaker->Cancel();
        const auto next = h.say(L"Inventory");
        if (h.hear(next)) {
            const auto phrase = Tap().phrase(next);
            interrupt.push_back(phrase.first - phrase.spoke);
        }
        const auto cancelled = Milliseconds();
        h.speaker->Cancel();
        if (h.quiet(150ms)) cancelQuiet.push_back(Milliseconds() - cancelled);
        const auto fade = Tap().read([](const Trace& t) { return t.fadeEnd; });
        if (fade >= cancelled) cancelFade.push_back(fade - cancelled);
    }
    const auto staleAfterInterrupt = Tap().read([](const Trace& t) { return t.stale; });
    checks.check(Meets(interrupt, 20, 40, std::numeric_limits<double>::infinity()) && staleAfterInterrupt == 0, "interrupt",
                 Stats(interrupt) + ". Stale packets: " + std::to_string(staleAfterInterrupt) + ".");

    // Use a high-resolution timer for the 30 ms request cadence without
    // changing Windows' global timer resolution.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    std::vector<double> intervals;
    auto prior = Clock::now();
    for (int run = 0; run < 50; ++run) {
        h.speaker->Cancel();
        h.say(run % 2 ? L"Open the map" : L"Inventory");
        const auto now = Clock::now();
        if (run) intervals.push_back(std::chrono::duration<double, std::milli>(now - prior).count());
        prior = now;
        LARGE_INTEGER due;
        due.QuadPart = -300000;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
        else Sleep(30);
    }
    if (timer) CloseHandle(timer);
    h.speaker->Cancel();
    const auto last = h.say(L"Quest complete.");
    const bool finalSpoke = h.hear(last) && h.quiet(3s);
    const auto final = Tap().phrase(last);
    const auto faults = Tap().read([](const Trace& t) { return t.stale + t.badFade; });
    checks.check(finalSpoke && final.complete && faults == 0, "rapid interruption",
                 "50 pairs at " + Stats(intervals) + " between requests. Final phrase " +
                 Number(final.rate ? final.frames * 1000.0 / final.rate : 0) +
                 " ms, stale packets or broken fades " + Number(faults) + ".");

    const std::wstring texts[] = {L"One", L"Two", L"Three", L"Four", L"Five", L"Six", L"Seven", L"Eight", L"Nine", L"Ten"};
    std::vector<std::uint64_t> ids;
    for (const auto& text : texts) ids.push_back(h.say(text));
    const bool finished = h.quiet(15s);
    double detectedEnd = Milliseconds();
    const auto actualOrder = Tap().read([&](const Trace& t) {
        std::vector<std::uint64_t> result;
        for (auto id : t.playOrder) if (id >= ids.front()) result.push_back(id);
        return result;
    });
    std::vector<double> gaps;
    std::vector<double> silentGaps;
    double previousEnd = 0;
    double previousLoud = 0;
    for (auto id : ids) {
        auto phrase = Tap().phrase(id);
        intact += phrase.intact();
        if (previousEnd && phrase.scheduledFirst) gaps.push_back(std::max(0.0, phrase.scheduledFirst - previousEnd));
        if (previousLoud && phrase.scheduledFirstLoud) silentGaps.push_back(phrase.scheduledFirstLoud - previousLoud);
        previousEnd = phrase.scheduledLast;
        previousLoud = phrase.scheduledLastLoud;
    }
    checks.check(finished && actualOrder == ids, "queued order", Stats(gaps) +
                 " between packets for adjacent phrases. Above-threshold gaps: " + Stats(silentGaps) + ".");
    checks.check(intact == 70, "onset samples", std::to_string(intact) +
                 " of 70 phrases preserved 15 ms and matched the 3 ms fade sample for sample.");
    const auto lastDelay = detectedEnd - Tap().phrase(ids.back()).scheduledLast;
    const auto maximumCancel = cancelQuiet.empty() ? 0 : *std::max_element(cancelQuiet.begin(), cancelQuiet.end());
    checks.check(immediatelySpeaking && finished && lastDelay < 50 && cancelQuiet.size() == 20 && maximumCancel < 50,
                 "IsSpeaking", "Final drain detection " + Number(lastDelay) + " ms. Cancel to false: " +
                 Stats(cancelQuiet) + ". Scheduled end of fades: " + Stats(cancelFade) + ".");

    std::wstring giant;
    while (giant.size() < 20000) giant += Sentence + L" ";
    giant.resize(20000);
    const auto large = h.say(giant);
    const bool started = h.hear(large);
    const auto fillDeadline = Clock::now() + 12s;
    double peak = 0;
    while (Clock::now() < fillDeadline) {
        peak = Tap().read([](const Trace& t) { return t.peak; });
        if (peak >= 29990 || !h.speaker->IsSpeaking()) break;
        Sleep(5);
    }
    const auto cancelled = Milliseconds();
    h.speaker->Cancel();
    const bool didStop = h.quiet(150ms);
    const double stop = Milliseconds() - cancelled;
    checks.check(started && peak <= 30000.01 && peak >= 29000 && didStop, "20,000-character buffer",
                 "Peak " + Number(peak) + " ms, Cancel to false " + Number(stop) + " ms.");
    checks.note("Fade packets formed " + std::to_string(Tap().read([](const Trace& t) { return t.fades; })) +
                " fade observations over the run.");
    if (silent) checks.note("WASAPI stops: " +
                           std::to_string(Tap().read([](const Trace& t) { return t.stopped; })) + ".");
    const bool speakingAtShutdown = h.hear(h.say(Long));
    const auto shutdown = Milliseconds();
    h.speaker->Shutdown();
    const double shutdownTime = Milliseconds() - shutdown;
    checks.check(speakingAtShutdown && !h.speaker->IsSpeaking() && shutdownTime < 100,
                 "shutdown during speech", Number(shutdownTime) + " ms.");
}

void Cached(Checks& checks, bool silent) {
    auto config = checks.config();
    config.followNarrator = false;
    config.keepAwakeSeconds = 0;
    Harness h(checks, config, silent, "Phrase cache");
    if (!h.warm(checks)) return;
    const auto first = h.say(L"Inventory");
    if (!h.hear(first) || !h.quiet(3s)) {
        checks.check(false, "cache prime", "The first phrase did not play through.");
        return;
    }
    std::vector<double> hits;
    for (int run = 0; run < 20; ++run) {
        const auto hit = h.say(L"Inventory");
        if (h.hear(hit)) {
            const auto phrase = Tap().phrase(hit);
            if (phrase.cache) hits.push_back(phrase.first - phrase.spoke);
        }
        h.speaker->Cancel();
        h.quiet(100ms);
    }
    checks.check(Meets(hits, 20, 5, 5), "cache hit", Stats(hits) + ".");

    const auto cancelled = h.say(L"Open the map");
    const bool synthesised = Tap().wait([&](const Trace& t) { return t.phrases.at(cancelled).complete; }, 1s);
    h.speaker->Cancel();
    h.quiet(150ms);
    const auto retry = h.say(L"Open the map");
    const bool spoke = h.hear(retry) && h.quiet(3s);
    checks.check(synthesised && spoke && !Tap().phrase(retry).cache, "cancelled cache entry",
                 "Cancelling after synthesis but during playback did not prime the cache.");
}

void VoiceAndRecovery(Checks& checks, bool silent) {
    auto config = checks.config();
    const auto preferred = config.voice;
    config.voice = L"NoSuchVoice";
    config.fallbackVoice = checks.expectedVoice().empty() ? L"David" : preferred;
    config.followNarrator = false;
    config.cacheMegabytes = 0;
    {
        Harness h(checks, config, silent, "Voice fallback");
        size_t count = 0;
        const bool works = h.hear(h.say(L"Fallback test.")) && h.quiet(3s);
        const auto line = h.log.matching("Fallback voice:", &count);
        checks.check(works && count == 1, "voice fallback", line);
    }
    config.voice = preferred;
    config.fallbackVoice.clear();
    {
        SetEnvironmentVariableA("VOICEBRIDGE_TEST_HANG", "1");
        Harness h(checks, config, silent, "Hung voice replacement");
        if (h.warm(checks)) {
            const auto id = h.say(L"Recovered speech.");
            const bool spoken = h.hear(id, 5s);
            const bool replaced = Tap().wait([](const Trace& t) { return t.warmed >= 3 && t.abandoned == 1; }, 5s);
            const auto phrase = Tap().phrase(id);
            checks.check(spoken && replaced && h.quiet(3s), "hung voice replacement",
                         "First sample " + Number(phrase.first - phrase.spoke) + " ms after request. " +
                         h.log.matching("abandoned"));
        }
        SetEnvironmentVariableA("VOICEBRIDGE_TEST_HANG", nullptr);
    }
    {
        SetEnvironmentVariableA("VOICEBRIDGE_TEST_DEVICE_LOSS", "1");
        Harness h(checks, config, silent, "Device recovery");
        if (h.warm(checks)) {
            const auto id = h.say(L"The map is ready to explore.");
            const bool works = h.hear(id) && h.quiet(6s);
            const auto phrase = Tap().phrase(id);
            const bool events = Tap().read([&](const Trace& t) {
                return t.lost == 1 && t.restored == 1 && (!silent || t.opened >= 2);
            });
            checks.check(works && events && phrase.complete, "device recovery",
                         std::to_string(phrase.frames) + " frames reached the output across the injected loss. " +
                         h.log.matching("Audio device:"));
        }
        SetEnvironmentVariableA("VOICEBRIDGE_TEST_DEVICE_LOSS", nullptr);
    }
}

ULONGLONG FileTicks(const FILETIME& time) {
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) + time.dwLowDateTime;
}

struct CpuTime {
    ULONGLONG kernel = 0;
    ULONGLONG user = 0;
};

std::optional<CpuTime> ProcessCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return std::nullopt;
    return CpuTime{FileTicks(kernel), FileTicks(user)};
}

struct ThreadSample {
    DWORD id;
    ULONGLONG created;
    CpuTime cpu;
    std::optional<ULONGLONG> cycles;
    std::string label;
    bool voiceBridge;
};

std::string ThreadLabel(HANDLE thread, DWORD id, bool& voiceBridge) {
    std::string label = id == GetCurrentThreadId() ? "Test harness" : "";
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description))) {
        if (description && *description) label = Utf8(description);
        LocalFree(description);
    }
    voiceBridge = label == "VoiceBridge audio" || label == "VoiceBridge scheduler" || label == "VoiceBridge SAPI";

    using Query = decltype(&NtQueryInformationThread);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    PVOID address = nullptr;
    HMODULE module = nullptr;
    std::string name = "module unavailable";
    if (query && query(thread, ThreadQuerySetWin32StartAddress, &address, sizeof(address), nullptr) >= 0 &&
        address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &module)) {
        wchar_t path[MAX_PATH];
        const auto length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            std::wstring fullPath(path, length);
            name = Utf8(fullPath.substr(fullPath.find_last_of(L"\\/") + 1));
        }
    }
    return label.empty() ? name : label + " (" + name + ")";
}

std::optional<std::vector<ThreadSample>> SampleThreads() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) {
        CloseHandle(snapshot);
        return std::nullopt;
    }
    std::vector<ThreadSample> samples;
    const auto processId = GetCurrentProcessId();
    do {
        if (entry.th32OwnerProcessID != processId) continue;
        const auto id = entry.th32ThreadID;
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessIdOfThread(thread) == processId && GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            ULONGLONG cycles = 0;
            const bool haveCycles = QueryThreadCycleTime(thread, &cycles);
            bool voiceBridge = false;
            auto label = ThreadLabel(thread, id, voiceBridge);
            samples.push_back({id, FileTicks(created), {FileTicks(kernel), FileTicks(user)},
                               haveCycles ? std::optional<ULONGLONG>{cycles} : std::nullopt, std::move(label), voiceBridge});
        }
        CloseHandle(thread);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return samples;
}

// Thread counts by name and entry module, for the step log.
void NoteThreads(const std::string& label) {
    const auto threads = SampleThreads();
    if (!threads) {
        vb::test::crash::Step(label + ": thread snapshot unavailable");
        return;
    }
    std::map<std::string, unsigned> counts;
    for (const auto& thread : *threads) ++counts[thread.label];
    std::string line = label + ": " + std::to_string(threads->size()) + " threads";
    for (const auto& [name, count] : counts) line += "; " + name + " x" + std::to_string(count);
    vb::test::crash::Step(line);
}

std::optional<CpuTime> NoteIdleThreads(Checks& checks, const std::vector<ThreadSample>& before,
                                      const std::vector<ThreadSample>& after) {
    auto find = [](const std::vector<ThreadSample>& samples, const ThreadSample& thread) {
        return std::find_if(samples.begin(), samples.end(), [&](const ThreadSample& sample) {
            return sample.id == thread.id && sample.created == thread.created;
        });
    };
    checks.note("Idle thread snapshots: " + std::to_string(before.size()) + " start, " +
                std::to_string(after.size()) + " end. Modules identify thread entry points. " +
                "Harness thread counts include endpoint sampling.");
    CpuTime voiceBridge{};
    unsigned matched = 0;
    bool changed = false;
    for (const auto& thread : before) {
        const auto end = find(after, thread);
        const std::string label = "Idle thread " + std::to_string(thread.id) + " " + thread.label;
        if (end == after.end()) {
            checks.note(label + ": only seen at start.");
            changed |= thread.voiceBridge;
            continue;
        }
        const auto kernel = end->cpu.kernel - thread.cpu.kernel;
        const auto user = end->cpu.user - thread.cpu.user;
        if (thread.voiceBridge && end->voiceBridge) {
            voiceBridge.kernel += kernel;
            voiceBridge.user += user;
            ++matched;
        }
        const auto cycles = thread.cycles && end->cycles ? std::to_string(*end->cycles - *thread.cycles) : "unavailable";
        checks.note(label + ": kernel " + Number(kernel / 10000.0) +
                    " ms, user " + Number(user / 10000.0) + " ms, cycles " + cycles + ".");
    }
    for (const auto& thread : after) {
        if (find(before, thread) == before.end()) {
            checks.note("Idle thread " + std::to_string(thread.id) + " " + thread.label + ": only seen at end.");
            changed |= thread.voiceBridge;
        }
    }
    if (changed || matched != 4) return std::nullopt;
    return voiceBridge;
}

std::string CpuDetail(const CpuTime& cpu, double elapsed) {
    const double kernel = cpu.kernel / 10000.0;
    const double user = cpu.user / 10000.0;
    return Number(kernel + user) + " ms in " + Number(elapsed) + " ms elapsed. Kernel " + Number(kernel) +
        " ms, user " + Number(user) + " ms.";
}

void NarratorAndCost(Checks& checks, bool silent, bool cost) {
    auto config = checks.config();
    config.keepAwakeSeconds = 0;
    Harness h(checks, config, silent, "Narrator and cost");
    if (!h.warm(checks)) return;
    const bool works = h.hear(h.say(L"Narrator settings.")) && h.quiet(3s);
    const auto line = h.log.matching("SAPI rate");
    checks.check(works && !line.empty(), "follow Narrator", line);
    if (!cost || !works) return;
    if (silent) {
        const bool opened = Tap().read([](const Trace& t) { return t.opened > 0; });
        checks.check(opened, "idle cost with WASAPI", opened ? h.log.matching("Audio device:")
                     : h.log.matching("Audio device unavailable"));
    }
    NoteResources(checks, "Idle start after warming and playing one phrase");
    const auto beforeThreads = SampleThreads();
    const auto beforeCpu = ProcessCpu();
    const auto start = Milliseconds();
    Sleep(60000);
    const auto elapsed = Milliseconds() - start;
    const auto afterCpu = ProcessCpu();
    const auto afterThreads = SampleThreads();
    NoteResources(checks, "Idle end");
    const auto cacheBytes = Tap().read([](const Trace& t) { return t.cacheBytes; });
    if (cacheBytes) checks.note("Idle end cache storage: " + Number(*cacheBytes / 1048576.0) + " MiB.");
    if (beforeCpu && afterCpu) {
        checks.note("Idle process CPU: " + CpuDetail({afterCpu->kernel - beforeCpu->kernel,
                                                     afterCpu->user - beforeCpu->user}, elapsed));
    } else {
        checks.check(false, "idle process CPU measurement", "Process CPU timing was unavailable.");
    }
    const auto ownCpu = beforeThreads && afterThreads ? NoteIdleThreads(checks, *beforeThreads, *afterThreads) : std::nullopt;
    checks.check(ownCpu && ownCpu->kernel + ownCpu->user < 1000000, "idle VoiceBridge CPU",
                 ownCpu ? CpuDetail(*ownCpu, elapsed) + " Measured across the four named VoiceBridge threads."
                        : "The four named VoiceBridge threads could not all be measured for the full interval.");
    const auto shutdown = Milliseconds();
    h.speaker->Shutdown();
    const double shutdownTime = Milliseconds() - shutdown;
    checks.check(shutdownTime < 100, "shutdown", Number(shutdownTime) + " ms.");
}

bool NormalExit(Checks& checks) {
    vb::SpeakerConfig config;
    config.followNarrator = false;
    config.voice = L"Jenny (Natural)";
    Harness h(checks, config, false, "Normal exit");
    bool played = false;
    if (h.warm(checks)) {
        const auto id = h.say(L"Normal exit test.");
        played = h.hear(id) && h.quiet(3s) && Tap().phrase(id).complete;
    }
    const auto line = h.log.matching("SAPI rate");
    const bool actualJenny = line.find("Voice: Microsoft Jenny (Natural)") == 0;
    checks.check(played && actualJenny, "natural Jenny playback before normal exit", line);
    const bool drained = h.finish(5000);
    if (played && actualJenny && drained) {
        checks.note("Voice threads exited. Returning normally from WinMain after closing this report.");
        return true;
    }
    checks.note("Forced process exit because the normal-exit prerequisites failed.");
    return false;
}

int RunTests(std::string mode, bool& normalReturn) {
    const auto delimiter = mode.find_first_of(" \t");
    if (delimiter != std::string::npos) mode.resize(delimiter);
    const bool silent = mode == "silent" || mode == "device" || mode == "silent-cost";
    const auto voice = mode == "null-jenny" ? SuiteVoice::Jenny : mode == "null-david" ? SuiteVoice::David : SuiteVoice::Default;
    vb::test::crash::Start(mode + "-steps.log");
    Checks checks(mode + "-results.txt", voice);
    if (!checks.startVoiceIsolation()) return checks.result();
    NoteResources(checks, "Process baseline before creating a speaker");
    if (mode == "device") {
        vb::SpeakerConfig config;
        Harness h(checks, config, true, "Device probe");
        Tap().wait([](const Trace& t) { return t.opened > 0; }, 3s);
        const bool opened = Tap().read([](const Trace& t) { return t.opened > 0; });
        checks.note(opened ? h.log.matching("Audio device:") : h.log.matching("Audio device unavailable"));
        return opened ? 0 : 2;
    }
    if (mode == "normal-exit") {
        normalReturn = NormalExit(checks);
    } else if (mode == "cost" || mode == "silent-cost") {
        NarratorAndCost(checks, silent, true);
    } else if (mode == "playback") {
        LatencyAndOrder(checks, false);
        Cached(checks, false);
        VoiceAndRecovery(checks, false);
        NarratorAndCost(checks, false, false);
    } else if (mode == "null" || silent || voice != SuiteVoice::Default) {
        LatencyAndOrder(checks, silent);
        if (silent && !Tap().read([](const Trace& t) { return t.opened != 0; })) return 2;
        Cached(checks, silent);
        VoiceAndRecovery(checks, silent);
        NarratorAndCost(checks, silent, true);
    } else {
        checks.check(false, "mode", "Use null, null-jenny, null-david, silent, device, playback, cost, silent-cost or normal-exit.");
    }
    return checks.result();
}


}  // namespace

namespace vb::voice::test {
Sink observer = ::Observe;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR args, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    bool normalReturn = false;
    const auto code = RunTests(args, normalReturn);
    vb::test::crash::Step("Tests finished with exit code " + std::to_string(code) + ".");
    if (normalReturn) return code;
    // Match the VoiceBridge host: after Shutdown and closing the result file,
    // Windows can end the process without running an abandoned engine's DLL cleanup.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
