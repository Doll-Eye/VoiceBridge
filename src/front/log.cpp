#include "log.h"

#include <cstdio>
#include <vector>

namespace vb {
namespace {
constexpr auto MaxLogBytes = 1024 * 1024;
constexpr auto MaxPendingChars = 1024 * 1024;
constexpr auto MaxPendingEntries = 4096;

Handle CreateLogMutex(const std::filesystem::path& directory) {
    const Handle folder(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ID_INFO id{};
    // The same directory can be reached from the signed-in desktop and SSH,
    // or through another path to the same directory. Filesystems without
    // directory IDs share a single lock.
    std::wstring name = L"Global\\VoiceBridge.Log";
    if (HasHandle(folder.get()) && GetFileInformationByHandleEx(folder.get(), FileIdInfo, &id, sizeof(id))) {
        name += L"." + std::to_wstring(id.VolumeSerialNumber) + L".";
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto byte : id.FileId.Identifier) {
            name += hex[byte >> 4];
            name += hex[byte & 0xf];
        }
    }
    return Handle(CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
}

std::string Utf8(const std::wstring& value) {
    const auto length = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    if (bytes) WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(), bytes, nullptr, nullptr);
    return result;
}

void AppendEscaped(std::string& line, const std::string& message) {
    for (char ch : message) {
        switch (ch) {
        case '\\': line += "\\\\"; break;
        case '\n': line += "\\n"; break;
        case '\r': line += "\\r"; break;
        default: line += ch; break;
        }
    }
}
}

Log::Log(const std::filesystem::path& directory, const wchar_t* role, LogLevel level)
    : path_(directory / L"bridge.log"), role_(role), level_(level),
      fileMutex_(CreateLogMutex(directory)),
      worker_(fileMutex_ ? CreateThread(nullptr, 0, Worker, this, 0, nullptr) : nullptr) {}

void Log::Write(LogLevel level, const std::wstring& line) {
    if (level > level_ || !worker_) return;
    Entry entry{{}, level, line};
    GetLocalTime(&entry.time);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        if (pending_.size() >= MaxPendingEntries || pendingChars_ + line.size() > MaxPendingChars) {
            ++dropped_;
            return;
        }
        pending_.push_back(std::move(entry));
        pendingChars_ += line.size();
    }
    ready_.notify_one();
}

void Log::FlushAndStop(DWORD milliseconds) {
    if (!worker_) return;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    WaitForSingleObject(worker_.get(), milliseconds);
}

DWORD WINAPI Log::Worker(void* log) {
    static_cast<Log*>(log)->Run();
    return 0;
}

void Log::Run() {
    for (;;) {
        std::deque<Entry> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;
            batch.swap(pending_);
            pendingChars_ = 0;
            if (dropped_) {
                Entry notice{{}, LogLevel::Error, L"Skipped " + std::to_wstring(dropped_) + L" log entries"};
                GetLocalTime(&notice.time);
                batch.push_back(std::move(notice));
                dropped_ = 0;
            }
        }
        Append(batch);
    }
}

void Log::Append(const std::deque<Entry>& batch) {
    const auto role = Utf8(role_);
    const auto pid = std::to_string(pid_);
    std::vector<std::string> lines;
    lines.reserve(batch.size());
    for (const auto& entry : batch) {
        const auto& t = entry.time;
        char stamp[64];
        std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        const char* level = entry.level == LogLevel::Error ? "error" :
                            entry.level == LogLevel::Info ? "info" : "debug";
        auto line = std::string(stamp) + " " + pid + " " + role + " " + level + " ";
        AppendEscaped(line, Utf8(entry.line));
        line += '\n';
        lines.push_back(std::move(line));
    }

    // Wait on the logging worker only. Shutdown still bounds its flush, and
    // a competing writer no longer causes a whole batch to be discarded.
    const auto lock = WaitForSingleObject(fileMutex_.get(), INFINITE);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return;
    struct MutexRelease {
        HANDLE handle;
        ~MutexRelease() { ReleaseMutex(handle); }
    } release{fileMutex_.get()};

    auto openLog = [this] {
        return Handle(CreateFileW(path_.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    };
    auto file = openLog();
    LARGE_INTEGER current{};
    if (!HasHandle(file.get()) || !SetFilePointerEx(file.get(), {}, &current, FILE_END)) return;
    for (const auto& line : lines) {
        if (current.QuadPart + static_cast<LONGLONG>(line.size()) > MaxLogBytes && current.QuadPart > 0) {
            auto old = path_;
            old += L".old";
            file.reset();
            // If a reader prevents renaming, reopen and append to the current
            // file. The next entry will try rotation again.
            MoveFileExW(path_.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
            file = openLog();
            if (!HasHandle(file.get()) || !SetFilePointerEx(file.get(), {}, &current, FILE_END)) return;
        }
        DWORD written = 0;
        if (!WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
            written != line.size()) {
            if (SetFilePointerEx(file.get(), current, nullptr, FILE_BEGIN)) SetEndOfFile(file.get());
            return;
        }
        current.QuadPart += written;
    }
}

LogLevel SpeakerLog::Level() const {
    if (!logText_ && log_.Level() == LogLevel::Debug) return LogLevel::Info;
    return log_.Level();
}

void SpeakerLog::Write(LogLevel level, const std::wstring& line) {
    if (level <= Level()) log_.Write(level, line);
}
}
