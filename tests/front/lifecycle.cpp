#include "test-process.h"
#include "controller_rpc.h"

#include <chrono>
#include <cwchar>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Child {
    vb::Handle handle;
    DWORD pid;
    ~Child() {
        // Only the test executable launched by this object can be terminated.
        if (WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(handle.get(), 0);
            WaitForSingleObject(handle.get(), 3000);
        }
    }
};

Child Start(const wchar_t* program, const wchar_t* arguments) {
    const auto path = TestPath(program);
    auto command = L"\"" + path + L"\" " + arguments;
    PROCESS_INFORMATION process{};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process), "start child");
    CloseHandle(process.hThread);
    return {vb::Handle(process.hProcess), process.dwProcessId};
}

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void PostToWorker(HWND window, HANDLE worker, UINT message, WPARAM parameter = 0) {
    Require(WaitForSingleObject(worker, 0) == WAIT_TIMEOUT && Owner(window) == GetProcessId(worker),
            "the exact test worker still owns the window");
    Require(PostMessageW(window, message, parameter, 0), "post message to the test worker");
}

double WaitForExit(HANDLE process, Clock::time_point since, DWORD timeout) {
    Require(WaitForSingleObject(process, timeout) == WAIT_OBJECT_0, "process exit deadline");
    return Milliseconds(Clock::now() - since);
}

struct Windows {
    DWORD guardPid;
    DWORD workerPid;
    unsigned visible = 0;
};

BOOL CALLBACK CountVisible(HWND window, LPARAM context) {
    auto& windows = *reinterpret_cast<Windows*>(context);
    const auto owner = Owner(window);
    if ((owner == windows.guardPid || owner == windows.workerPid) && IsWindowVisible(window)) {
        ++windows.visible;
    }
    return TRUE;
}

void CheckNoConsole(DWORD pid) {
    if (AttachConsole(pid)) {
        FreeConsole();
        throw std::runtime_error("VoiceBridge had a console");
    }
    Require(GetLastError() == ERROR_INVALID_HANDLE, "verify absence of console");
}

void CheckWindow(FILE* result) {
    const auto foreground = GetForegroundWindow();
    Require(!NvdaWindow(), "no NVDA window in test desktop");
    const auto guard = Start(L"VoiceBridge.exe", L"--serve");
    const auto deadline = Clock::now() + 4s;
    HWND window = nullptr;
    bool ready = false;
    do {
        window = NvdaWindow();
        Require(GetForegroundWindow() == foreground, "foreground changed during startup");
        Require(WaitForSingleObject(guard.handle.get(), 0) == WAIT_TIMEOUT, "guard exited during startup");
        if (window) {
            Require(Owner(window) != guard.pid, "the guard must not own the NVDA window");
            const auto worker = OpenTestProcess(Owner(window), L"VoiceBridge.exe");
            ready = vb::ControllerAnswers(vb::EndpointName());
        }
        if (ready) break;
        Sleep(10);
    } while (Clock::now() < deadline);

    const auto workerPid = Owner(window);
    Require(ready && workerPid && workerPid != guard.pid, "hidden worker window is discoverable");
    const auto worker = OpenTestProcess(workerPid, L"VoiceBridge.exe");
    Require(GetPriorityClass(worker.get()) == ABOVE_NORMAL_PRIORITY_CLASS, "above-normal server priority");
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    Require((style & WS_EX_TOOLWINDOW) && (style & WS_EX_NOACTIVATE), "window flags");
    Require(GetAncestor(window, GA_ROOT) == window && GetParent(window) == nullptr, "top-level window");
    Windows windows{guard.pid, workerPid};
    EnumWindows(CountVisible, reinterpret_cast<LPARAM>(&windows));
    Require(!IsWindowVisible(window) && !windows.visible, "no visible VoiceBridge windows");
    CheckNoConsole(guard.pid);
    CheckNoConsole(workerPid);
    Require(GetForegroundWindow() == foreground, "foreground changed");
    const auto since = Clock::now();
    PostToWorker(window, worker.get(), WM_CLOSE);
    const auto workerElapsed = WaitForExit(worker.get(), since, 250);
    const auto guardElapsed = WaitForExit(guard.handle.get(), since, 1000);
    DWORD code = 1;
    Require(GetExitCodeProcess(guard.handle.get(), &code) && code == 0, "guard exit code after WM_CLOSE");
    Require(GetForegroundWindow() == foreground, "foreground changed on close");
    fprintf(result, "Exact hidden NVDA window, no console, unchanged foreground. WM_CLOSE stopped the worker in "
                    "%.2f ms and the guard in %.2f ms.\n", workerElapsed, guardElapsed);
}

