#include "test.h"
#include "controller_rpc.h"
#include "handle.h"

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
    std::wstring command = L"\"";
    command += program;
    command += L"\" ";
    command += arguments;
    PROCESS_INFORMATION process{};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Require(CreateProcessW(program, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process), "start child");
    CloseHandle(process.hThread);
    return {vb::Handle(process.hProcess), process.dwProcessId};
}

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

HWND BridgeWindow() {
    return FindWindowW(L"VoiceBridge.ServerWindow", L"VoiceBridge");
}

HWND NvdaWindow() {
    return FindWindowW(L"wxWindowClassNR", L"NVDA");
}

DWORD Owner(HWND window) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    return owner;
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
    Require(!BridgeWindow(), "no previous VoiceBridge window in test desktop");
    Require(!NvdaWindow(), "no NVDA window in test desktop");
    const auto guard = Start(L"VoiceBridge.exe", L"--serve");
    const auto deadline = Clock::now() + 4s;
    HWND window = nullptr;
    bool ready = false;
    do {
        window = BridgeWindow();
        Require(GetForegroundWindow() == foreground, "foreground changed during startup");
        Require(WaitForSingleObject(guard.handle.get(), 0) == WAIT_TIMEOUT, "guard exited during startup");
        ready = window && vb::ControllerAnswers(vb::EndpointName());
        if (ready) break;
        Sleep(10);
    } while (Clock::now() < deadline);

    const auto workerPid = Owner(window);
    Require(ready && workerPid && workerPid != guard.pid, "hidden worker window is discoverable");
    Require(!NvdaWindow(), "NVDA must not find the bridge window");
    const vb::Handle worker(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, workerPid));
    Require(!!worker, "open RPC worker");
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
    Require(PostMessageW(window, WM_CLOSE, 0, 0), "send WM_CLOSE");
    const auto workerElapsed = WaitForExit(worker.get(), since, 250);
    const auto guardElapsed = WaitForExit(guard.handle.get(), since, 1000);
    DWORD code = 1;
    Require(GetExitCodeProcess(guard.handle.get(), &code) && code == 0, "guard exit code after WM_CLOSE");
    Require(GetForegroundWindow() == foreground, "foreground changed on close");
    fprintf(result, "Private hidden window, no console, unchanged foreground. WM_CLOSE stopped the worker in "
                    "%.2f ms and the guard in %.2f ms.\n", workerElapsed, guardElapsed);
}

void CheckStartupPending(FILE* result, DWORD guardPid) {
    const vb::Handle guard(OpenProcess(SYNCHRONIZE, FALSE, guardPid));
    Require(!!guard, "open pending startup guard");
    const auto deadline = Clock::now() + 3s;
    HWND window = nullptr;
    do {
        Require(WaitForSingleObject(guard.get(), 0) == WAIT_TIMEOUT, "guard still lives during startup");
        window = BridgeWindow();
        if (window) break;
        Sleep(10);
    } while (Clock::now() < deadline);
    const auto workerPid = Owner(window);
    Require(workerPid && workerPid != guardPid, "find pending worker's private window");
    const auto endpoint = vb::EndpointName();
    Require(!endpoint.empty(), "find the pending worker's endpoint name");
    Require(!vb::ControllerAnswers(endpoint), "RPC must not answer while the speaker is starting");
    Sleep(150);
    Require(WaitForSingleObject(guard.get(), 0) == WAIT_TIMEOUT && Owner(BridgeWindow()) == workerPid,
            "guard and worker remained alive with a stalled speaker");
    Require(!vb::ControllerAnswers(endpoint), "RPC must still be unavailable after 150 ms");
    fputs("The stalled speaker kept RPC unavailable while the guard and worker stayed alive.\n", result);
}

