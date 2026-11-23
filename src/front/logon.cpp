#include "process.h"

#include <ntsecapi.h>

namespace vb {
std::optional<ULONGLONG> LogonAgeMs() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return std::nullopt;
    const Handle token(rawToken);
    TOKEN_STATISTICS statistics{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token.get(), TokenStatistics, &statistics, sizeof(statistics), &bytes)) {
        return std::nullopt;
    }

    PSECURITY_LOGON_SESSION_DATA session = nullptr;
    if (LsaGetLogonSessionData(&statistics.AuthenticationId, &session) != 0 || !session) return std::nullopt;
    const auto logon = session->Size >= offsetof(SECURITY_LOGON_SESSION_DATA, LogonTime) + sizeof(LARGE_INTEGER)
        ? session->LogonTime.QuadPart : 0;
    LsaFreeReturnBuffer(session);
    if (logon <= 0) return std::nullopt;

    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    const auto now = (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (now < static_cast<ULONGLONG>(logon)) return std::nullopt;
    return (now - logon) / 10000; // FILETIME units are 100 nanoseconds.
}
}