void CheckStartupPending(FILE* result, DWORD guardPid) {
    const auto guard = OpenTestProcess(guardPid, L"stuck-bridge.exe");
    const auto deadline = Clock::now() + 3s;
    HWND window = nullptr;
    do {
        Require(WaitForSingleObject(guard.get(), 0) == WAIT_TIMEOUT, "guard still lives during startup");
        window = NvdaWindow();
        if (window) break;
        Sleep(10);
    } while (Clock::now() < deadline);
    const auto workerPid = Owner(window);
    Require(workerPid && workerPid != guardPid, "find the pending worker's NVDA window");
    const auto worker = OpenTestProcess(workerPid, L"stuck-bridge.exe");
    const auto endpoint = vb::EndpointName();
    Require(!endpoint.empty(), "find the pending worker's endpoint name");
    Require(!vb::ControllerAnswers(endpoint), "RPC must not answer while the speaker is starting");
    Sleep(150);
    Require(WaitForSingleObject(guard.get(), 0) == WAIT_TIMEOUT && Owner(NvdaWindow()) == workerPid &&
                WaitForSingleObject(worker.get(), 0) == WAIT_TIMEOUT,
            "guard and worker remained alive with a stalled speaker");
    Require(!vb::ControllerAnswers(endpoint), "RPC must still be unavailable after 150 ms");
    fputs("The stalled speaker kept RPC unavailable while the guard and worker stayed alive.\n", result);
}

void RequestExit(FILE* result, DWORD pid, UINT message, bool stalled = false) {
    const auto window = NvdaWindow();
    Require(window && Owner(window) == pid, "expected VoiceBridge window");
    const auto process = OpenTestProcess(pid, stalled ? L"stuck-bridge.exe" : L"VoiceBridge.exe");
    using Speak = unsigned long (WINAPI*)(const wchar_t*);
    Speak speak = nullptr;
    vb::Handle shutdownEntered;
    if (stalled) {
        shutdownEntered.reset(OpenEventW(SYNCHRONIZE, FALSE, L"Local\\VoiceBridge.Test.ShutdownEntered"));
        Require(!!shutdownEntered, "open speaker shutdown signal");
        const auto client = LoadLibraryW(L".\\nvdaControllerClient64.dll");
        Require(client != nullptr, "load stock client during shutdown test");
        speak = Load<Speak>(client, "nvdaController_speakText");
        Require(speak(L"Before shutdown") == 0, "speech available before shutdown");
    }
    if (message == WM_ENDSESSION) {
        PostToWorker(window, process.get(), message, FALSE);
        Require(WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT &&
                vb::ControllerAnswers(vb::EndpointName()), "bridge survived canceled shutdown");
    }
    const auto since = Clock::now();
    PostToWorker(window, process.get(), message, message == WM_ENDSESSION ? TRUE : 0);
    if (stalled) {
        Require(WaitForSingleObject(shutdownEntered.get(), 250) == WAIT_OBJECT_0, "speaker entered Shutdown");
        Require(speak(L"During shutdown") == ERROR_NOT_READY, "detached speaker did not acknowledge speech");
    }
    const auto elapsed = WaitForExit(process.get(), since, 250);
    Require(elapsed <= 250, "worker released the endpoint within 250 ms");
    fprintf(result, "%s exit: %.2f ms\n", message == WM_QUIT ? "WM_QUIT" : "WM_ENDSESSION", elapsed);
    if (message == WM_QUIT) {
        const auto status = vb::RegisterController(vb::EndpointName());
        fprintf(result, "Re-registering the same endpoint returned %ld\n", status);
        Require(status == RPC_S_OK, "RPC endpoint released");
    }
}

