#pragma once
#include <windows.h>
#include <memory>

namespace vb {
struct CloseHandleDeleter {
    void operator()(HANDLE handle) const {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

inline bool HasHandle(HANDLE handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}
}
