#include "log.h"
#include "../common/server_speaker.h"
#include "../common/speech_journal.h"
#include "process.h"
#include "controller_rpc.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdlib>

#ifdef VB_RESTART_TEST
namespace vb { void BeforeRestartTestControllerProbe(); }
#endif

namespace {
using namespace vb;

constexpr DWORD NvdaCheckMs = 500;
// A server that ran this long before failing was healthy: its failure starts a
// new run of failures instead of extending the last one. Recovering from one
// stall or crash takes well under half of this.
#ifdef VB_RESTART_TEST
constexpr ULONGLONG HealthyRunMs = 30000;
#else
constexpr ULONGLONG HealthyRunMs = 60000;
#endif

[[noreturn]] void Finish(Log& log, UINT code) {
    log.FlushAndStop(75);
    // This also releases the RPC endpoint. ExitProcess can block in DLL detach
    // when a worker owns a lock, so terminate after the speaker is shut down.
    TerminateProcess(GetCurrentProcess(), code);
    std::abort();
}

[[noreturn]] void Fail(Log& log, const std::wstring& message, DWORD error) {
    log.Write(LogLevel::Error, message + L" (" + std::to_wstring(error) + L")");
    Finish(log, 1);
}

[[noreturn]] void StandAside(Log& log, const std::wstring& reason) {
    log.Write(LogLevel::Info, L"Standing aside because " + reason);
    Finish(log, ServerStandAside);
}

bool Stopping(HANDLE stop) {
    return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

int Stop() {
    const Handle stop(CreateEventW(nullptr, TRUE, FALSE, StopEventName));
    if (!stop || !SetEvent(stop.get())) return 1;
    // The supervisor waits for its child before exiting. Keep the event set
    // while waiting for both roles, including a server started directly.
    for (const auto name : {SupervisorMutex, ServerMutex}) {
        const Handle role(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name));
        if (!role) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return 1;
            continue;
        }
        const DWORD wait = WaitForSingleObject(role.get(), 15000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return 1;
        ReleaseMutex(role.get());
    }
    return 0;
}

// NVDA's ordinary, ease-of-access and restart paths all acquire this mutex
// before registering RPC. Only create it while absent. Never wait for NVDA's
// mutex or keep a handle to an existing one.
DWORD RunServer(HANDLE stop, const std::filesystem::path& executable, Log& log, SpeechJournalMapping& journal) {
    const auto desktop = DesktopName();
    if (desktop.empty()) return ERROR_INVALID_NAME;
    const auto name = L"Local\\NVDA_" + desktop;
    if (!journal.handle() && !journal.create()) {
        log.Write(LogLevel::Error, L"Could not create the pending speech queue");
        return 1;
    }
    auto server = StartServer(executable, journal.handle());
    if (!server.process) {
        log.Write(LogLevel::Error, L"Could not start server (" + std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    bool alreadyExists = false;
    const auto gate = CreateNvdaStartupMutex(name, alreadyExists);
    const auto nvda = gate ? HasOtherMutexHandle(gate.get()) : std::optional<bool>(alreadyExists);
    DWORD code = 1;
    bool requested = Stopping(stop);
    if (gate && nvda == false && !requested &&
        ResumeThread(server.thread.get()) != static_cast<DWORD>(-1)) {
        const HANDLE waitFor[] = {server.process.get(), stop};
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, waitFor, FALSE, 100);
            if (wait == WAIT_OBJECT_0) break;
            const auto competing = HasOtherMutexHandle(gate.get());
            if (wait == WAIT_OBJECT_0 + 1 || competing == true) {
                requested = true;
                SetEvent(server.stop.get());
                if (WaitForSingleObject(server.process.get(), 200) != WAIT_OBJECT_0) {
                    TerminateProcess(server.process.get(), 1);
                }
                break;
            }
            if (wait != WAIT_TIMEOUT || !competing.has_value()) {
                TerminateProcess(server.process.get(), 1);
                break;
            }
        }
    } else {
        // The worker is still suspended and has never owned an endpoint.
        if (requested) code = 0;
        else if (nvda == true) code = ServerStandAside;
        TerminateProcess(server.process.get(), code);
    }
    WaitForSingleObject(server.process.get(), INFINITE);
    // Release on this thread, only after the worker's endpoint is gone.
    if (gate) ReleaseMutex(gate.get());
    if (!gate) log.Write(LogLevel::Info, L"Standing aside because the NVDA startup mutex is unavailable");
    if (requested) log.Write(LogLevel::Info, L"Released the controller after a stop or NVDA startup request");
    GetExitCodeProcess(server.process.get(), &code);
    // A stop or handover ends this game's queue. Keep it across a worker crash.
    if (requested || code == 0 || code == ServerStandAside) journal.reset();
    return code;
}

ULONGLONG SignInDeadline(const Settings& settings, Log& log) {
    ULONGLONG remaining = 0;
    std::wstring decision;
    std::optional<bool> configured = settings.startsAtSignIn == SignInStart::Yes;
    if (settings.signInWaitSeconds && settings.startsAtSignIn == SignInStart::Auto) {
        configured = NvdaConfiguredAtSignIn();
    }
    if (!settings.signInWaitSeconds) {
        decision = L"disabled by signInWaitSeconds=0";
    } else if (!configured.value_or(true)) {
        decision = settings.startsAtSignIn == SignInStart::No ? L"disabled by startsAtSignIn=no"
                                                              : L"NVDA is not configured to start at sign-in";
    } else {
        const auto age = LogonAgeMs();
        const auto limit = static_cast<ULONGLONG>(settings.signInWaitSeconds) * 1000;
        remaining = limit - std::min(age.value_or(0), limit);
        decision = remaining ? L"waiting up to " + std::to_wstring(remaining) + L" ms for NVDA"
                             : L"the configured wait since sign-in has elapsed";
        if (!age) decision += L" (logon time unavailable)";
        if (!configured.has_value()) decision += L" (sign-in setting unreadable)";
    }
    log.Write(LogLevel::Info, L"Sign-in wait: " + decision);
    return GetTickCount64() + remaining;
}

[[noreturn]] void Supervise(HANDLE stop, const Settings& settings,
                            const std::filesystem::path& executable, Log& log) {
    log.Write(LogLevel::Info, L"Started");
    DWORD backoff = 1000;
    DWORD waitingPid = 0;
    unsigned consecutiveSpeechFailures = 0;
    SpeechJournalMapping journal;
    auto signInDeadline = SignInDeadline(settings, log);
    while (!Stopping(stop)) {
        const auto nvda = FindNvda(settings, executable);
        if (nvda.pid) {
            signInDeadline = 0;
            backoff = 1000;
            consecutiveSpeechFailures = 0;
            if (waitingPid != nvda.pid) {
                if (journal.handle()) journal.reset();
                log.Write(LogLevel::Info, L"Waiting for NVDA process " + std::to_wstring(nvda.pid));
                if (!nvda.handle) {
                    log.Write(LogLevel::Error, L"Cannot open the NVDA process for synchronization");
                }
                waitingPid = nvda.pid;
            }
            if (nvda.handle) {
                const HANDLE handles[] = {stop, nvda.handle.get()};
                if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_FAILED) {
                    Fail(log, L"Could not wait for NVDA", GetLastError());
                }
            } else {
                WaitForSingleObject(stop, NvdaCheckMs);
            }
            continue;
        }

        waitingPid = 0;
        if (Stopping(stop)) break;
        const auto started = GetTickCount64();
        if (started < signInDeadline) {
            WaitForSingleObject(stop, static_cast<DWORD>(std::min<ULONGLONG>(signInDeadline - started, NvdaCheckMs)));
            continue;
        }
        DWORD code = 1;
        {
            // Direct --serve and the supervisor share this one serving slot.
            const Handle slot(CreateMutexW(nullptr, TRUE, ServerMutex));
            if (slot && GetLastError() != ERROR_ALREADY_EXISTS) {
                code = RunServer(stop, executable, log, journal);
                log.Write(code != 0 && code != ServerStandAside ? LogLevel::Error : LogLevel::Info,
                          L"Server exited with code " + std::to_wstring(code));
            }
        }
        if (Stopping(stop)) break;
        const auto ran = GetTickCount64() - started;
        if (ran >= HealthyRunMs) {
            // Rare failures far apart must never add up to a lost queue or a long wait.
            consecutiveSpeechFailures = 0;
            backoff = 1000;
        }
        const bool speechFailure = code == SpeechWorkersStuck || (code & 0xC0000000) == 0xC0000000;
        const bool cleanExit = code == 0 || code == ServerStandAside;
        if (speechFailure) {
            consecutiveSpeechFailures = std::min(consecutiveSpeechFailures + 1, 2U);
            if (consecutiveSpeechFailures == 2) {
                log.Write(LogLevel::Error, L"Discarding pending speech after consecutive server failures.");
                journal.reset();
            }
        } else if (cleanExit) {
            consecutiveSpeechFailures = 0;
        }

        // A watchdog exit calls for immediate recovery on its first failure.
        // A startup probe timeout leaves the queue and speech failure count intact.
        const bool firstFailure = speechFailure && consecutiveSpeechFailures == 1;
        const bool ranSteadily = ran >= 5000;
        if ((firstFailure && (code == SpeechWorkersStuck || ranSteadily)) || (cleanExit && ranSteadily)) {
            backoff = 1000;
        } else {
            log.Write(LogLevel::Info, L"Retrying server after " + std::to_wstring(backoff) + L" ms");
            WaitForSingleObject(stop, backoff);
            backoff = std::min(backoff * 2, 60000UL);
        }
    }
    log.Write(LogLevel::Info, L"Stopping after stop event");
    Finish(log, 0);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE || (message == WM_ENDSESSION && wParam)) {
        PostQuitMessage(message);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct ServerStartup {
    const std::wstring& endpoint;
    const SpeakerConfig& config;
    Log& log;
    SpeakerLog speakerLog;
    SpeechJournal& journal;
    HANDLE exhausted;
    std::mutex mutex;
    ISpeaker* speaker = nullptr;
    bool stopping = false;

    ServerStartup(const std::wstring& endpoint, const Settings& settings, Log& log, SpeechJournal& journal, HANDLE exhausted)
        : endpoint(endpoint), config(settings.speaker), log(log), speakerLog(log, settings.logText),
          journal(journal), exhausted(exhausted) {}
};

DWORD WINAPI StartServing(void* state) {
    auto& startup = *static_cast<ServerStartup*>(state);
    try {
        auto speaker = CreateServerSpeaker(startup.config, startup.speakerLog, startup.journal, startup.exhausted);
        {
            const std::lock_guard<std::mutex> lock(startup.mutex);
            if (!startup.stopping) {
                // Retain it until process exit, including for any in-flight calls.
                startup.speaker = speaker.release();
                SetControllerSpeaker(startup.speaker);
            }
        }
        if (speaker) {
            speaker->Shutdown();
        } else {
            auto status = RegisterController(startup.endpoint);
            if (status != RPC_S_OK) return status;
            startup.log.Write(LogLevel::Info, L"Serving " + startup.endpoint);
        }
    } catch (...) {
        return ERROR_FUNCTION_FAILED;
    }
    return RPC_S_OK;
}

DWORD WINAPI ShutdownSpeaker(void* speaker) {
    try { static_cast<ISpeaker*>(speaker)->Shutdown(); } catch (...) { return 1; }
    return 0;
}

[[noreturn]] void StopServing(ServerStartup& startup, const std::wstring& reason, DWORD code = 0) {
    ISpeaker* speaker;
    {
        const std::lock_guard<std::mutex> lock(startup.mutex);
        startup.stopping = true;
        SetControllerSpeaker(nullptr);
        speaker = startup.speaker;
    }
    if (speaker) {
        const Handle shutdown(CreateThread(nullptr, 0, ShutdownSpeaker, speaker, 0, nullptr));
        if (!shutdown || WaitForSingleObject(shutdown.get(), 100) != WAIT_OBJECT_0) {
            startup.log.Write(LogLevel::Error, L"Speaker shutdown exceeded its deadline or failed");
        }
    }
    startup.log.Write(LogLevel::Info, L"Stopping: " + reason);
    Finish(startup.log, code);
}

DWORD WINAPI ProbeController(void* endpoint) {
#ifdef VB_RESTART_TEST
    BeforeRestartTestControllerProbe();
#endif
    return ControllerAnswers(*static_cast<std::wstring*>(endpoint)) ? 1 : 0;
}

[[noreturn]] void Serve(HINSTANCE instance, HANDLE stop, const Settings& settings,
                        const std::filesystem::path& executable, Log& log, SpeechJournal& journal) {
    log.Write(LogLevel::Info, L"Started");
    if (const auto nvda = FindNvda(settings, executable); nvda.pid) {
        StandAside(log, L"NVDA process " + std::to_wstring(nvda.pid) + L" is running");
    }
    if (HasExternalNvdaWindow(executable)) StandAside(log, L"the NVDA window already exists");
    auto endpoint = EndpointName();
    if (endpoint.empty()) Fail(log, L"Could not obtain the desktop endpoint", ERROR_INVALID_NAME);
    const Handle probe(CreateThread(nullptr, 0, ProbeController, &endpoint, 0, nullptr));
    if (!probe) Fail(log, L"Could not start the controller probe", GetLastError());
    const HANDLE startupHandles[] = {stop, probe.get()};
    const DWORD probeWait = WaitForMultipleObjects(2, startupHandles, FALSE, 175);
    if (probeWait == WAIT_FAILED) Fail(log, L"Could not wait for the controller probe", GetLastError());
    if (probeWait == WAIT_TIMEOUT) {
        log.Write(LogLevel::Error, L"The controller probe timed out at " + endpoint);
        Finish(log, ControllerProbeTimedOut);
    }
    if (probeWait == WAIT_OBJECT_0 + 1) {
        DWORD answered = 0;
        GetExitCodeThread(probe.get(), &answered);
        if (answered) StandAside(log, L"the controller answered at " + endpoint);
    }
    if (Stopping(stop)) {
        log.Write(LogLevel::Info, L"Stopping after stop event");
        Finish(log, 0);
    }

    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        log.Write(LogLevel::Error, L"Could not raise server priority (" + std::to_wstring(GetLastError()) + L")");
    }
    if (!ChangeWindowMessageFilter(WM_QUIT, MSGFLT_ALLOW)) {
        log.Write(LogLevel::Error, L"Could not open the WM_QUIT message filter (" +
                                      std::to_wstring(GetLastError()) + L")");
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = NvdaWindowClass;
    if (!RegisterClassW(&windowClass)) Fail(log, L"Could not register the VoiceBridge window class", GetLastError());
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, NvdaWindowClass, NvdaWindowTitle, WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance, nullptr)) {
        Fail(log, L"Could not create the VoiceBridge window", GetLastError());
    }

    Handle exhausted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!exhausted) Fail(log, L"Could not create the speech watchdog event", GetLastError());
    ServerStartup startup(endpoint, settings, log, journal, exhausted.get());
    Handle worker(CreateThread(nullptr, 0, StartServing, &startup, 0, nullptr));
    if (!worker) Fail(log, L"Could not start RPC and speaker worker", GetLastError());
    auto nextScan = GetTickCount64() + NvdaCheckMs;
    for (;;) {
        if (Stopping(stop)) StopServing(startup, L"stop event");
        if (Stopping(exhausted.get())) {
            log.Write(LogLevel::Error, L"All four SAPI workers exceeded their watchdog while speech was waiting. Restarting the server.");
            Finish(log, SpeechWorkersStuck);
        }
        MSG message;
        for (int count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                const wchar_t* reason = message.wParam == WM_CLOSE ? L"WM_CLOSE" :
                    message.wParam == WM_ENDSESSION ? L"WM_ENDSESSION" : L"WM_QUIT";
                StopServing(startup, reason);
            }
            DispatchMessageW(&message);
        }

        auto now = GetTickCount64();
        if (now >= nextScan) {
            if (const auto nvda = FindNvda(settings, executable, true); nvda.pid) {
                StopServing(startup, L"NVDA process " + std::to_wstring(nvda.pid) + L" appeared");
            }
            nextScan = now + NvdaCheckMs;
        }
        if (worker && WaitForSingleObject(worker.get(), 0) == WAIT_OBJECT_0) {
            DWORD status = ERROR_FUNCTION_FAILED;
            GetExitCodeThread(worker.get(), &status);
            worker.reset();
            if (status == RPC_S_DUPLICATE_ENDPOINT) {
                StopServing(startup, L"standing aside because the endpoint is already bound at " + endpoint,
                            ServerStandAside);
            }
            if (status != RPC_S_OK) {
                log.Write(LogLevel::Error, L"Startup failed at " + endpoint + L" (" + std::to_wstring(status) + L")");
                StopServing(startup, L"startup failed", 1);
            }
        }

        const HANDLE handles[] = {stop, exhausted.get(), worker.get()};
        now = GetTickCount64();
        const DWORD remaining = now >= nextScan ? 0 : static_cast<DWORD>(nextScan - now);
        if (MsgWaitForMultipleObjectsEx(worker ? 3 : 2, handles, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
            == WAIT_FAILED) {
            StopServing(startup, L"message wait failed (" + std::to_wstring(GetLastError()) + L")", 1);
        }
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int count = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return 1;
    const bool stopping = count == 2 && wcscmp(argv[1], L"--stop") == 0;
    const bool server = count == 2 && wcscmp(argv[1], L"--serve") == 0;
    const bool worker = count == 4 && wcscmp(argv[1], L"--rpc-worker") == 0;
    HANDLE workerStop = nullptr;
    SpeechJournal journal;
    if (worker) {
        auto inherited = [](const wchar_t* argument) -> HANDLE {
            wchar_t* end = nullptr;
            auto handle = reinterpret_cast<HANDLE>(wcstoull(argument, &end, 10));
            DWORD flags = 0;
            return !*end && GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) ? handle : nullptr;
        };
        workerStop = inherited(argv[2]);
        const auto mapping = inherited(argv[3]);
        if (!workerStop || !mapping || !journal.map(mapping)) {
            LocalFree(argv);
            return 2;
        }
    }
    LocalFree(argv);
    if (stopping) return Stop();
    if (count != 1 && !server && !worker) return 2;

    Handle stop;
    Handle role;
    bool roleExists = false;
    if (!worker) {
        stop.reset(CreateEventW(nullptr, TRUE, FALSE, StopEventName));
        if (!stop) return 1;
        role.reset(CreateMutexW(nullptr, TRUE, server ? ServerMutex : SupervisorMutex));
        roleExists = GetLastError() == ERROR_ALREADY_EXISTS;
        if (!role) return 1;
        if ((!server && roleExists) || Stopping(stop.get())) return 0;
    }
    const auto executable = ExecutablePath();
    if (executable.empty()) return 1;
    const auto directory = executable.parent_path();
    const auto settings = ReadSettings(directory);
    if (worker) {
        Log log(directory, L"server", settings.level);
        Serve(instance, workerStop, settings, executable, log, journal);
    }
    Log log(directory, server ? L"guard" : L"supervisor", settings.level);
    if (server) {
        if (roleExists) StandAside(log, L"a VoiceBridge server is already running");
        SpeechJournalMapping directJournal;
        Finish(log, RunServer(stop.get(), executable, log, directJournal));
    }
    Supervise(stop.get(), settings, executable, log);
}
