#include "test.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return Test("fake.txt", [](FILE* file) {
        fputs("Fake NVDA fixture ready\n", file);
        fflush(file);
        Sleep(20000);
    });
}
