#pragma once
#include "handle.h"
#include "settings.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace vb {
class Log final : public ILog {
public:
    Log(const std::filesystem::path& directory, const wchar_t* role, LogLevel level);

    LogLevel Level() const override { return level_; }
    void Write(LogLevel level, const std::wstring& line) override;
    void FlushAndStop(DWORD milliseconds);

private:
    struct Entry {
        SYSTEMTIME time;
        LogLevel level;
        std::wstring line;
    };
    static DWORD WINAPI Worker(void* log);
    void Run();
    void Append(const std::deque<Entry>& batch);

    std::filesystem::path path_;
    std::wstring role_;
    LogLevel level_;
    DWORD pid_ = GetCurrentProcessId();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Entry> pending_;
    size_t pendingChars_ = 0;
    size_t dropped_ = 0;
    bool stopping_ = false;
    Handle fileMutex_;
    Handle worker_;
};

// The speaker interface reserves debug entries for any diagnostic that could
// include speech. Only enable those entries when the user opts in to text logging.
class SpeakerLog final : public ILog {
public:
    SpeakerLog(Log& log, bool logText) : log_(log), logText_(logText) {}
    LogLevel Level() const override;
    void Write(LogLevel level, const std::wstring& line) override;

private:
    Log& log_;
    bool logText_;
};
}
