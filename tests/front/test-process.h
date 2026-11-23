#pragma once
#include "test.h"
#include "handle.h"

#include <cwchar>

inline constexpr wchar_t TestDirectory[] = LR"(C:\VoiceBridge-test\front\)";

inline std::wstring TestPath(const wchar_t* image) {
    return std::wstring(TestDirectory) + image;
}

inline bool IsTestProcess(HANDLE process, const wchar_t* image) {
    wchar_t path[MAX_PATH]{};
    DWORD length = MAX_PATH;
    DWORD session = 0;
    const auto pid = GetProcessId(process);
    return pid && ProcessIdToSessionId(pid, &session) && session == 0 &&
           QueryFullProcessImageNameW(process, 0, path, &length) &&
           _wcsicmp(path, TestPath(image).c_str()) == 0 &&
           WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

inline vb::Handle OpenTestProcess(DWORD pid, const wchar_t* image) {
    vb::Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    Require(process && IsTestProcess(process.get(), image), "expected the exact live SSH test process");
    return process;
}

inline HWND NvdaWindow() {
    return FindWindowW(L"wxWindowClassNR", L"NVDA");
}

inline DWORD Owner(HWND window) {
    DWORD owner = 0;
    if (window) GetWindowThreadProcessId(window, &owner);
    return owner;
}
