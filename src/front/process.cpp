#include "process.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <string_view>

namespace vb {

std::filesystem::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == path.size()) return {};
    path.resize(length);
    return path;
}

namespace {
// The current Windows SDK defines this information class, but MinGW does not.
constexpr auto BasicProcessInformation = static_cast<SYSTEM_INFORMATION_CLASS>(252);
constexpr NTSTATUS BufferTooSmall = static_cast<NTSTATUS>(0xC0000004u);

struct BasicProcessEntry {
    ULONG nextOffset;
    HANDLE pid;
    HANDLE parentPid;
    ULONG64 sequence;
    UNICODE_STRING image;
};

bool SameName(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
            == CSTR_EQUAL;
}

template <class Visit>
bool ScanBasicProcesses(Visit visit) {
    using Query = decltype(&NtQuerySystemInformation);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!query) return false;

    // Reuse the buffer across the server's twice-per-second scans.
    static std::vector<BYTE> buffer(64 * 1024);
    ULONG bytes = 0;
    NTSTATUS status;
    for (;;) {
        status = query(BasicProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &bytes);
        if (status != BufferTooSmall) break;
        const auto needed = std::max(buffer.size() * 2, static_cast<size_t>(bytes));
        if (needed > 8 * 1024 * 1024) return false;
        buffer.resize(needed);
    }
    if (status < 0 || bytes > buffer.size()) return false;

    size_t offset = 0;
    while (offset + sizeof(BasicProcessEntry) <= bytes) {
        const auto& entry = *reinterpret_cast<const BasicProcessEntry*>(buffer.data() + offset);
        if (entry.image.Length && visit(static_cast<DWORD>(reinterpret_cast<uintptr_t>(entry.pid)),
            std::wstring_view(entry.image.Buffer, entry.image.Length / sizeof(wchar_t)))) return true;
        if (!entry.nextOffset) return true;
        if (entry.nextOffset < sizeof(BasicProcessEntry)) return false;
        offset += entry.nextOffset;
    }
    return false;
}
}

NvdaProcess FindNvda(const Settings& settings, const std::filesystem::path& executable) {
    if (settings.nvdaNames.empty()) return {};
    const DWORD self = GetCurrentProcessId();
    DWORD session = 0;
    if (!ProcessIdToSessionId(self, &session)) return {};
    const auto selfImage = executable.filename().wstring();

    NvdaProcess match;
    const auto visit = [&](DWORD pid, std::wstring_view image) {
        if (pid == self) return false;
        const auto named = std::any_of(settings.nvdaNames.begin(), settings.nvdaNames.end(),
            [image](const std::wstring& name) { return SameName(image, name); });
        if (!named || SameName(image, selfImage) || SameName(image, L"VoiceBridge.exe")) return false;

        DWORD candidateSession;
        if (!ProcessIdToSessionId(pid, &candidateSession) || candidateSession != session) return false;
        Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process) {
            if (GetLastError() == ERROR_ACCESS_DENIED) match.pid = pid;
            return false;
        }

        // Recheck the name on the process handle in case that PID was reused.
        wchar_t path[32768];
        DWORD length = static_cast<DWORD>(std::size(path));
        if (!QueryFullProcessImageNameW(process.get(), 0, path, &length)) {
            if (GetLastError() == ERROR_ACCESS_DENIED) match.pid = pid;
            return false;
        }
        const std::wstring_view fullImage(path, length);
        if (!SameName(image, fullImage.substr(fullImage.find_last_of(L"\\/") + 1))) return false;
        if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return false;
        match = {pid, std::move(process)};
        return true;
    };

    if (ScanBasicProcesses(visit)) return match;

    // Windows builds without the basic query use the compatible Win32 snapshot.
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!HasHandle(snapshot.get())) return match;
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    if (!Process32FirstW(snapshot.get(), &item)) return match;
    do {
        if (visit(item.th32ProcessID, item.szExeFile)) break;
    } while (Process32NextW(snapshot.get(), &item));
    return match;
}

Handle StartServer(const std::filesystem::path& executable) {
    auto command = L"\"" + executable.wstring() + L"\" --serve";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) return {};
    CloseHandle(child.hThread);
    return Handle(child.hProcess);
}
}
