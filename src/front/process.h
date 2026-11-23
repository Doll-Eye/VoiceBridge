#pragma once
#include "handle.h"
#include "settings.h"

namespace vb {
inline constexpr wchar_t StopEventName[] = L"Local\\VoiceBridge.Stop";
inline constexpr wchar_t SupervisorMutex[] = L"Local\\VoiceBridge.Supervisor";
inline constexpr wchar_t ServerMutex[] = L"Local\\VoiceBridge.Server";

struct NvdaProcess {
    DWORD pid = 0;
    Handle handle;
};

std::filesystem::path ExecutablePath();
NvdaProcess FindNvda(const Settings& settings, const std::filesystem::path& executable);
struct ServerProcess {
    Handle job;
    Handle stop;
    Handle process;
    Handle thread;
};

// Start suspended so the parent can take NVDA's desktop mutex before RPC starts.
ServerProcess StartServer(const std::filesystem::path& executable);
Handle CreateNvdaStartupMutex(const std::wstring& name);
bool HasOtherMutexHandle(HANDLE mutex);
}
