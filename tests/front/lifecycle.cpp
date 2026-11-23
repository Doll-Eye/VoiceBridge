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
    DWORD pid;
    unsigned visible = 0;
};

BOOL CALLBACK CountVisible(HWND window, LPARAM context) {
    auto& windows = *reinterpret_cast<Windows*>(context);
    if (Owner(window) == windows.pid && IsWindowVisible(window)) ++windows.visible;
    return TRUE;
}

void CheckWindow(FILE* result) {
    const auto foreground = GetForegroundWindow();
    Require(!BridgeWindow(), "no previous NVDA window in test desktop");
    const auto bridge = Start(L"VoiceBridge.exe", L"--serve");
    const auto deadline = Clock::now() + 4s;
    HWND window = nullptr;
    do {
        window = BridgeWindow();
        Require(GetForegroundWindow() == foreground, "foreground changed during startup");
        Require(WaitForSingleObject(bridge.handle.get(), 0) == WAIT_TIMEOUT, "server exited during startup");
        if (window && vb::ControllerAnswers(vb::EndpointName())) break;
        Sleep(10);
    } while (Clock::now() < deadline);

    Require(window && Owner(window) == bridge.pid, "hidden window is discoverable");
    Require(GetPriorityClass(bridge.handle.get()) == ABOVE_NORMAL_PRIORITY_CLASS, "above-normal server priority");
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    Require((style & WS_EX_TOOLWINDOW) && (style & WS_EX_NOACTIVATE), "window flags");
    Require(GetAncestor(window, GA_ROOT) == window && GetParent(window) == nullptr, "top-level window");
    Windows windows{bridge.pid};
    EnumWindows(CountVisible, reinterpret_cast<LPARAM>(&windows));
    Require(!IsWindowVisible(window) && !windows.visible, "no visible VoiceBridge windows");
    if (AttachConsole(bridge.pid)) {
        FreeConsole();
        throw std::runtime_error("VoiceBridge had a console");
    }
    Require(GetLastError() == ERROR_INVALID_HANDLE, "verify absence of console");
    Require(GetForegroundWindow() == foreground, "foreground changed");
    const auto since = Clock::now();
    Require(PostMessageW(window, WM_CLOSE, 0, 0), "send WM_CLOSE");
    const auto elapsed = WaitForExit(bridge.handle.get(), since, 250);
    Require(GetForegroundWindow() == foreground, "foreground changed on close");
    fprintf(result, "Hidden top-level window, no console, unchanged foreground. WM_CLOSE exit: %.2f ms\n", elapsed);
}

void RequestExit(FILE* result, DWORD pid, UINT message) {
    const auto window = BridgeWindow();
    Require(window && Owner(window) == pid, "expected VoiceBridge window");
    vb::Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Require(!!process, "open VoiceBridge process");
    const auto since = Clock::now();
    Require(PostMessageW(window, message, message == WM_ENDSESSION ? TRUE : 0, 0), "post message");
    const auto elapsed = WaitForExit(process.get(), since, 250);
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
    return Test("lifecycle.txt", [&](FILE* result) {
        const std::wstring command(arguments);
        const auto separator = command.find(L' ');
        const auto mode = command.substr(0, separator);
        const DWORD pid = separator == std::wstring::npos ? 0 :
            wcstoul(command.c_str() + separator + 1, nullptr, 10);
        if (mode == L"quit" || mode == L"endsession") {
            RequestExit(result, pid, mode == L"quit" ? WM_QUIT : WM_ENDSESSION);
        } else if (mode == L"appear") {
            Appear(result, pid);
        } else if (mode == L"window") {
            CheckWindow(result);
        } else if (mode == L"hold-window" || mode == L"hold-endpoint") {
            Hold(result, instance, mode);
        } else {
            throw std::runtime_error("invalid test mode");
        }
    });
}