void Appear(FILE* result, DWORD pid) {
    Require(Owner(NvdaWindow()) == pid, "expected a serving child");
    const auto prior = OpenTestProcess(pid, L"VoiceBridge.exe");
    const auto started = Clock::now();
    const auto fake = Start(L"vb-fake-nvda.exe", L"");
    const auto exit = WaitForExit(prior.get(), started, 750);
    Require(exit <= 750, "server exited within 750 ms of launch");
    const auto heldUntil = Clock::now() + 2500ms;
    do {
        Require(!NvdaWindow(), "bridge was present while fake NVDA was running");
        Require(WaitForSingleObject(fake.handle.get(), 0) == WAIT_TIMEOUT, "fake process still running");
        Sleep(40);
    } while (Clock::now() < heldUntil);

    Require(TerminateProcess(fake.handle.get(), 0), "end test fixture");
    Require(WaitForSingleObject(fake.handle.get(), 1000) == WAIT_OBJECT_0, "fake exited");
    const auto stopped = Clock::now();
    const auto deadline = stopped + 2s;
    bool ready = false;
    do {
        const auto next = Owner(NvdaWindow());
        if (next && next != pid) {
            const auto worker = OpenTestProcess(next, L"VoiceBridge.exe");
            ready = vb::ControllerAnswers(vb::EndpointName());
        }
        if (ready) break;
        Sleep(10);
    } while (Clock::now() < deadline);
    const auto restart = Milliseconds(Clock::now() - stopped);
    Require(ready && restart <= 2000, "supervisor restored service within two seconds");
    fprintf(result, "Same-session fake NVDA: stopped serving in %.2f ms, stood aside for 2.5 s, restored in %.2f ms\n",
            exit, restart);
}

class NvdaStartupMutex {
public:
    explicit NvdaStartupMutex(bool expectExisting = false) {
        wchar_t desktop[256]{};
        Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                          desktop, sizeof(desktop), nullptr), "get NVDA desktop name");
        const auto name = std::wstring(L"Local\\NVDA_") + desktop;
        mutex_.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        Require(!!mutex_, "open NVDA startup mutex");
        Require(!expectExisting || existed, "open the startup mutex the guard already owns");
    }

    ~NvdaStartupMutex() {
        if (owned_) ReleaseMutex(mutex_.get());
    }

    void Acquire() {
        const DWORD wait = WaitForSingleObject(mutex_.get(), 2000);
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        Require(wait == WAIT_OBJECT_0, "NVDA startup mutex was released normally within two seconds");
    }

private:
    vb::Handle mutex_;
    bool owned_ = false;
};

void StartNvdaAfterMissedWindow(FILE* result) {
    Require(!NvdaWindow(), "NVDA's initial window lookup found no window");
    const auto guard = Start(L"VoiceBridge.exe", L"--serve");
    const auto endpoint = vb::EndpointName();
    const auto deadline = Clock::now() + 4s;
    vb::Handle worker;
    do {
        Require(WaitForSingleObject(guard.handle.get(), 0) == WAIT_TIMEOUT, "the guard stayed alive during startup");
        if (const auto pid = Owner(NvdaWindow())) {
            Require(pid != guard.pid, "the guard does not own the NVDA window");
            auto candidate = OpenTestProcess(pid, L"VoiceBridge.exe");
            if (vb::ControllerAnswers(endpoint)) {
                worker = std::move(candidate);
                break;
            }
        }
        Sleep(10);
    } while (Clock::now() < deadline);
    Require(!!worker, "VoiceBridge started serving after NVDA's initial lookup");

    // Normal and ease-of-access startup both take this branch after a miss.
    const auto started = Clock::now();
    NvdaStartupMutex mutex(true);
    mutex.Acquire();
    Require(WaitForSingleObject(worker.get(), 0) == WAIT_OBJECT_0, "worker exited before the mutex was released");
    Require(vb::RegisterController(endpoint) == RPC_S_OK, "NVDA registered RPC after acquiring the mutex");
    const auto elapsed = Milliseconds(Clock::now() - started);
    const auto guardElapsed = WaitForExit(guard.handle.get(), started, 1000);
    DWORD code = 1;
    Require(GetExitCodeProcess(guard.handle.get(), &code) && code == 0, "guard exited cleanly");
    Require(elapsed <= 750, "prompt handover after the window lookup missed");
    fprintf(result, "NVDA's initial lookup found no window. After the bridge started serving, the fixture acquired "
                    "the startup mutex and registered RPC in %.2f ms without WM_QUIT. The worker had already exited "
                    "and the guard exited in %.2f ms.\n", elapsed, guardElapsed);
}