void RequestExit(FILE* result, DWORD pid, UINT message, bool stalled = false) {
    const auto window = BridgeWindow();
    Require(window && Owner(window) == pid, "expected VoiceBridge window");
    vb::Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Require(!!process, "open VoiceBridge process");
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
        Require(PostMessageW(window, message, FALSE, 0), "send canceled session end");
        Require(WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT &&
                vb::ControllerAnswers(vb::EndpointName()), "bridge survived canceled shutdown");
    }
    const auto since = Clock::now();
    Require(PostMessageW(window, message, message == WM_ENDSESSION ? TRUE : 0, 0), "post message");
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
    Require(Owner(BridgeWindow()) == pid, "expected a serving child");
    vb::Handle prior(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Require(!!prior, "open serving child");
    const auto started = Clock::now();
    const auto fake = Start(L"vb-fake-nvda.exe", L"");
    const auto exit = WaitForExit(prior.get(), started, 750);
    Require(exit <= 750, "server exited within 750 ms of launch");
    const auto heldUntil = Clock::now() + 2500ms;
    do {
        Require(!BridgeWindow(), "bridge was present while fake NVDA was running");
        Require(WaitForSingleObject(fake.handle.get(), 0) == WAIT_TIMEOUT, "fake process still running");
        Sleep(40);
    } while (Clock::now() < heldUntil);

    Require(TerminateProcess(fake.handle.get(), 0), "end test fixture");
    Require(WaitForSingleObject(fake.handle.get(), 1000) == WAIT_OBJECT_0, "fake exited");
    const auto stopped = Clock::now();
    const auto deadline = stopped + 2s;
    bool ready = false;
    do {
        const auto next = Owner(BridgeWindow());
        ready = next && next != pid && vb::ControllerAnswers(vb::EndpointName());
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
    NvdaStartupMutex() {
        wchar_t desktop[256]{};
        Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                          desktop, sizeof(desktop), nullptr), "get NVDA desktop name");
        const auto name = std::wstring(L"Local\\NVDA_") + desktop;
        mutex_.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        Require(!!mutex_, "open NVDA startup mutex");
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

vb::Handle RestartEvent(DWORD run, const wchar_t* step) {
    const auto name = L"Local\\VoiceBridge.Test.Restart." + std::to_wstring(run) + L"." + step;
    vb::Handle event(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
    Require(!!event, "open NVDA restart fixture event");
    return event;
}

void StartNvda(FILE* result, const std::wstring& mode, DWORD pid, DWORD run) {
    const bool replacing = mode == L"restart-next";
    const bool first = mode == L"restart-first";
    if (!replacing) Require(Owner(BridgeWindow()) == pid, "expected serving worker before NVDA startup");
    const vb::Handle prior(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Require(!!prior, "open process being replaced");
    // NVDA looks up its window before creating the mutex. In this fixture no
    // NVDA window exists, so an ease-of-access start will continue too.
    Require(!NvdaWindow(), "NVDA startup must not discover a window in this fixture");
    const auto started = Clock::now();
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
    Require(!BridgeWindow(), "VoiceBridge is standing aside after NVDA took the endpoint");
    fprintf(result, "NVDA fixture acquired its mutex and bound the endpoint in %.2f ms.\n",
            Milliseconds(Clock::now() - started));
    if (first) {
        const auto ready = RestartEvent(run, L"FirstReady");
        const auto waiting = RestartEvent(run, L"NextWaiting");
        Require(SetEvent(ready.get()), "signal first NVDA fixture ready");
        Require(WaitForSingleObject(waiting.get(), 3000) == WAIT_OBJECT_0, "replacement opened the startup mutex");
    } else {
        Sleep(650);
        Require(!BridgeWindow(), "VoiceBridge stayed out while NVDA owned the endpoint");
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
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
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
        } else if (mode == L"startup-pending") {
            CheckStartupPending(result, pid);
        } else if (mode == L"normal-start" || mode == L"ease-start" ||
                   mode == L"restart-first" || mode == L"restart-next") {
            StartNvda(result, mode, pid, run);
        } else if (mode == L"restart") {
            RestartNvda(result, pid);
        } else if (mode == L"hold-window" || mode == L"hold-endpoint") {
            Hold(result, instance, mode);
        } else {
            throw std::runtime_error("invalid test mode");
        }
    });
}
