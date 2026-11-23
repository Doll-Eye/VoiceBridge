#include "test.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    return Test("stock.txt", [&](FILE* result) {
        const auto dll = LoadLibraryW(L".\\nvdaControllerClient64.dll");
        Require(dll != nullptr, "load stock client");
        using Status = unsigned long;
        using NoArguments = Status (WINAPI*)();
        using Text = Status (WINAPI*)(const wchar_t*);
        const auto running = Load<NoArguments>(dll, "nvdaController_testIfRunning");
        const auto speak = Load<Text>(dll, "nvdaController_speakText");
        const auto cancel = Load<NoArguments>(dll, "nvdaController_cancelSpeech");
        const auto braille = Load<Text>(dll, "nvdaController_brailleMessage");
        const auto status = running();
        fprintf(result, "testIfRunning = %lu\n", status);
        Require(status == 0, "testIfRunning");
        if (std::wstring(args) == L"--cost") {
            LARGE_INTEGER frequency, start, finish;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&start);
            for (int call = 0; call < 1000; ++call) {
                Require(speak(L"Timing") == 0, "timed speakText");
            }
            QueryPerformanceCounter(&finish);
            fprintf(result, "1000 stock speakText calls averaged %.2f microseconds\n",
                    (finish.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart);
        } else {
            Require(speak(L"Stock plain") == 0, "plain");
            Require(speak(L"Café déjà vu 中文") == 0, "Unicode");
            Require(speak(std::wstring(4000, L'x').c_str()) == 0, "long");
            Require(speak(L"") == 0, "empty");
            Require(speak(L"<b>5 < 6 & 7") == 0, "literal markup");
            Require(cancel() == 0, "cancelSpeech");
            Require(braille(L"Braille is ignored") == 0, "brailleMessage");
        }
        FreeLibrary(dll);
    });
}
