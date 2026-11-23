#include "restart-trace.h"
#include "test-process.h"
#include "controller_rpc.h"
#include "controller.h"
#include "process.h"
#include "../../src/common/server_speaker.h"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace {
using namespace restart_test;

struct Child {
    vb::Handle process;

    ~Child() {
        if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 3000);
        }
    }
};

Child StartBridge(const wchar_t* args) {
    const auto path = TestPath(L"restart-bridge.exe");
    Require(!path.empty(), "expected the exact fixture path");
    auto command = L"\"" + path + L"\" " + args;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child), "start test bridge");
    CloseHandle(child.hThread);
    return {vb::Handle(child.hProcess)};
}

struct Mapping {
    vb::Handle handle;
    Trace* trace = nullptr;

    explicit Mapping(Scenario scenario) {
        const auto name = L"Local\\VoiceBridge.PuckRestart." + std::to_wstring(GetCurrentProcessId());
        handle.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Trace), name.c_str()));
        Require(handle && GetLastError() != ERROR_ALREADY_EXISTS, "create private restart trace");
        trace = static_cast<Trace*>(MapViewOfFile(handle.get(), FILE_MAP_WRITE, 0, 0, sizeof(Trace)));
        Require(trace, "map restart trace");
        trace->scenario = scenario;
        Require(SetEnvironmentVariableW(MappingVariable, name.c_str()), "pass private trace name to fixture");
    }

    ~Mapping() {
        if (trace) UnmapViewOfFile(trace);
        SetEnvironmentVariableW(MappingVariable, nullptr);
    }
};

template<class Predicate>
void WaitFor(Predicate ready, DWORD timeout, const char* error) {
    const auto deadline = GetTickCount64() + timeout;
    while (!ready()) {
        Require(GetTickCount64() < deadline, error);
        Sleep(10);
    }
}

DWORD WaitServing(Worker& worker, const Child& supervisor, const std::wstring& endpoint) {
    DWORD pid = 0;
    WaitFor([&] {
        Require(WaitForSingleObject(supervisor.process.get(), 0) == WAIT_TIMEOUT, "supervisor exited early");
        pid = static_cast<DWORD>(Read(worker.pid));
        const auto window = NvdaWindow();
        if (!pid || Owner(window) != pid) return false;
        const auto own = OpenTestProcess(pid, L"restart-bridge.exe");
        Require(!IsWindowVisible(window), "test bridge window must be hidden");
        return vb::ControllerAnswers(endpoint);
    }, 10000, "the expected test worker did not begin serving");
    return pid;
}

struct Controller {
    explicit Controller(const std::wstring& endpoint) {
        Require(vb::BindController(endpoint, &controllerBinding) == RPC_S_OK, "bind the test controller");
        controller3Binding = controllerBinding;
    }
    ~Controller() {
        RpcBindingFree(&controllerBinding);
        controller3Binding = nullptr;
    }
    void speak(const wchar_t* text) {
        Require(client_speakText(text) == RPC_S_OK, "test speakText RPC acknowledged");
    }
    bool speaking() {
        boolean status = FALSE;
        Require(client_isSpeaking(&status) == RPC_S_OK, "test isSpeaking RPC acknowledged");
        return status;
    }
};

void ExpectOnsets(Worker& worker, std::initializer_list<Phrase> expected, FILE* result) {
    const LONG count = Read(worker.onsetCount);
    fprintf(result, "Worker %ld first nonzero packets (%ld):", Read(worker.pid), count);
    for (LONG i = 0; i < count && i < OnsetCapacity; ++i)
        fprintf(result, " %ld", static_cast<LONG>(worker.onsets[i].phrase));
    fputc('\n', result);
    fflush(result);
    Require(count == static_cast<LONG>(expected.size()), "wrong number of nonzero packet onsets");
    unsigned i = 0;
    for (const auto phrase : expected)
        Require(worker.onsets[i++].phrase == phrase, "unexpected or duplicated phrase reached nonzero audio");
}

double Milliseconds(LONG64 duration) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return duration * 1000.0 / frequency.QuadPart;
}

