#pragma once
#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>

inline void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
Function Load(HMODULE module, const char* name) {
    // GetProcAddress has a generic function-pointer type.
    auto entry = GetProcAddress(module, name);
    Require(entry != nullptr, name);
    return reinterpret_cast<Function>(reinterpret_cast<uintptr_t>(entry));
}

template <class Run>
int Test(const char* filename, Run run) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    FILE* file = fopen(filename, "w");
    if (!file) return 2;
    int code = 0;
    try {
        run(file);
        fputs("PASS\n", file);
    } catch (const std::exception& error) {
        fprintf(file, "FAIL: %s\n", error.what());
        code = 1;
    }
    fclose(file);
    return code;
}
