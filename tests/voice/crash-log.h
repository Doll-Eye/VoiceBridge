#pragma once

// Test-only crash diagnostics for the voice tests.
//
// Every line goes to Windows with WriteFile as soon as it is complete, so it
// survives the process ending a moment later. The handlers only record what
// is ending the process, on which thread and through which modules; the
// original failure then carries on exactly as before. Nothing here attaches
// to another process or changes a Windows setting.
//
// Lines written from the loader notification and the exception handler use
// no heap, no locks of their own and no CRT formatting, because the heap may
// be the thing that is damaged and the loader lock may be held.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <cxxabi.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <typeinfo>

namespace vb::test::crash {

inline HANDLE logFile = INVALID_HANDLE_VALUE;

class Line {
public:
    Line() {
        SYSTEMTIME now;
        GetLocalTime(&now);
        two(now.wHour).text(":").two(now.wMinute).text(":").two(now.wSecond).text(".");
        number(now.wMilliseconds / 100).number(now.wMilliseconds / 10 % 10).number(now.wMilliseconds % 10);
        text(" t").number(GetCurrentThreadId()).text(" ");
    }
    Line& text(const char* value) {
        while (value && *value && size_ < Capacity) data_[size_++] = *value++;
        return *this;
    }
    Line& text(const std::string& value) { return text(value.c_str()); }
    Line& wide(const wchar_t* value, size_t count) {
        for (size_t i = 0; value && i < count && size_ < Capacity; ++i)
            data_[size_++] = value[i] >= 32 && value[i] < 127 ? static_cast<char>(value[i]) : '?';
        return *this;
    }
    Line& number(unsigned long long value) {
        char digits[24];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && size_ < Capacity) data_[size_++] = digits[--count];
        return *this;
    }
    Line& hex(unsigned long long value) {
        text("0x");
        char digits[16];
        int count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        while (count && size_ < Capacity) data_[size_++] = digits[--count];
        return *this;
    }
    // Module name and offset, found without the loader lock: the inverted
    // function table gives the image, and its export directory gives the name.
    Line& address(const void* pc) {
        void* image = nullptr;
        RtlPcToFileHeader(const_cast<void*>(pc), &image);
        if (!image) return text("[no module] ").hex(reinterpret_cast<std::uintptr_t>(pc));
        const auto* base = static_cast<const unsigned char*>(image);
        const char* name = "module";
        if (image == GetModuleHandleW(nullptr)) {
            name = "test-exe";
        } else {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const auto& exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (exports.VirtualAddress && exports.Size) {
                name = reinterpret_cast<const char*>(
                    base + reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exports.VirtualAddress)->Name);
            }
        }
        return text(name).text("+").hex(static_cast<const unsigned char*>(pc) - base);
    }
    void write() {
        if (size_ < Capacity) data_[size_++] = '\r';
        data_[size_++] = '\n';
        DWORD written = 0;
        if (logFile != INVALID_HANDLE_VALUE) WriteFile(logFile, data_, static_cast<DWORD>(size_), &written, nullptr);
    }

private:
    Line& two(unsigned value) { return number(value / 10).number(value % 10); }
    static constexpr size_t Capacity = 2040;
    char data_[Capacity + 8];
    size_t size_ = 0;
};

// Return addresses of the calling thread, outermost last.
inline void Stack(const char* label, ULONG skip) {
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(skip + 1, 40, frames, nullptr);
    Line line;
    line.text("  ").text(label).text(" stack:");
    for (USHORT i = 0; i < count; ++i) line.text(" ").address(frames[i]);
    line.write();
}

// Where the current thread began, which tells a VoiceBridge thread from one
// started by SAPI, the speech engine or Windows.
inline void ThreadOrigin(bool name = true) {
    using Query = NTSTATUS (NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    PVOID start = nullptr;
    Line line;
    line.text("  thread ").number(GetCurrentThreadId()).text(" began at ");
    if (query && query(GetCurrentThread(), static_cast<THREADINFOCLASS>(9), &start, sizeof(start), nullptr) >= 0 && start)
        line.address(start);
    else
        line.text("an unknown address");
    line.write();
    if (!name) return;
    // The description needs a small heap allocation, so it comes last.
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description && *description) {
        Line named;
        named.text("  thread name: ").wide(description, lstrlenW(description)).write();
    }
    if (description) LocalFree(description);
}

inline void OnTerminate() {
    static std::atomic<bool> entered{false};
    if (!entered.exchange(true)) {
        Line line;
        line.text("FAILURE std::terminate");
        if (const auto* type = abi::__cxa_current_exception_type()) {
            line.text(" with exception of type ").text(type->name());
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& error) {
                line.text(", what(): ").text(error.what());
            } catch (...) {
            }
        } else {
            line.text(" without an active exception");
        }
        line.write();
        ThreadOrigin();
        Stack("terminate", 0);
    }
    std::abort();
}

inline void OnAbort(int) {
    Line().text("FAILURE abort() was called").write();
    ThreadOrigin();
    Stack("abort", 0);
    // Returning lets the CRT continue its own abort, a fail-fast (0xC0000409).
}

inline void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
    Line().text("FAILURE the C runtime rejected an invalid parameter").write();
    ThreadOrigin();
    Stack("invalid parameter", 0);
    // Give this failure its own exit code instead of the CRT's fail-fast.
    TerminateProcess(GetCurrentProcess(), 0xC0000417);
}

// Records serious first-chance exceptions. Some are handled later by the code
// that raised them, so the exit code says whether one was fatal.
inline LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    static std::atomic<unsigned> serious{0}, ours{0}, others{0};
    std::atomic<unsigned>* counter;
    unsigned limit;
    if (code == 0x20474343) {  // A C++ exception thrown by code built with GCC: this program.
        counter = &ours;
        limit = 20;
    } else if (code == 0xE06D7363) {  // A C++ exception thrown inside an MSVC-built DLL.
        counter = &others;
        limit = 3;
    } else if ((code & 0xF0000000) == 0xC0000000 || code == EXCEPTION_BREAKPOINT) {
        counter = &serious;
        limit = 20;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (counter->fetch_add(1) >= limit) return EXCEPTION_CONTINUE_SEARCH;
    Line line;
    line.text("EXCEPTION ").hex(code).text(" at ").address(record->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto kind = record->ExceptionInformation[0];
        line.text(kind == 0 ? ", reading " : kind == 1 ? ", writing " : ", executing ")
            .hex(record->ExceptionInformation[1]);
    }
    line.text(" (first chance)").write();
    ThreadOrigin(code != 0xC0000374);  // A damaged heap must not be asked for the thread name.
    Stack("exception", 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Opens the step log beside the results and installs the handlers.
inline void Start(const std::string& path) {
    logFile = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbort);
    _set_invalid_parameter_handler(OnInvalidParameter);
    AddVectoredExceptionHandler(1, OnException);
    Line().text("Step log started. Process ").number(GetCurrentProcessId()).text(", test-exe at ")
        .hex(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))).write();
}

inline void Step(const std::string& text) { Line().text(text).write(); }

// Called by the loader with its lock held: no heap and no further loading.
inline void Library(bool loaded, const UNICODE_STRING* name, const void* base, unsigned long size) {
    Line line;
    line.text(loaded ? "dll loaded " : "dll unloaded ");
    if (name) line.wide(name->Buffer, name->Length / sizeof(wchar_t));
    line.text(" at ").hex(reinterpret_cast<std::uintptr_t>(base)).text(", ").number(size).text(" bytes").write();
}

}  // namespace vb::test::crash
