#pragma once
#include "handle.h"
#include "settings.h"
#include <optional>

namespace vb {
inline constexpr wchar_t StopEventName[] = L"Local\\VoiceBridge.Stop";
inline constexpr wchar_t SupervisorMutex[] = L"Local\\VoiceBridge.Supervisor";
inline constexpr wchar_t ServerMutex[] = L"Local\\VoiceBridge.Server";
inline constexpr wchar_t NvdaWindowClass[] = L"wxWindowClassNR";
inline constexpr wchar_t NvdaWindowTitle[] = L"NVDA";
inline constexpr DWORD ServerStandAside = 3;
inline constexpr DWORD ControllerProbeTimedOut = 73;

struct NvdaProcess {
    DWORD pid = 0;
    Handle handle;
};

std::filesystem::path ExecutablePath();
std::optional<ULONGLONG> LogonAgeMs();
bool HasExternalNvdaWindow(const std::filesystem::path& executable);
NvdaProcess FindNvda(const Settings& settings, const std::filesystem::path& executable,
                     bool serving = false);
struct ServerProcess {
    Handle job;
    Handle stop;
    Handle process;
    Handle thread;
};

// Start suspended so the parent can take NVDA's desktop mutex before RPC starts.
ServerProcess StartServer(const std::filesystem::path& executable, HANDLE journal);
Handle CreateNvdaStartupMutex(const std::wstring& name, bool& alreadyExists);
std::optional<bool> HasOtherMutexHandle(HANDLE mutex);
}
