#include "restart-trace.h"
#include "../voice/observer.h"
#include "../../src/common/server_speaker.h"
#include "../../src/front/handle.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace vb::voice::test { Sink observer = nullptr; }

namespace vb {
std::unique_ptr<ISpeaker> CreateInstrumentedSpeaker(const SpeakerConfig&, ILog&, SpeechJournal&, HANDLE);

namespace {
using namespace restart_test;
namespace obs = voice::test;

Trace* trace;
Worker* worker;
LONG workerIndex;
std::uint64_t lastOnset;
Handle crashEvent;
bool crashOnRequest;

DWORD WINAPI CrashWorker(void*) {
    if (crashOnRequest) {
        while (!Read(worker->crashRequested)) Sleep(5);
    } else if (WaitForSingleObject(crashEvent.get(), INFINITE) != WAIT_OBJECT_0) {
        return 1;
    }
    InterlockedExchange64(&worker->crashTick, Tick());
    // This native thread has no C++ exception handler. The process error mode
    // established at startup suppresses crash dialogs for this exception.
    RaiseException(CrashCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 1;
}

Phrase Identify(std::wstring_view text) {
    if (text == Cancelled) return Phrase::Cancelled;
    if (text == InterruptedText()) return Phrase::Interrupted;
    if (text == FirstQueued) return Phrase::FirstQueued;
    if (text == SecondQueued) return Phrase::SecondQueued;
    if (text == Later) return Phrase::Later;
    if (text == Poison) return Phrase::Poison;
    if (text == BeforeReset) return Phrase::BeforeReset;
    return Phrase::Unknown;
}

void Observe(const obs::Observation& event) {
    if (event.kind == obs::Kind::Warmed) {
        InterlockedIncrement(&worker->warmCount);
    } else if (event.kind == obs::Kind::LifecycleEntered) {
        InterlockedIncrement(&worker->lifecycleCount);
    } else if (event.kind == obs::Kind::StallEntered) {
        InterlockedCompareExchange64(&worker->firstStall, Tick(), 0);
        InterlockedIncrement(&worker->stallCount);
    } else if (event.kind == obs::Kind::Packet && event.text) {
        if (crashEvent && event.id == lastOnset && Read(worker->crashRequested) &&
            std::wstring_view(event.text) == InterruptedText()) {
            InterlockedExchange(&worker->crashRequested, 0);
            SetEvent(crashEvent.get());
        }
        if (event.id != lastOnset &&
            std::any_of(event.samples, event.samples + event.count, [](auto sample) { return sample != 0; })) {
            lastOnset = event.id;
            const LONG index = Read(worker->onsetCount);
            if (index < OnsetCapacity) worker->onsets[index] = {Identify(event.text), Tick()};
            InterlockedIncrement(&worker->onsetCount);
        }
    }
}
}

void BeforeRestartTestControllerProbe() {
    DWORD session = 0;
    wchar_t name[100]{};
    const auto length = GetEnvironmentVariableW(MappingVariable, name, 100);
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session != 0 || !length || length >= 100 ||
        std::wstring_view(name).find(L"Local\\VoiceBridge.PuckRestart.") != 0) {
        return;
    }
    const Handle handle(OpenFileMappingW(FILE_MAP_WRITE, FALSE, name));
    if (!handle) return;
    trace = static_cast<Trace*>(MapViewOfFile(handle.get(), FILE_MAP_WRITE, 0, 0, sizeof(Trace)));
    if (!trace) return;
    workerIndex = InterlockedIncrement(&trace->nextWorker) - 1;
    if (workerIndex >= static_cast<LONG>(std::size(trace->workers))) return;
    worker = &trace->workers[workerIndex];
    worker->startTick = Tick();
    InterlockedExchange(&worker->pid, GetCurrentProcessId());
    if (trace->scenario == Scenario::SlowProbe && workerIndex == 1) Sleep(INFINITE);
}

std::unique_ptr<ISpeaker> CreateServerSpeaker(const SpeakerConfig& config, ILog& log,
                                             SpeechJournal& journal, HANDLE exhausted) {
    if (!worker) throw std::runtime_error("Expected an isolated restart test worker in SSH session zero.");
    worker->speakerTick = Tick();
    // The probe mapped the trace before startup. Keep it mapped through any
    // abrupt exit and initialize the observer before starting voice threads.
    InterruptedText();
    obs::observer = Observe;
    if (workerIndex == 0 && trace->scenario == Scenario::Stalled) obs::SetTestStalls(4);
    if (trace->scenario == Scenario::Poison || trace->scenario == Scenario::RecurringStall) {
        if (!SetEnvironmentVariableW(L"VOICEBRIDGE_TEST_STALL_TEXT", Poison))
            throw std::runtime_error("Could not set the recurring poison phrase.");
        if (workerIndex == 0 && trace->scenario == Scenario::Poison) obs::SetTestStalls(3);
    }
    crashOnRequest = workerIndex == 1 && trace->scenario == Scenario::Poison;
    const bool crashInFlight = (workerIndex == 0 &&
        (trace->scenario == Scenario::Crash || trace->scenario == Scenario::SlowProbe)) ||
        (workerIndex <= 1 && trace->scenario == Scenario::HealthyRun);
    if (crashOnRequest || crashInFlight) {
        if (!(GetErrorMode() & SEM_NOGPFAULTERRORBOX)) throw std::runtime_error("Crash UI must be disabled.");
        if (crashInFlight) {
            crashEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!crashEvent) throw std::runtime_error("Could not create the crash trigger.");
        }
        const Handle thread(CreateThread(nullptr, 0, CrashWorker, nullptr, 0, nullptr));
        if (!thread) throw std::runtime_error("Could not start the crash fixture thread.");
    }

    auto silent = config;
    silent.followNarrator = false;
    silent.nullOutput = true;
    silent.silentOutput = true;
    silent.voice = L"Jenny (Natural)";
    silent.fallbackVoice = L"Jenny (Natural)";
    silent.cacheMegabytes = 0;
    return CreateInstrumentedSpeaker(silent, log, journal, exhausted);
}
}  // namespace vb