vb::Handle RestartEvent(DWORD run, const wchar_t* step) {
    const auto name = L"Local\\VoiceBridge.Test.Restart." + std::to_wstring(run) + L"." + step;
    vb::Handle event(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
    Require(!!event, "open NVDA restart fixture event");
    return event;
}

void StartNvda(FILE* result, const std::wstring& mode, DWORD pid, DWORD run) {
    const bool replacing = mode == L"restart-next";
    const bool first = mode == L"restart-first";
    const auto started = Clock::now();
    const auto window = NvdaWindow();
    const auto lookup = Clock::now();
    if (!replacing) Require(Owner(window) == pid, "NVDA found the expected serving worker");
    const auto prior = OpenTestProcess(pid, replacing ? L"lifecycle.exe" : L"VoiceBridge.exe");
    if (mode == L"normal-start") {
        const auto request = Clock::now();
        PostToWorker(window, prior.get(), WM_QUIT);
        const auto posted = Clock::now();
        const auto exit = WaitForExit(prior.get(), posted, 4000);
        const auto mutexStarted = Clock::now();
        NvdaStartupMutex mutex;
        const auto mutexOpened = Clock::now();
        mutex.Acquire();
        const auto mutexAcquired = Clock::now();
        const auto status = vb::RegisterController(vb::EndpointName());
        const auto registered = Clock::now();
        const auto total = Milliseconds(registered - started);
        fprintf(result, "Normal NVDA startup found the window in %.2f ms, posted WM_QUIT in %.2f ms, "
                        "observed worker exit %.2f ms later, opened the mutex in %.2f ms, "
                        "acquired it in %.2f ms and registered RPC in %.2f ms (%ld). Total %.2f ms.\n",
                Milliseconds(lookup - started), Milliseconds(posted - request), exit,
                Milliseconds(mutexOpened - mutexStarted), Milliseconds(mutexAcquired - mutexOpened),
                Milliseconds(registered - mutexAcquired), status, total);
        Require(status == RPC_S_OK && exit <= 250 && total <= 750, "prompt normal NVDA handover");
        Sleep(650);
        Require(!NvdaWindow(), "VoiceBridge stayed out while normal NVDA owned the endpoint");
        return;
    }
    // The first process exercises the mutex path where NVDA looked up the
    // window before the worker created it, so it never posted WM_QUIT.
    if (replacing) Require(!window, "no bridge window during the NVDA fixture restart");
    NvdaStartupMutex mutex;
    if (replacing) {
        const auto waiting = RestartEvent(run, L"NextWaiting");
        Require(SetEvent(waiting.get()), "signal that replacement opened the startup mutex");
    }
    mutex.Acquire();
    if (replacing) {
        // The retiring fixture releases ownership normally before exiting.
        // ALPC closes its endpoint with the process, so wait for that exit.
        WaitForExit(prior.get(), started, 1000);
    }
    Require(vb::RegisterController(vb::EndpointName()) == RPC_S_OK, "NVDA fixture bound the desktop endpoint");
    Require(WaitForSingleObject(prior.get(), 0) == WAIT_OBJECT_0, "prior process had already released endpoint");
    Require(!NvdaWindow(), "VoiceBridge is standing aside after NVDA took the endpoint");
    fprintf(result, "NVDA fixture acquired its mutex and bound the endpoint in %.2f ms.\n",
            Milliseconds(Clock::now() - started));
    if (first) {
        const auto ready = RestartEvent(run, L"FirstReady");
        const auto waiting = RestartEvent(run, L"NextWaiting");
        Require(SetEvent(ready.get()), "signal first NVDA fixture ready");
        Require(WaitForSingleObject(waiting.get(), 3000) == WAIT_OBJECT_0, "replacement opened the startup mutex");
    } else {
        Sleep(650);
        Require(!NvdaWindow(), "VoiceBridge stayed out while NVDA owned the endpoint");
    }
}

void RestartNvda(FILE* result, DWORD workerPid) {
    const DWORD run = GetCurrentProcessId();
    const auto ready = RestartEvent(run, L"FirstReady");
    const auto waiting = RestartEvent(run, L"NextWaiting");
    const auto firstArgs = L"restart-first " + std::to_wstring(workerPid) + L" " + std::to_wstring(run);
    const auto first = Start(L"lifecycle.exe", firstArgs.c_str());
    const HANDLE startup[] = {ready.get(), first.handle.get()};
    Require(WaitForMultipleObjects(2, startup, FALSE, 4000) == WAIT_OBJECT_0,
            "first NVDA fixture was ready for replacement");
    const auto nextArgs = L"restart-next " + std::to_wstring(first.pid) + L" " + std::to_wstring(run);
    const auto next = Start(L"lifecycle.exe", nextArgs.c_str());
    const HANDLE processes[] = {first.handle.get(), next.handle.get()};
    Require(WaitForMultipleObjects(2, processes, TRUE, 4500) == WAIT_OBJECT_0,
            "both NVDA fixtures finished their holds and exited");
    DWORD code = 1;
    Require(GetExitCodeProcess(first.handle.get(), &code) && code == 0, "first NVDA startup succeeded");
    Require(GetExitCodeProcess(next.handle.get(), &code) && code == 0, "replacement NVDA startup succeeded");
    fputs("Both NVDA fixture processes acquired the same startup mutex in turn and bound the desktop endpoint.\n",
          result);
}

void Hold(FILE* result, HINSTANCE instance, const std::wstring& mode) {
    if (mode == L"hold-window") {
        WNDCLASSW window{};
        window.hInstance = instance;
        window.lpszClassName = L"wxWindowClassNR";
        window.lpfnWndProc = DefWindowProcW;
        Require(RegisterClassW(&window), "register the window clash fixture");
        Require(CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window.lpszClassName, L"NVDA",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr) != nullptr,
                "create the window clash fixture");
    } else {
        Require(vb::RegisterController(vb::EndpointName()) == RPC_S_OK, "register the RPC clash fixture");
    }
    fputs("Fixture ready\n", result);
    fflush(result);
    Sleep(20000);
}

