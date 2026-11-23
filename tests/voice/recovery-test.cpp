#include "../../src/common/speaker.h"
#include "observer.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using vb::voice::test::Kind;
using vb::voice::test::Observation;

struct Results {
    unsigned workers = 0, warm = 0, stalls = 0, retired = 0;
    std::map<unsigned long long, unsigned> completions, frames, rates;
    Clock::time_point firstStall{}, firstRetirement{};
};

Results events;
std::mutex eventMutex;
std::condition_variable changed;

void Observe(const Observation& observation) {
    std::lock_guard<std::mutex> lock(eventMutex);
    switch (observation.kind) {
    case Kind::WorkerStarted: ++events.workers; break;
    case Kind::Warmed: ++events.warm; break;
    case Kind::StallEntered:
        if (!events.stalls++) events.firstStall = Clock::now();
        break;
    case Kind::Abandoned:
        if (!events.retired++) events.firstRetirement = Clock::now();
        break;
    case Kind::Complete: ++events.completions[observation.id]; break;
    case Kind::Packet:
        events.rates[observation.id] = observation.rate;
        events.frames[observation.id] += observation.count / observation.channels;
        break;
    default: return;
    }
    changed.notify_all();
}

template<class Condition>
bool Wait(std::chrono::milliseconds timeout, Condition condition) {
    std::unique_lock<std::mutex> lock(eventMutex);
    return changed.wait_for(lock, timeout, [&] { return condition(events); });
}

Results Snapshot() {
    std::lock_guard<std::mutex> lock(eventMutex);
    return events;
}

class Log final : public vb::ILog {
public:
    void Write(vb::LogLevel, const std::wstring& text) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (text.find(L"David") != std::wstring::npos && text.find(L"16000 Hz") != std::wstring::npos)
            david = true;
        if (david && text.find(L"Jenny") != std::wstring::npos && text.find(L"24000 Hz") != std::wstring::npos)
            restoredJenny = true;
        if (text.find(L"private marker") != std::wstring::npos) leaked = true;
    }
    vb::LogLevel Level() const override { return vb::LogLevel::Info; }
    std::mutex mutex;
    bool david = false, leaked = false;
    std::atomic<bool> restoredJenny{false};
};

std::ofstream report;
int failures = 0;
void Check(bool passed, const char* description) {
    report << (passed ? "PASS " : "FAIL ") << description << std::endl;
    failures += !passed;
}

class Fixture {
public:
    Fixture() {
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            events = {};
        }
        vb::SpeakerConfig config;
        config.nullOutput = true;
        config.silentOutput = true;
        config.cacheMegabytes = 0;
        config.followNarrator = false;
        config.voice = L"Jenny (Natural)";
        config.fallbackVoice = L"David";
        speaker = vb::CreateSpeaker(config, log);
    }
    ~Fixture() {
        speaker->Shutdown();
        vb::voice::test::ReleaseTestStalls();
        bool drained = vb::voice::test::WaitForVoiceThreads(5000);
        Check(drained, "Every VoiceBridge thread exited, including retired workers.");
        if (!drained) TerminateProcess(GetCurrentProcess(), 1);
    }
    bool played(unsigned id) {
        if (!Wait(4s, [id](Results& r) { return r.completions[id] && r.frames[id]; })) return false;
        const auto deadline = Clock::now() + 4s;
        while (speaker->IsSpeaking() && Clock::now() < deadline) Sleep(10);
        return !speaker->IsSpeaking() && Snapshot().completions[id] == 1;
    }
    Log log;
    std::unique_ptr<vb::ISpeaker> speaker;
};

void CallDeadline() {
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_CALL_STALL", "1");
    {
        Fixture h;
        h.speaker->Speak(L"Ready.");
        Check(h.played(1), "The other voice played the startup queue during a stalled setup call.");
        bool replaced = Wait(13s, [](const Results& r) { return r.retired == 1 && r.warm >= 2; });
        const auto r = Snapshot();
        const auto delay = std::chrono::duration<double>(r.firstRetirement - r.firstStall).count();
        report << "Configuration call deadline: " << delay << " s, started workers: " << r.workers << std::endl;
        Check(replaced && delay >= 9.95 && delay < 11.0 && r.workers == 3,
              "The ten-second COM watchdog replaced the blocked worker while the call remained outstanding.");
        auto begin = Clock::now();
        h.speaker->Shutdown();
        auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        report << "Shutdown with a held call: " << elapsed << " ms." << std::endl;
        Check(elapsed < 100 && !h.speaker->IsSpeaking() && !vb::voice::test::WaitForVoiceThreads(40),
              "Shutdown returned while the stalled thread was still blocked.");
    }
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_CALL_STALL", nullptr);
}