void AssertIsolatedSession(const std::wstring& endpoint) {
    Require(IsTestProcess(GetCurrentProcess(), L"restart-driver.exe"), "expected the exact SSH test driver");
    Require(!endpoint.empty() && !NvdaWindow(), "expected an idle SSH controller desktop");
    const auto nvdaMutex = L"Local\\NVDA_" + vb::DesktopName();
    for (const auto name : {vb::SupervisorMutex, vb::ServerMutex, nvdaMutex.c_str()}) {
        const vb::Handle mutex(OpenMutexW(SYNCHRONIZE, FALSE, name));
        Require(!mutex && GetLastError() == ERROR_FILE_NOT_FOUND, "existing startup mutex in SSH session");
    }
    const vb::Handle stop(OpenEventW(SYNCHRONIZE, FALSE, vb::StopEventName));
    Require(!stop && GetLastError() == ERROR_FILE_NOT_FOUND, "existing VoiceBridge stop event in SSH session");
}

void CheckLog(Scenario scenario, DWORD initial, DWORD intermediate, DWORD replacement) {
    std::ifstream log("bridge.log", std::ios::binary);
    Require(log.good(), "open supervisor log");
    const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    const auto retry = contents.find("Retrying server after ");
    const auto reset = contents.find("Discarding pending speech after consecutive server failures.");
    if (scenario == Scenario::Poison || scenario == Scenario::SlowProbe) {
        constexpr std::string_view firstDelay = "Retrying server after 1000 ms";
        Require(retry != std::string::npos && contents.compare(retry, firstDelay.size(), firstDelay) == 0 &&
                contents.find("Retrying server after ", retry + 1) == std::string::npos,
                "expected one one-second restart delay");
    } else {
        Require(retry == std::string::npos, "supervisor applied restart backoff");
    }
    if (scenario == Scenario::Poison) {
        Require(reset < retry && contents.find("Discarding pending speech", reset + 1) == std::string::npos,
                "expected one journal reset before restart backoff");
    } else {
        Require(reset == std::string::npos, "unexpected journal reset after server failure");
    }
    if (scenario == Scenario::SlowProbe) {
        const auto timedOut = " " + std::to_string(intermediate) + " server error The controller probe timed out at ";
        const auto timeoutExit = "Server exited with code " + std::to_string(vb::ControllerProbeTimedOut);
        const auto timeout = contents.find(timeoutExit);
        Require(contents.find(timedOut) < timeout && timeout < retry,
                "expected a distinct probe timeout exit before restart backoff");
    }
    for (const auto pid : {initial, replacement}) {
        const auto selected = " " + std::to_string(pid) + " server info Voice: Microsoft Jenny (Natural)";
        Require(contents.find(selected) != std::string::npos, "expected both workers to select natural Jenny");
    }
    const std::string fatal = " server error All four SAPI workers exceeded their watchdog while speech was waiting.";
    const auto fatalAt = contents.find(fatal);
    if (scenario == Scenario::Stalled || scenario == Scenario::Poison) {
        const auto exact = " " + std::to_string(initial) + fatal;
        Require(contents.find(exact) != std::string::npos && contents.find(fatal, fatalAt + 1) == std::string::npos,
                "expected one watchdog fatal line from the original worker");
    } else {
        Require(fatalAt == std::string::npos, "unexpected watchdog failure during the crash test");
    }
}

LONG64 ExpectExit(HANDLE process, DWORD expected, FILE* result) {
    Require(WaitForSingleObject(process, 35000) == WAIT_OBJECT_0, "the worker did not exit");
    const auto observed = Tick();
    DWORD exit = 0;
    Require(GetExitCodeProcess(process, &exit), "obtain actual worker exit status");
    fprintf(result, "Worker %lu exited with 0x%08lx.\n", GetProcessId(process), exit);
    fflush(result);
    Require(exit == expected, "the worker did not exit with the expected status");
    return observed;
}

void StopBridge(const Child& supervisor, HANDLE worker) {
    const auto stop = StartBridge(L"--stop");
    Require(WaitForSingleObject(stop.process.get(), 5000) == WAIT_OBJECT_0 &&
            WaitForSingleObject(supervisor.process.get(), 0) == WAIT_OBJECT_0 &&
            WaitForSingleObject(worker, 0) == WAIT_OBJECT_0, "the supervisor and worker did not stop");
    DWORD stopped = 1;
    Require(GetExitCodeProcess(stop.process.get(), &stopped) && stopped == 0, "stop command failed");
}