void CheckTestDesktop(FILE* result) {
    Require(!NvdaWindow(), "no previous NVDA window is on the SSH desktop");
    const auto desktop = vb::DesktopName();
    Require(!desktop.empty(), "read the SSH desktop name");
    const auto nvdaMutex = L"Local\\NVDA_" + desktop;
    for (const auto name : {L"Local\\VoiceBridge.Supervisor", L"Local\\VoiceBridge.Server", nvdaMutex.c_str()}) {
        const vb::Handle mutex(OpenMutexW(SYNCHRONIZE, FALSE, name));
        Require(!mutex && GetLastError() == ERROR_FILE_NOT_FOUND, "no previous startup mutex is on the SSH desktop");
    }
    const vb::Handle stop(OpenEventW(SYNCHRONIZE, FALSE, L"Local\\VoiceBridge.Stop"));
    Require(!stop && GetLastError() == ERROR_FILE_NOT_FOUND, "no previous VoiceBridge stop event is on the SSH desktop");
    fputs("The SSH desktop had no NVDA window, Bridge stop event or existing startup mutex.\n", result);
}

void CheckServing(DWORD pid, const wchar_t* image) {
    const auto window = NvdaWindow();
    Require(Owner(window) == pid, "exact NVDA window still belongs to the expected worker");
    const auto worker = OpenTestProcess(pid, image);
    Require(!IsWindowVisible(window) && vb::ControllerAnswers(vb::EndpointName()),
            "window stayed hidden and the test RPC endpoint still answered");
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    if (!IsTestProcess(GetCurrentProcess(), L"lifecycle.exe")) return 2;
    const std::wstring command(arguments);
    const auto separator = command.find(L' ');
    const auto mode = command.substr(0, separator);
    wchar_t* end = nullptr;
    const DWORD pid = separator == std::wstring::npos ? 0 :
        wcstoul(command.c_str() + separator + 1, &end, 10);
    const DWORD run = end ? wcstoul(end, nullptr, 10) : 0;
    const char* output = mode == L"restart-first" ? "lifecycle-restart-first.txt" :
                         mode == L"restart-next" ? "lifecycle-restart-next.txt" : "lifecycle.txt";
    return Test(output, [&](FILE* result) {
        if (mode == L"quit" || mode == L"endsession" || mode == L"stuck-quit") {
            RequestExit(result, pid, mode == L"endsession" ? WM_ENDSESSION : WM_QUIT, mode == L"stuck-quit");
        } else if (mode == L"appear") {
            Appear(result, pid);
        } else if (mode == L"window") {
            CheckWindow(result);
        } else if (mode == L"test-desktop") {
            CheckTestDesktop(result);
        } else if (mode == L"no-nvda-window") {
            Require(!NvdaWindow(), "no NVDA window during the sign-in wait");
        } else if (mode == L"serving" || mode == L"signin-serving") {
            CheckServing(pid, mode == L"signin-serving" ? L"signin-bridge.exe" : L"VoiceBridge.exe");
        } else if (mode == L"startup-pending") {
            CheckStartupPending(result, pid);
        } else if (mode == L"normal-start" || mode == L"restart-first" || mode == L"restart-next") {
            StartNvda(result, mode, pid, run);
        } else if (mode == L"missed-window-start") {
            StartNvdaAfterMissedWindow(result);
        } else if (mode == L"restart") {
            RestartNvda(result, pid);
        } else if (mode == L"hold-window" || mode == L"hold-endpoint") {
            Hold(result, instance, mode);
        } else {
            throw std::runtime_error("invalid test mode");
        }
    });
}
