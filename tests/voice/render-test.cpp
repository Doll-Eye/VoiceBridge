#include "../../src/common/speaker.h"
#include "observer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using vb::voice::test::Kind;
using vb::voice::test::Observation;

std::string Number(double value) {
    std::ostringstream result;
    result << std::fixed << std::setprecision(2) << value;
    return result.str();
}

std::string Utf8(const std::wstring& input) {
    const auto length = static_cast<int>(input.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, input.data(), length, nullptr, 0, nullptr, nullptr);
    std::string output(bytes, '\0');
    if (bytes) WideCharToMultiByte(CP_UTF8, 0, input.data(), length, output.data(), bytes, nullptr, nullptr);
    return output;
}

class Report {
public:
    void note(const std::string& line) { out_ << line << std::endl; }
    void check(bool passed, const std::string& name, const std::string& detail) {
        if (!passed) ++failed_;
        note(std::string(passed ? "PASS " : "FAIL ") + name + ": " + detail);
    }
    int result() {
        note("Failures: " + std::to_string(failed_));
        return failed_ ? 1 : 0;
    }

private:
    std::ofstream out_{"render-results.txt"};
    int failed_ = 0;
};

class Log final : public vb::ILog {
public:
    void Write(vb::LogLevel level, const std::wstring& line) override {
        if (level == vb::LogLevel::Debug) return;
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back({level, line});
    }
    vb::LogLevel Level() const override { return vb::LogLevel::Info; }
    std::vector<std::wstring> matching(const std::wstring& part, vb::LogLevel minimum = vb::LogLevel::Error) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::wstring> result;
        for (const auto& line : lines_) {
            if (line.level >= minimum && line.message.find(part) != std::wstring::npos) result.push_back(line.message);
        }
        return result;
    }

private:
    struct Line {
        vb::LogLevel level;
        std::wstring message;
    };
    std::mutex mutex_;
    std::vector<Line> lines_;
};

struct Format {
    unsigned rate = 0;
    unsigned channels = 0;
    bool operator==(const Format& other) const { return rate == other.rate && channels == other.channels; }
};

struct Start {
    std::uint64_t phrase;
    double primedMs;
    double delayMs;
};

struct Progress {
    unsigned warmed = 0;
    unsigned stops = 0;
    unsigned lost = 0;
    unsigned restored = 0;
    unsigned watchdogs = 0;
    unsigned emptyPadding = 0;
    unsigned submissions = 0;
    unsigned submissionsAfterWatchdog = 0;
    unsigned submissionsAfterRecovery = 0;
    std::uint64_t phrase = 0;
    std::uint64_t completed = 0;
    std::uint64_t lastPacket = 0;
    Format packetFormat;
    Clock::time_point firstPacket;
    double peakPaddingMs = 0;
    std::vector<Format> openings;
    std::vector<Start> starts;
};

class Trace {
public:
    void observe(const Observation& event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            switch (event.kind) {
            case Kind::Warmed: ++progress_.warmed; break;
            case Kind::DeviceOpened: progress_.openings.push_back({event.rate, event.channels}); break;
            case Kind::IdleStopped: ++progress_.stops; break;
            case Kind::DeviceLost: ++progress_.lost; break;
            case Kind::DeviceRestored: ++progress_.restored; break;
            case Kind::DeviceWatchdog: ++progress_.watchdogs; break;
            case Kind::RenderStarved: ++progress_.emptyPadding; break;
            case Kind::Spoke:
                progress_.phrase = event.id;
                progress_.firstPacket = {};
                break;
            case Kind::Packet:
                if (progress_.firstPacket == Clock::time_point{}) progress_.firstPacket = Clock::now();
                progress_.lastPacket = event.id;
                progress_.packetFormat = {event.rate, event.channels};
                break;
            case Kind::Complete: progress_.completed = event.id; break;
            case Kind::HardwareQueued:
                ++progress_.submissions;
                if (progress_.watchdogs) ++progress_.submissionsAfterWatchdog;
                if (progress_.restored && progress_.openings.size() >= 2 && progress_.starts.size() >= 2)
                    ++progress_.submissionsAfterRecovery;
                progress_.peakPaddingMs = std::max(progress_.peakPaddingMs, event.value);
                break;
            case Kind::DeviceStarted: {
                const auto delay = progress_.firstPacket == Clock::time_point{} ? 0.0 :
                    std::chrono::duration<double, std::milli>(Clock::now() - progress_.firstPacket).count();
                progress_.starts.push_back({progress_.lastPacket, event.rate ? event.value * 1000 / event.rate : 0, delay});
                break;
            }
            default: return;
            }
        }
        changed_.notify_all();
    }

    template<class Predicate>
    bool wait(Predicate predicate, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(progress_); });
    }

    Progress snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return progress_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    Progress progress_;
};

