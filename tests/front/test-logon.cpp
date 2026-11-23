#include "../../src/front/process.h"

// Simulate a fresh logon on each start without changing Windows or its registry.
std::optional<ULONGLONG> vb::LogonAgeMs() {
    return std::nullopt;
}
