#include "test-process.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    return Test("fake.txt", [=](FILE* file) {
        if (*arguments) {
            Require(IsTestProcess(GetCurrentProcess(), L"vb-fake-nvda.exe"), "expected the exact SSH launcher fixture");
            const std::wstring command(arguments);
            const auto separator = command.find(L' ');
            const auto option = command.substr(0, separator);
            Require(separator != std::wstring::npos && (option == L"--ease-of-access" || option == L"--check-running"),
                    "launch with an NVDA option and the expected worker PID");
            const auto pidText = command.c_str() + separator + 1;
            wchar_t* end = nullptr;
            const auto pid = wcstoul(pidText, &end, 10);
            Require(pid && !*end, "read the expected worker PID");
            const auto worker = OpenTestProcess(pid, L"VoiceBridge.exe");
            const auto started = GetTickCount64();
            Sleep(850);
            Require(Owner(NvdaWindow()) == pid && WaitForSingleObject(worker.get(), 0) == WAIT_TIMEOUT,
                    "the slow launcher found the same NVDA window and worker");
            fprintf(file, "The %ls fixture found the original worker after %llu ms and exited quietly.\n",
                    option.c_str(), GetTickCount64() - started);
            return;
        }
        fputs("Fake NVDA fixture ready\n", file);
        fflush(file);
        Sleep(20000);
    });
}