void RunRecurringStall(FILE* result) {
    const auto endpoint = vb::EndpointName();
    AssertIsolatedSession(endpoint);
    const auto foreground = GetForegroundWindow();
    Mapping mapping(Scenario::RecurringStall);
    const auto supervisor = StartBridge(L"");
    auto& initial = mapping.trace->workers[0];
    const auto pid = WaitServing(initial, supervisor, endpoint);
    const auto worker = OpenTestProcess(pid, L"restart-bridge.exe");
    WaitFor([&] { return Read(initial.warmCount) >= 2; }, 10000, "the original worker did not warm two engines");
    {
        Controller controller(endpoint);
        controller.speak(Poison);
        controller.speak(FirstQueued);
        controller.speak(SecondQueued);
        WaitFor([&] {
            Require(WaitForSingleObject(worker.get(), 0) == WAIT_TIMEOUT, "one poison phrase caused a server restart");
            return Read(initial.onsetCount) >= 2;
        }, 15000, "speech queued behind the recurring stall did not play");
        Require(Read(initial.stallCount) == 3, "expected the original poison attempt and two retries");
        ExpectOnsets(initial, {Phrase::FirstQueued, Phrase::SecondQueued}, result);
        controller.speak(Later);
        WaitFor([&] { return Read(initial.onsetCount) >= 3 && !controller.speaking(); }, 10000,
                "fresh speech did not finish after the recurring stall");
        ExpectOnsets(initial, {Phrase::FirstQueued, Phrase::SecondQueued, Phrase::Later}, result);
    }
    StopBridge(supervisor, worker.get());
    Require(Read(initial.lifecycleCount) > 0, "SAPI voice lifecycle lock was not exercised");
    Require(Read(mapping.trace->nextWorker) == 1, "the recurring stall restarted the server");
    Require(GetForegroundWindow() == foreground, "foreground changed during the recurring stall test");
    CheckLog(Scenario::RecurringStall, pid, 0, pid);
    fputs("One phrase stalled three engines. Both waiting phrases and fresh speech played exactly once on the same server.\n",
          result);
}

void RunHealthyRun(FILE* result) {
    const auto endpoint = vb::EndpointName();
    AssertIsolatedSession(endpoint);
    const auto foreground = GetForegroundWindow();
    Mapping mapping(Scenario::HealthyRun);
    const auto supervisor = StartBridge(L"");
    auto& first = mapping.trace->workers[0];
    auto& healthy = mapping.trace->workers[1];
    auto& replacement = mapping.trace->workers[2];
    const auto firstPid = WaitServing(first, supervisor, endpoint);
    const auto firstProcess = OpenTestProcess(firstPid, L"restart-bridge.exe");
    WaitFor([&] { return Read(first.warmCount) >= 2; }, 10000, "the original worker did not warm two engines");
    WaitFor([&] { return Milliseconds(Tick() - first.startTick) >= 5300; }, 6000,
            "first worker did not reach the supervisor's steady-state restart threshold");
    {
        Controller controller(endpoint);
        controller.speak(InterruptedText().c_str());
        WaitFor([&] { return Read(first.onsetCount) >= 1; }, 4000, "in-flight phrase did not begin");
        InterlockedExchange(&first.crashRequested, 1);
    }
    ExpectExit(firstProcess.get(), CrashCode, result);

    const auto healthyPid = WaitServing(healthy, supervisor, endpoint);
    Require(healthyPid != firstPid, "the same supervisor started a different worker");
    const auto healthyProcess = OpenTestProcess(healthyPid, L"restart-bridge.exe");
    WaitFor([&] { return Read(healthy.warmCount) >= 2; }, 10000, "the healthy worker did not warm two engines");
    // Outlast the shortest healthy run the test build accepts (30 s).
    WaitFor([&] { return Milliseconds(Tick() - healthy.startTick) >= 31500; }, 33000,
            "the healthy worker did not run long enough");
    {
        Controller controller(endpoint);
        controller.speak(Later);
        WaitFor([&] { return Read(healthy.onsetCount) >= 1 && !controller.speaking(); }, 10000,
                "the healthy worker did not speak a phrase");
        ExpectOnsets(healthy, {Phrase::Later}, result);
        controller.speak(InterruptedText().c_str());
        WaitFor([&] { return Read(healthy.onsetCount) >= 2; }, 4000, "in-flight phrase did not begin");
        controller.speak(BeforeReset);
        ExpectOnsets(healthy, {Phrase::Later, Phrase::Interrupted}, result);
        InterlockedExchange(&healthy.crashRequested, 1);
    }
    const auto exitObserved = ExpectExit(healthyProcess.get(), CrashCode, result);

    const auto nextPid = WaitServing(replacement, supervisor, endpoint);
    Require(nextPid != healthyPid && nextPid != firstPid, "the same supervisor started a third worker");
    const auto next = OpenTestProcess(nextPid, L"restart-bridge.exe");
    {
        Controller controller(endpoint);
        WaitFor([&] { return Read(replacement.onsetCount) >= 1; }, 10000,
                "the waiting phrase did not reach PCM output after the second crash");
        fprintf(result, "Second crash: observed exit to replacement speaker creation: %.2f ms; to waiting phrase: %.2f ms.\n",
                Milliseconds(replacement.speakerTick - exitObserved),
                Milliseconds(replacement.onsets[0].tick - exitObserved));
        Require(Milliseconds(replacement.startTick - exitObserved) < 900, "the second crash was delayed by backoff");
        controller.speak(Later);
        WaitFor([&] { return Read(replacement.onsetCount) >= 2 && !controller.speaking(); }, 10000,
                "fresh speech did not finish after the second crash");
        ExpectOnsets(replacement, {Phrase::BeforeReset, Phrase::Later}, result);
    }
    StopBridge(supervisor, next.get());
    Require(Read(mapping.trace->nextWorker) == 3, "unexpected worker process count");
    Require(GetForegroundWindow() == foreground, "foreground changed during the healthy run test");
    CheckLog(Scenario::HealthyRun, firstPid, healthyPid, nextPid);
    fputs("After a healthy run the second crash restarted at once, kept the waiting phrase, and played it exactly once.\n",
          result);
}

