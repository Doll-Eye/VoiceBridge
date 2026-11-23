#include "test.h"

#include <cwchar>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return Test("tolk.txt", [](FILE* result) {
        const auto dll = LoadLibraryW(L".\\Tolk.dll");
        Require(dll != nullptr, "load the GTA11Y Revo Tolk DLL");
        const auto load = Load<void (*)()>(dll, "Tolk_Load");
        const auto unload = Load<void (*)()>(dll, "Tolk_Unload");
        const auto detect = Load<const wchar_t* (*)()>(dll, "Tolk_DetectScreenReader");
        const auto output = Load<bool (*)(const wchar_t*, bool)>(dll, "Tolk_Output");
        const auto silence = Load<bool (*)()>(dll, "Tolk_Silence");
        load();
        const auto reader = detect();
        Require(reader && wcscmp(reader, L"NVDA") == 0, "Tolk detected NVDA");
        Require(output(L"Tolk plain", false), "Tolk_Output succeeded");
        Require(silence(), "Tolk_Silence succeeded");
        unload();
        FreeLibrary(dll);
        fputs("GTA11Y Revo Tolk detected NVDA, output a phrase and silenced speech.\n", result);
    });
}
