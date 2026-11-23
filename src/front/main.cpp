#include "log.h"
#include "process.h"
#include "controller_rpc.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdlib>

namespace {
using namespace vb;

constexpr wchar_t WindowClass[] = L"VoiceBridge.ServerWindow";
constexpr wchar_t WindowTitle[] = L"VoiceBridge";
constexpr DWORD NvdaCheckMs = 500;

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
    Finish(log, 3);
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
DWORD RunServer(HANDLE stop, const std::filesystem::path& executable, Log& log) {
    const auto desktop = DesktopName();
    if (desktop.empty()) return ERROR_INVALID_NAME;
    const auto name = L"Local\\NVDA_" + desktop;
    auto server = StartServer(executable);
    if (!server.process) {
        log.Write(LogLevel::Error, L"Could not start server (" + std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    const auto gate = CreateNvdaStartupMutex(name);
    DWORD code = 3;
    bool requested = false;
    if (gate && !HasOtherMutexHandle(gate.get()) && !Stopping(stop) &&
        ResumeThread(server.thread.get()) != static_cast<DWORD>(-1)) {
        const HANDLE waitFor[] = {server.process.get(), stop};
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, waitFor, FALSE, 100);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_TIMEOUT || HasOtherMutexHandle(gate.get())) {
                requested = true;
                SetEvent(server.stop.get());
                if (WaitForSingleObject(server.process.get(), 200) != WAIT_OBJECT_0) {
                    TerminateProcess(server.process.get(), 1);
                }
                break;
            }
        }
    } else {
        // The worker is still suspended and has never owned an endpoint.
        TerminateProcess(server.process.get(), code);
    }
    WaitForSingleObject(server.process.get(), INFINITE);
    // Release on this thread, only after the worker's endpoint is gone.
    if (gate) ReleaseMutex(gate.get());
    if (!gate) log.Write(LogLevel::Info, L"Standing aside because the NVDA startup mutex is unavailable");
    if (requested) log.Write(LogLevel::Info, L"Released the controller after a stop or NVDA startup request");
    GetExitCodeProcess(server.process.get(), &code);
    return code;
}

[[noreturn]] void Supervise(HANDLE stop, const Settings& settings,
                            const std::filesystem::path& executable, Log& log) {
    log.Write(LogLevel::Info, L"Started");
    DWORD backoff = 1000;
    DWORD waitingPid = 0;
    while (!Stopping(stop)) {
        const auto nvda = FindNvda(settings, executable);
        if (nvda.pid) {
            backoff = 1000;
            if (waitingPid != nvda.pid) {
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
        {
            // Direct --serve and the supervisor share this one serving slot.
            const Handle slot(CreateMutexW(nullptr, TRUE, ServerMutex));
            if (slot && GetLastError() != ERROR_ALREADY_EXISTS) {
                const auto code = RunServer(stop, executable, log);
                log.Write(code != 0 && code != 3 ? LogLevel::Error : LogLevel::Info,
                          L"Server exited with code " + std::to_wstring(code));
            }
        }
        if (Stopping(stop)) break;
        if (GetTickCount64() - started >= 5000) {
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
    std::mutex mutex;
    ISpeaker* speaker = nullptr;
    bool stopping = false;

    ServerStartup(const std::wstring& endpoint, const Settings& settings, Log& log)
        : endpoint(endpoint), config(settings.speaker), log(log), speakerLog(log, settings.logText) {}
};

DWORD WINAPI StartServing(void* state) {
    auto& startup = *static_cast<ServerStartup*>(state);
    try {
        auto speaker = CreateSpeaker(startup.config, startup.speakerLog);
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
    return ControllerAnswers(*static_cast<std::wstring*>(endpoint)) ? 1 : 0;
}

[[noreturn]] void Serve(HINSTANCE instance, HANDLE stop, const Settings& settings,
                        const std::filesystem::path& executable, Log& log) {
    log.Write(LogLevel::Info, L"Started");
    if (const auto nvda = FindNvda(settings, executable); nvda.pid) {
        StandAside(log, L"NVDA process " + std::to_wstring(nvda.pid) + L" is running");
    }
    if (FindWindowW(L"wxWindowClassNR", L"NVDA")) StandAside(log, L"the NVDA window already exists");
    auto endpoint = EndpointName();
    if (endpoint.empty()) Fail(log, L"Could not obtain the desktop endpoint", ERROR_INVALID_NAME);
    const Handle probe(CreateThread(nullptr, 0, ProbeController, &endpoint, 0, nullptr));
    if (!probe) Fail(log, L"Could not start the controller probe", GetLastError());
    const HANDLE startupHandles[] = {stop, probe.get()};
    const DWORD probeWait = WaitForMultipleObjects(2, startupHandles, FALSE, 175);
    if (probeWait == WAIT_FAILED) Fail(log, L"Could not wait for the controller probe", GetLastError());
    if (probeWait == WAIT_TIMEOUT) StandAside(log, L"the controller probe timed out at " + endpoint);
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
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassW(&windowClass)) Fail(log, L"Could not register the VoiceBridge window class", GetLastError());
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WindowClass, WindowTitle, WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance, nullptr)) {
        Fail(log, L"Could not create the VoiceBridge window", GetLastError());
    }

    ServerStartup startup(endpoint, settings, log);
    Handle worker(CreateThread(nullptr, 0, StartServing, &startup, 0, nullptr));
    if (!worker) Fail(log, L"Could not start RPC and speaker worker", GetLastError());
    auto nextScan = GetTickCount64() + NvdaCheckMs;
    for (;;) {
        if (Stopping(stop)) StopServing(startup, L"stop event");
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
            if (const auto nvda = FindNvda(settings, executable); nvda.pid) {
                StopServing(startup, L"NVDA process " + std::to_wstring(nvda.pid) + L" appeared");
            }
            nextScan = now + NvdaCheckMs;
        }
        if (worker && WaitForSingleObject(worker.get(), 0) == WAIT_OBJECT_0) {
            DWORD status = ERROR_FUNCTION_FAILED;
            GetExitCodeThread(worker.get(), &status);
            worker.reset();
            if (status == RPC_S_DUPLICATE_ENDPOINT) {
                StopServing(startup, L"standing aside because the endpoint is already bound at " + endpoint, 3);
            }
            if (status != RPC_S_OK) {
                log.Write(LogLevel::Error, L"Startup failed at " + endpoint + L" (" + std::to_wstring(status) + L")");
                StopServing(startup, L"startup failed", 1);
            }
        }

        const HANDLE handles[] = {stop, worker.get()};
        now = GetTickCount64();
        const DWORD remaining = now >= nextScan ? 0 : static_cast<DWORD>(nextScan - now);
        if (MsgWaitForMultipleObjectsEx(worker ? 2 : 1, handles, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
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
    const bool worker = count == 3 && wcscmp(argv[1], L"--rpc-worker") == 0;
    HANDLE workerStop = nullptr;
    if (worker) {
        wchar_t* end = nullptr;
        workerStop = reinterpret_cast<HANDLE>(wcstoull(argv[2], &end, 10));
        DWORD flags = 0;
        if (*end || !GetHandleInformation(workerStop, &flags) || !(flags & HANDLE_FLAG_INHERIT)) {
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
        Serve(instance, workerStop, settings, executable, log);
    }
    Log log(directory, server ? L"guard" : L"supervisor", settings.level);
    if (server) {
        if (roleExists) StandAside(log, L"a VoiceBridge server is already running");
        Finish(log, RunServer(stop.get(), executable, log));
    }
    Supervise(stop.get(), settings, executable, log);
}