// The sink stays fixed. Each fixture sets the target before starting any
// threads and clears it only after the actual thread handles have signaled.
Trace* activeTrace = nullptr;
void Observe(const Observation& event) { activeTrace->observe(event); }

enum class Availability { Ready, NoDevice, Failed };

class Fixture {
public:
    Fixture(Report& report, const char* flag, const wchar_t* fallback = L"")
        : report_(report), flag_(flag) {
        if (!SetEnvironmentVariableA(flag_, "1")) {
            report_.check(false, "test setup", std::string("Could not set ") + flag_ + ".");
            return;
        }
        vb::SpeakerConfig config;
        config.followNarrator = false;
        config.fallbackVoice = fallback;
        config.keepAwakeSeconds = 0;
        config.cacheMegabytes = 0;
        config.silentOutput = true;
        activeTrace = &trace;
        speaker_ = vb::CreateSpeaker(config, log);
    }

    ~Fixture() {
        if (speaker_) {
            speaker_->Shutdown();
            speaker_.reset();
            vb::voice::test::ReleaseTestStalls();
            if (!vb::voice::test::WaitForVoiceThreads(5000)) {
                report_.check(false, "voice thread drain", flag_);
                TerminateProcess(GetCurrentProcess(), static_cast<UINT>(report_.result()));
            }
            activeTrace = nullptr;
        }
        if (!SetEnvironmentVariableA(flag_, nullptr)) report_.check(false, "test cleanup", flag_);
    }

    Availability ready() {
        if (!speaker_) return Availability::Failed;
        if (!trace.wait([](const Progress& p) { return p.warmed >= 2; }, 10s)) return Availability::Failed;
        return trace.wait([](const Progress& p) { return !p.openings.empty(); }, 3s)
            ? Availability::Ready : Availability::NoDevice;
    }

    bool play(const wchar_t* text, std::chrono::milliseconds timeout) {
        const auto oldStops = trace.snapshot().stops;
        speaker_->Speak(text);
        const auto phrase = trace.snapshot().phrase;
        const auto deadline = Clock::now() + timeout;
        if (!phrase || !trace.wait([&](const Progress& p) {
                return p.stops > oldStops && p.completed == phrase && p.lastPacket == phrase;
            }, timeout)) return false;
        while (speaker_->IsSpeaking() && Clock::now() < deadline) Sleep(10);
        return !speaker_->IsSpeaking();
    }

    std::string describe(const std::wstring& match) {
        const auto lines = log.matching(match);
        return lines.empty() ? "No matching log entry." : Utf8(lines.back());
    }

    Trace trace;
    Log log;

private:
    Report& report_;
    const char* flag_;
    std::unique_ptr<vb::ISpeaker> speaker_;
};

void CheckPadding(Report& report, const std::string& name, const Progress& p) {
    report.check(p.submissions > 0 && p.peakPaddingMs > 0 && p.peakPaddingMs <= 20.001,
                 name + " padding", "Peak " + Number(p.peakPaddingMs) + " ms over " +
                 std::to_string(p.submissions) + " hardware writes. Empty-padding observations with audio queued: " +
                 std::to_string(p.emptyPadding) + ".");
}