void RepeatedStalls() {
    Fixture h;
    if (!Wait(10s, [](const Results& r) { return r.warm == 2; })) {
        Check(false, "Both voices warmed for the repeated-stall case.");
        return;
    }
    vb::voice::test::SetTestStalls(8);
    h.speaker->Speak(L"Discard this.");
    const bool allBlocked = Wait(15s, [](const Results& r) { return r.retired == 4; });
    const auto before = Snapshot();
    auto begin = Clock::now();
    h.speaker->Cancel();
    h.speaker->Speak(L"New selection.");
    double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    // Give a runaway replacement time to start even with all four calls held.
    Sleep(250);
    auto bounded = Snapshot();
    Check(allBlocked && before.workers == 4 && bounded.workers == 4 && bounded.stalls == 4,
          "Four held engine calls exhausted the worker budget without starting a fifth worker.");
    vb::voice::test::ReleaseTestStalls();
    bool played = h.played(2);
    auto after = Snapshot();
    Check(played && !after.frames[1] && after.frames[2] && after.completions[2] == 1,
          "The replacement phrase played once after capacity returned. The cancelled phrase supplied zero frames.");
    report << "Cancel and Speak returned in " << elapsed << " ms. Held workers: " << bounded.workers
           << ". Pre-cancel frames: " << after.frames[1] << ". Replacement frames: " << after.frames[2] << std::endl;
}

void RuntimeFailure() {
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_FAIL_JENNY", "1");
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_REPROBE", "1");
    {
        Fixture h;
        Check(Wait(10s, [](const Results& r) { return r.warm == 2; }), "Both Jenny workers warmed.");
        h.speaker->Speak(L"private marker");
        h.speaker->Speak(L"Next item.");
        bool heard = Wait(8s, [](Results& r) { return r.completions[2] && r.frames[2]; });
        auto r = Snapshot();
        {
            std::lock_guard<std::mutex> lock(h.log.mutex);
            Check(heard && r.completions[1] == 1 && r.completions[2] == 1 && r.frames[1] &&
                  r.rates[1] == 16000 && r.rates[2] == 16000 && h.log.david,
                  "Failing local Jenny tokens were replaced. David spoke both queued requests.");
            Check(!h.log.leaked, "The marker from the failed and retried phrase never reached the info log.");
        }
        report << "Retry completions: " << r.completions[1] << ", " << r.completions[2]
               << ". First phrase frames: " << r.frames[1] << std::endl;
        SetEnvironmentVariableA("VOICEBRIDGE_TEST_FAIL_JENNY", nullptr);
        const auto deadline = Clock::now() + 8s;
        while (heard && !h.log.restoredJenny && Clock::now() < deadline) Sleep(10);
        const bool restored = h.log.restoredJenny;
        h.speaker->Speak(L"Restored voice.");
        Check(restored && h.played(3) && Snapshot().rates[3] == 24000,
              "The preferred Jenny voice returned without a new Narrator setting or a restart.");
    }
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_FAIL_JENNY", nullptr);
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_REPROBE", nullptr);
}

void SilentWarm() {
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_SILENT_WARM", "1");
    {
        Fixture h;
        h.speaker->Speak(L"Map.");
        Check(h.played(1) && Snapshot().rates[1] == 16000,
              "A Jenny warm-up containing only silence fell back to audible David PCM.");
    }
    SetEnvironmentVariableA("VOICEBRIDGE_TEST_SILENT_WARM", nullptr);
}
} // namespace

namespace vb::voice::test { Sink observer = ::Observe; }

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    report.open("recovery-results.txt");
    report << std::fixed << std::setprecision(3);
    CallDeadline();
    RepeatedStalls();
    RuntimeFailure();
    SilentWarm();
    report << "Failures: " << failures << std::endl;
    report.close();
    TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
    return failures ? 1 : 0;
}
