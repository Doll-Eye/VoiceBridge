#include "process.h"

#include <sddl.h>
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

Handle CreateNvdaStartupMutex(const std::wstring& name) {
    // CreateMutexW in NVDA requests full access. Preserve access for this
    // user at ordinary integrity even if VoiceBridge was started elevated.
    HANDLE rawToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return {};
    const Handle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size)) return {};
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return {};
    auto sddl = std::wstring(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;") + sid + L")S:(ML;;NW;;;ME)";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) return {};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle mutex(CreateMutexW(&security, TRUE, name.c_str()));
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(descriptor);
    if (existed) return {};
    return mutex;
}

bool HasOtherMutexHandle(HANDLE mutex) {
    using Query = decltype(&NtQueryObject);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
    PUBLIC_OBJECT_BASIC_INFORMATION info{};
    // NVDA opens the existing mutex before it waits on it. Yield on a query
    // failure too: serving must never depend on guessing a launcher's image.
    return !query || query(mutex, ObjectBasicInformation, &info, sizeof(info), nullptr) < 0 ||
           info.HandleCount != 1;
}

ServerProcess StartServer(const std::filesystem::path& executable) {
    ServerProcess server;
    server.job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!server.job || !SetInformationJobObject(server.job.get(), JobObjectExtendedLimitInformation,
                                                &limits, sizeof(limits))) return {};
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    server.stop.reset(CreateEventW(&inherit, TRUE, FALSE, nullptr));
    if (!server.stop) return {};
    HANDLE event = server.stop.get(), job = server.job.get();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    std::vector<BYTE> buffer(size);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0, &size)) return {};
    PROCESS_INFORMATION child{};
    auto command = L"\"" + executable.wstring() + L"\" --rpc-worker " +
                   std::to_wstring(reinterpret_cast<uintptr_t>(event));
    // Job membership is atomic with creation. Even a crashing supervisor
    // cannot leave an orphan with the controller endpoint.
    bool started = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                             &job, sizeof(job), nullptr, nullptr) &&
                   UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                             &event, sizeof(event), nullptr, nullptr) &&
                   CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                  nullptr, nullptr, &startup.StartupInfo, &child);
    auto error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) {
        SetLastError(error);
        return {};
    }
    server.process.reset(child.hProcess);
    server.thread.reset(child.hThread);
    return server;
}
}