Availability TestDelayedStart(Report& report) {
    Fixture h(report, "VOICEBRIDGE_TEST_DELAY_DEVICE_START");
    const auto ready = h.ready();
    if (ready == Availability::NoDevice) {
        report.note("Silent WASAPI unavailable. " + h.describe(L"Audio device unavailable"));
        return ready;
    }
    if (ready == Availability::Failed) {
        report.check(false, "stopped device setup", h.describe(L"SAPI"));
        return ready;
    }
    report.note(h.describe(L"Audio device:"));
    const bool first = h.play(L"Map.", 4s);
    const auto firstStats = h.trace.snapshot();
    const bool second = first && h.play(L"Inventory.", 4s);
    const auto p = h.trace.snapshot();
    const auto primed = [](const Start& start) {
        return start.primedMs > 0 && start.primedMs <= 20.001 && start.delayMs > 20;
    };
    const bool initialStart = !firstStats.starts.empty() && primed(firstStats.starts.front()) &&
                              firstStats.starts.front().phrase == firstStats.completed;
    const bool restarted = p.starts.size() > firstStats.starts.size() && primed(p.starts.back()) &&
                           p.starts.back().phrase == p.completed && p.completed != firstStats.completed;
    std::string details;
    for (const auto& start : p.starts) {
        details += "Phrase " + std::to_string(start.phrase) + " primed " + Number(start.primedMs) +
                   " ms, preload to Start " + Number(start.delayMs) + " ms. ";
    }
    report.check(first && second && initialStart && restarted, "delayed hardware start and idle restart",
                 details.empty() ? "No hardware Start observed." : details);
    CheckPadding(report, "Delayed start", p);
    return ready;
}

void TestWatchdog(Report& report) {
    Fixture h(report, "VOICEBRIDGE_TEST_MISSING_RENDER_EVENT");
    const bool played = h.ready() == Availability::Ready && h.play(L"Map.", 12s);
    const auto p = h.trace.snapshot();
    report.check(played && p.watchdogs && p.submissionsAfterWatchdog && !p.starts.empty(),
                 "hardware progress after a missing render event", std::to_string(p.watchdogs) +
                 " watchdog timeouts and " + std::to_string(p.submissionsAfterWatchdog) +
                 " hardware writes after the first timeout.");
    CheckPadding(report, "Missing render event", p);
}

void TestRecovery(Report& report) {
    Fixture h(report, "VOICEBRIDGE_TEST_DEVICE_LOSS");
    const bool played = h.ready() == Availability::Ready &&
        h.play(L"The map is ready to explore. Open the door and follow the path to the bridge.", 8s);
    const auto p = h.trace.snapshot();
    const bool sameFormat = p.openings.size() >= 2 && p.openings.front() == p.openings.back() &&
                            p.packetFormat == p.openings.back();
    const bool samePhrase = p.starts.size() >= 2 && p.starts.front().phrase == p.completed &&
                            p.starts.back().phrase == p.completed;
    report.check(played && sameFormat && samePhrase && p.lost == 1 && p.restored == 1 && p.submissionsAfterRecovery,
                 "same-format hardware recovery during one phrase", std::to_string(p.openings.size()) +
                 " device opens, " + std::to_string(p.starts.size()) + " starts, and " +
                 std::to_string(p.submissionsAfterRecovery) + " hardware writes after the restarted stream. " +
                 "Final packets were " + std::to_string(p.packetFormat.rate) + " Hz.");
    CheckPadding(report, "Device recovery", p);
}

struct Resources {
    DWORD handles = 0;
    SIZE_T privateBytes = 0;
    bool valid = false;
};