void Run(Scenario scenario, FILE* result) {
    const bool poison = scenario == Scenario::Poison;
    const bool slowProbe = scenario == Scenario::SlowProbe;
    const bool stall = scenario == Scenario::Stalled || poison;
    const auto endpoint = vb::EndpointName();
    AssertIsolatedSession(endpoint);
    const auto foreground = GetForegroundWindow();
    Mapping mapping(scenario);
    const auto supervisor = StartBridge(L"");
    auto& initial = mapping.trace->workers[0];
    auto& intermediate = mapping.trace->workers[1];
    auto& replacement = mapping.trace->workers[poison || slowProbe ? 2 : 1];
    const auto pid = WaitServing(initial, supervisor, endpoint);
    const auto first = OpenTestProcess(pid, L"restart-bridge.exe");
    WaitFor([&] { return Read(initial.warmCount) >= 2; }, 10000, "the original worker did not warm two engines");

    LONG64 atFault = 0;
    {
        Controller controller(endpoint);
        if (stall) {
            // Two canceled calls tie up the first engines. The first waiting
            // phrase can use both retries on the replacement engines. In the
            // poison case reserve the last engine for the exact poison text.
            const LONG markers = poison ? 4 : 2;
            for (LONG count = 1; count <= markers; ++count) {
                const bool keep = poison && count == 4;
                controller.speak(keep ? Poison : Cancelled);
                WaitFor([&] { return Read(initial.stallCount) >= count; }, 14000, "test phrase did not enter a stall");
                if (!keep) Require(client_cancelSpeech() == RPC_S_OK, "test cancelSpeech RPC acknowledged");
            }
            atFault = Read(initial.firstStall);
        } else {
            WaitFor([&] { return Milliseconds(Tick() - initial.startTick) >= 5300; }, 6000,
                    "first worker did not reach the supervisor's steady-state restart threshold");
            controller.speak(InterruptedText().c_str());
            WaitFor([&] { return Read(initial.onsetCount) >= 1; }, 4000, "in-flight phrase did not begin");
        }
        controller.speak(FirstQueued);
        controller.speak(SecondQueued);
        fputs("Both queued speakText calls returned success before the worker exited.\n", result);
        fflush(result);
        if (!stall) {
            ExpectOnsets(initial, {Phrase::Interrupted}, result);
            InterlockedExchange(&initial.crashRequested, 1);
        }
    }

    const auto exitObserved = ExpectExit(first.get(), stall ? vb::SpeechWorkersStuck : CrashCode, result);
    if (stall) {
        fprintf(result, "The original worker entered %ld SAPI stalls.\n", Read(initial.stallCount));
        Require(Read(initial.stallCount) == 4, "exactly four SAPI workers entered a stall");
        ExpectOnsets(initial, {}, result);
    } else {
        ExpectOnsets(initial, {Phrase::Interrupted}, result);
        atFault = Read(initial.crashTick);
        Require(atFault > initial.onsets[0].tick, "the worker raised its own exception after playback began");
    }

    if (poison) {
        const auto replayPid = WaitServing(intermediate, supervisor, endpoint);
        Require(replayPid != pid, "the same supervisor started a separate replay worker");
        const auto replay = OpenTestProcess(replayPid, L"restart-bridge.exe");
        WaitFor([&] { return Read(intermediate.stallCount) >= 2; }, 10000, "the poison did not stall multiple replay attempts");
        WaitFor([&] { return Milliseconds(Tick() - intermediate.startTick) >= 5200; }, 6000,
                "the replay worker did not reach the steady-state restart threshold");
        {
            Controller controller(endpoint);
            controller.speak(BeforeReset);
        }
        InterlockedExchange(&intermediate.crashRequested, 1);
        ExpectExit(replay.get(), CrashCode, result);
        ExpectOnsets(intermediate, {}, result);
        fputs("The poison stalled multiple engines after replay and another RPC request was pending when that worker crashed.\n",
              result);
    }

    const auto nextPid = WaitServing(replacement, supervisor, endpoint);
    Require(pid != nextPid, "the same supervisor started a different worker");
    const auto next = OpenTestProcess(nextPid, L"restart-bridge.exe");
    const auto middlePid = static_cast<DWORD>(Read(intermediate.pid));
    if (slowProbe) {
        Require(middlePid && middlePid != pid && middlePid != nextPid && !intermediate.speakerTick &&
                !Read(intermediate.warmCount), "the delayed probe should exit before starting a speaker");
        ExpectOnsets(intermediate, {}, result);
        fputs("The first replacement timed out during its controller probe, before it could create a speaker.\n", result);
    }
    {
        Controller controller(endpoint);
        if (poison) {
            Require(nextPid != middlePid && Milliseconds(replacement.startTick - Read(intermediate.crashTick)) >= 950,
                    "consecutive failures should delay the replacement");
            Require(!controller.speaking() && !Read(replacement.stallCount), "the replacement inherited pending poison or speech");
        } else {
            WaitFor([&] { return Read(replacement.onsetCount) >= 2; }, 10000,
                    "recovered phrases did not reach PCM output");
            fprintf(result, "Fault to first replacement nonzero sample: %.2f ms. Observed exit to that sample: %.2f ms.\n",
                    Milliseconds(replacement.onsets[0].tick - atFault),
                    Milliseconds(replacement.onsets[0].tick - exitObserved));
            fprintf(result, "Observed exit to replacement speaker creation: %.2f ms.\n",
                    Milliseconds(replacement.speakerTick - exitObserved));
            ExpectOnsets(replacement, {Phrase::FirstQueued, Phrase::SecondQueued}, result);
        }
        controller.speak(Later);
        WaitFor([&] { return Read(replacement.onsetCount) >= (poison ? 1 : 3) && !controller.speaking(); }, 10000,
                "test playback did not finish after the fresh request");
        if (poison) ExpectOnsets(replacement, {Phrase::Later}, result);
        else ExpectOnsets(replacement, {Phrase::FirstQueued, Phrase::SecondQueued, Phrase::Later}, result);
    }

    StopBridge(supervisor, next.get());
    Require(Read(initial.lifecycleCount) > 0 && Read(replacement.lifecycleCount) > 0,
            "both servers should exercise the SAPI voice lifecycle lock");
    Require(Read(mapping.trace->nextWorker) == (poison || slowProbe ? 3 : 2), "unexpected worker process count");
    Require(GetForegroundWindow() == foreground, "foreground changed during the restart test");
    CheckLog(scenario, pid, middlePid, nextPid);
    if (stall) fputs("The original worker logged exactly one fatal watchdog line.\n", result);
    if (poison) {
        fputs("The same supervisor waited one second, reset the speech journal, and played fresh speech exactly once.\n",
              result);
    } else {
        fputs("The same supervisor recovered both waiting phrases and played fresh speech, each exactly once.\n", result);
    }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    const std::wstring_view mode(args);
    if (mode == L"stalled") return Test("restart-stalled.txt", [](FILE* result) { Run(Scenario::Stalled, result); });
    if (mode == L"crash") return Test("restart-crash.txt", [](FILE* result) { Run(Scenario::Crash, result); });
    if (mode == L"poison") return Test("restart-poison.txt", [](FILE* result) { Run(Scenario::Poison, result); });
    if (mode == L"slow-probe") return Test("restart-slow-probe.txt", [](FILE* result) { Run(Scenario::SlowProbe, result); });
    if (mode == L"recurring-stall") return Test("restart-recurring-stall.txt", RunRecurringStall);
    if (mode == L"healthy-run") return Test("restart-healthy-run.txt", RunHealthyRun);
    return 2;
}