Resources SampleResources() {
    const auto process = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    Resources resources;
    const bool haveHandles = GetProcessHandleCount(process, &resources.handles);
    const bool haveMemory = GetProcessMemoryInfo(process,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    resources.privateBytes = memory.PrivateUsage;
    resources.valid = haveHandles && haveMemory;
    return resources;
}

void TestRepeatedRecovery(Report& report) {
    Fixture h(report, "VOICEBRIDGE_TEST_REPEATED_DEVICE_LOSS");
    if (h.ready() != Availability::Ready) {
        report.check(false, "repeated device recovery setup", h.describe(L"Audio device unavailable"));
        return;
    }

    constexpr unsigned warmCycles = 3;
    constexpr unsigned totalCycles = 12;
    const auto format = h.trace.snapshot().openings.front();
    Resources warm;
    unsigned completed = 0;
    for (; completed < totalCycles; ++completed) {
        const auto before = h.trace.snapshot();
        const bool played = h.play(L"Open the inventory.", 5s);
        const auto after = h.trace.snapshot();
        const bool reopened = after.lost == before.lost + 1 && after.restored == before.restored + 1 &&
                              after.openings.size() == before.openings.size() + 1 && after.openings.back() == format;
        const bool samePhrase = after.starts.size() >= before.starts.size() + 2 &&
                                after.starts[before.starts.size()].phrase == after.completed &&
                                after.starts.back().phrase == after.completed && after.completed != before.completed;
        if (!played || !reopened || !samePhrase || !(after.packetFormat == format)) break;
        if (completed + 1 == warmCycles) warm = SampleResources();
    }
    const auto final = SampleResources();
    const auto p = h.trace.snapshot();
    report.check(completed == totalCycles, "repeated device recovery on one speaker",
                 std::to_string(completed) + " complete phrases, " + std::to_string(p.lost) + " losses, " +
                 std::to_string(p.restored) + " restorations and " + std::to_string(p.openings.size() - 1) +
                 " hardware reopens. Last complete phrase ID " + std::to_string(p.completed) + ".");

    constexpr double bytesPerMiB = 1024.0 * 1024;
    const auto handlesAdded = static_cast<long long>(final.handles) - warm.handles;
    const auto memoryAdded = (static_cast<double>(final.privateBytes) - warm.privateBytes) / bytesPerMiB;
    const bool sampled = warm.valid && final.valid && completed == totalCycles;
    report.check(sampled && handlesAdded <= 2 && memoryAdded <= 4, "resources across repeated device recovery",
                 sampled ? "After cycle 3 and cycle 12, handles " + std::to_string(warm.handles) + " to " +
                     std::to_string(final.handles) + ", private memory " + Number(warm.privateBytes / bytesPerMiB) +
                     " to " + Number(final.privateBytes / bytesPerMiB) + " MiB. Allowed growth is 2 handles and 4 MiB."
                     : "Could not compare resource samples after both sets of complete cycles.");
}

void TestDavidFallback(Report& report) {
    Fixture h(report, "VOICEBRIDGE_TEST_BAD_JENNY", L"David");
    const bool played = h.ready() == Availability::Ready && h.play(L"Voice fallback.", 5s);
    const auto p = h.trace.snapshot();
    const auto choices = h.log.matching(L"Fallback voice:", vb::LogLevel::Info);
    const bool choseDavid = choices.size() == 1 && choices.front().find(L"Fallback voice: Microsoft David") == 0 &&
                            choices.front().find(L"PCM 16000 Hz, 1 channel(s)") != std::wstring::npos;
    const Format nativeDavid{16000, 1};
    const bool renderedDavid = !p.openings.empty() && p.openings.back() == nativeDavid &&
                               p.packetFormat == nativeDavid && !p.starts.empty();
    report.check(played && choseDavid && renderedDavid, "failed Jenny falls back to David at native 16 kHz",
                 (choices.empty() ? "No fallback info log." : Utf8(choices.front())) + " Hardware and packets: " +
                 std::to_string(p.openings.empty() ? 0 : p.openings.back().rate) + " and " +
                 std::to_string(p.packetFormat.rate) + " Hz.");
    CheckPadding(report, "David fallback", p);
}

int RunTests() {
    Report report;
    if (!SetEnvironmentVariableA("VOICEBRIDGE_TEST_TRACE_DEVICE", "1")) {
        report.check(false, "test setup", "Could not enable hardware padding diagnostics.");
        return report.result();
    }
    const auto ready = TestDelayedStart(report);
    if (ready == Availability::NoDevice) return 2;
    if (ready == Availability::Ready) {
        TestWatchdog(report);
        TestRecovery(report);
        TestRepeatedRecovery(report);
        TestDavidFallback(report);
    }
    return report.result();
}

}  // namespace

namespace vb::voice::test {
Sink observer = ::Observe;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto code = RunTests();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
