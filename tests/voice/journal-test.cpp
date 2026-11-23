#include "../../src/common/speech_journal.h"
#include "observer.h"

#include <cstdlib>
#include <fstream>
#include <initializer_list>

namespace vb::voice::test { Sink observer = nullptr; }

namespace {

using vb::SpeechJournal;
using vb::SpeechJournalMapping;

enum class CrashMode : DWORD { InterruptedAppend = 61, CommittedAppend = 62, StartedReplay = 63 };

std::ofstream report;
int failures = 0;

void Check(bool passed, const char* description) {
    report << (passed ? "PASS " : "FAIL ") << description << std::endl;
    failures += !passed;
}

bool Matches(SpeechJournal& journal, std::initializer_list<SpeechJournal::Entry> expected) {
    const auto entries = journal.recover();
    return entries.size() == expected.size() &&
           std::equal(entries.begin(), entries.end(), expected.begin(), [](const auto& left, const auto& right) {
               return left.cursor == right.cursor && left.text == right.text;
           });
}

bool Rejects(SpeechJournal& journal, std::wstring_view text) {
    try {
        journal.append(text);
        return false;
    } catch (const std::bad_alloc&) {
        return true;
    }
}

void WrapAndOverflow() {
    SpeechJournalMapping mapping;
    SpeechJournal journal;
    if (!mapping.create() || !journal.map(mapping.handle())) {
        Check(false, "create and map journal for wraparound");
        return;
    }
    std::wstring phrase(100000, L'x');
    // Check both ends of the payload, including after the ring splits it.
    phrase.front() = L'\0';
    phrase.back() = L'z';
    std::uint64_t last = 0;
    bool wrapped = true;
    for (int index = 0; index < 13; ++index) {
        const auto cursor = journal.append(phrase);
        journal.retire(last);
        wrapped &= cursor > last && Matches(journal, {{cursor, phrase}});
        journal.retire(cursor);
        last = cursor;
    }
    Check(wrapped, "maximum-length phrases round-trip across the physical end of the ring");
    Check(Rejects(journal, std::wstring(100001, L'x')), "reject text beyond the controller limit");

    std::vector<std::uint64_t> cursors;
    for (;;) {
        try {
            cursors.push_back(journal.append(phrase));
        } catch (const std::bad_alloc&) {
            break;
        }
    }
    const auto filled = journal.recover();
    Check(cursors.size() > 1 && filled.size() == cursors.size() &&
              std::equal(filled.begin(), filled.end(), cursors.begin(), [&](const auto& entry, auto cursor) {
                  return entry.cursor == cursor && entry.text == phrase;
              }),
          "overflow preserves every accepted phrase in order");
    if (cursors.size() < 2) return;
    journal.retire(cursors[1]);
    Check(Rejects(journal, phrase), "out-of-order retirement does not overwrite an earlier pending phrase");
    journal.retire(cursors[0]);
    const auto reused = journal.append(phrase);
    const auto afterwards = journal.recover();
    Check(reused > cursors.back() && afterwards.size() == cursors.size() - 1 &&
              afterwards.back().cursor == reused && afterwards.back().text == phrase &&
              std::equal(afterwards.begin(), afterwards.end() - 1, cursors.begin() + 2,
                         [&](const auto& entry, auto cursor) { return entry.cursor == cursor && entry.text == phrase; }),
          "retiring the oldest phrase reclaims the contiguous retired entries");
}

void CancelAndRetire() {
    SpeechJournalMapping mapping;
    SpeechJournal journal;
    if (!mapping.create() || !journal.map(mapping.handle())) {
        Check(false, "create and map journal for cancellation");
        return;
    }
    const auto first = journal.append(L"first");
    const auto second = journal.append(L"second");
    const auto third = journal.append(L"third");
    journal.retire(second);
    journal.retire(second);
    Check(Matches(journal, {{first, L"first"}, {third, L"third"}}),
          "single retirement is idempotent and preserves pending phrases on either side");
    journal.retireThrough(second);
    journal.retireThrough(first);
    Check(Matches(journal, {{third, L"third"}}), "prefix retirement accepts an already retired ticket");
    journal.clear();
    Check(Matches(journal, {}), "cancellation clears all pending phrases");
    const auto next = journal.append(L"");
    journal.retire(third);
    journal.retireThrough(third);
    Check(next > third && Matches(journal, {{next, L""}}), "stale tickets cannot retire a phrase after cancellation");
}

int CrashChild(const char* args, CrashMode mode) {
    const auto inherited = std::strtoull(args, nullptr, 10);
    SpeechJournal journal;
    if (!inherited || !journal.map(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(inherited)))) return 1;
    if (mode == CrashMode::StartedReplay) {
        const auto entries = journal.recover();
        if (entries.empty()) return 1;
        journal.beginReplay(entries.front().cursor);
    } else {
        if (mode == CrashMode::InterruptedAppend) {
            SpeechJournal::beforePublish = [] {
                TerminateProcess(GetCurrentProcess(), static_cast<DWORD>(CrashMode::InterruptedAppend));
            };
        }
        journal.append(L"phrase from the child");
    }
    TerminateProcess(GetCurrentProcess(), static_cast<DWORD>(mode));
    return 1;
}

bool RunCrashChild(HANDLE handle, CrashMode mode) {
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return false;
    std::wstring command = L"journal-test.exe ";
    switch (mode) {
    case CrashMode::InterruptedAppend: command += L"interrupt "; break;
    case CrashMode::CommittedAppend: command += L"commit "; break;
    case CrashMode::StartedReplay: command += L"replay "; break;
    }
    command += std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    const bool exited = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 0;
    const bool passed = exited && GetExitCodeProcess(process.hProcess, &code) &&
                        code == static_cast<DWORD>(mode);
    CloseHandle(process.hProcess);
    return passed;
}

void RecoverAfterCrash() {
    SpeechJournalMapping mapping;
    std::uint64_t first = 0;
    {
        SpeechJournal journal;
        if (!mapping.create() || !journal.map(mapping.handle())) {
            Check(false, "create and map journal for crash recovery");
            return;
        }
        first = journal.append(L"pending phrase");
        journal.retire(journal.append(L"already retired"));
    }
    Check(RunCrashChild(mapping.handle(), CrashMode::InterruptedAppend),
          "terminate the child after copying and before committing its phrase");
    {
        SpeechJournal journal;
        Check(journal.map(mapping.handle()) && Matches(journal, {{first, L"pending phrase"}}),
              "replacement view recovers pending speech and skips retired and uncommitted speech");
    }
    Check(RunCrashChild(mapping.handle(), CrashMode::CommittedAppend), "terminate the child after committing its phrase");
    std::uint64_t second = 0;
    {
        SpeechJournal journal;
        if (!journal.map(mapping.handle())) {
            Check(false, "map journal after the committed child exits");
            return;
        }
        const auto entries = journal.recover();
        Check(entries.size() == 2 && entries.front().cursor == first && entries.front().text == L"pending phrase" &&
                  entries.back().cursor > first && entries.back().text == L"phrase from the child",
              "a committed phrase survives child death and replaces the interrupted append");
        if (entries.size() != 2) return;
        second = entries.back().cursor;
    }
    Check(RunCrashChild(mapping.handle(), CrashMode::StartedReplay), "terminate the child after dispatching its first replay");
    {
        SpeechJournal journal;
        Check(journal.map(mapping.handle()) && Matches(journal, {{second, L"phrase from the child"}}),
              "the next child drops the replayed phrase and preserves undispatched speech");
    }
    Check(RunCrashChild(mapping.handle(), CrashMode::StartedReplay), "terminate another child after dispatching a replay");
    {
        SpeechJournal journal;
        Check(journal.map(mapping.handle()) && Matches(journal, {}),
              "each phrase survives until it has been replayed once");
    }
    SpeechJournal fresh;
    Check(mapping.reset() && fresh.map(mapping.handle()) && Matches(fresh, {}),
          "a planned handover starts with a fresh journal");
}

struct JournalLog final : vb::ILog {
    void Write(vb::LogLevel level, const std::wstring& line) override {
        ++lines;
        loggedReset = level == vb::LogLevel::Error &&
                      line == L"The speech journal was damaged and has been reset.";
    }
    vb::LogLevel Level() const override { return vb::LogLevel::Error; }
    int lines = 0;
    bool loggedReset = false;
};

using vb::speech_journal::Storage;

volatile LONG64* Header(Storage& storage, std::uint64_t cursor) {
    using namespace vb::speech_journal;
    return &storage.ring[((cursor - 1) % Capacity) / Word];
}

Storage* OpenStorage(HANDLE handle) {
    return static_cast<Storage*>(MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(Storage)));
}

void RecoverDamagedJournal() {
    const struct {
        const char* description;
        void (*damage)(Storage&, std::uint64_t);
    } cases[] = {
        {"recovery rejects positions with no space left for a future record", [](Storage& s, std::uint64_t) {
            constexpr auto nearLimit = (std::numeric_limits<LONG64>::max)() - 7;
            InterlockedExchange64(&s.begin, nearLimit);
            InterlockedExchange64(&s.end, nearLimit);
        }},
        {"recovery rejects a reversed range", [](Storage& s, std::uint64_t) {
            InterlockedExchange64(&s.begin, 8192);
        }},
        {"recovery rejects an unaligned range", [](Storage& s, std::uint64_t) {
            InterlockedExchange64(&s.begin, 1);
        }},
        {"recovery rejects a range larger than the ring", [](Storage& s, std::uint64_t) {
            InterlockedExchange64(&s.end, vb::speech_journal::Capacity + vb::speech_journal::Word);
        }},
        {"recovery rejects a negative position", [](Storage& s, std::uint64_t) {
            InterlockedExchange64(&s.end, -8);
        }},
        {"recovery discards the whole queue when a later length exceeds the limit", [](Storage& s, std::uint64_t cursor) {
            InterlockedExchange64(Header(s, cursor), 100001);
        }},
        {"recovery rejects a record extending past the published end", [](Storage& s, std::uint64_t cursor) {
            InterlockedExchange64(Header(s, cursor), 100000);
        }},
    };
    for (const auto& test : cases) {
        JournalLog log;
        SpeechJournalMapping mapping;
        SpeechJournal journal;
        if (!mapping.create() || !journal.map(mapping.handle())) {
            Check(false, "create and map journal for damage recovery");
            return;
        }
        const auto first = journal.append(L"first");
        const auto second = journal.append(L"second");
        auto* storage = OpenStorage(mapping.handle());
        if (!storage) {
            Check(false, "open the shared view to damage the journal");
            return;
        }
        test.damage(*storage, second);
        UnmapViewOfFile(storage);

        Check(journal.recover(&log).empty() && log.lines == 1 && log.loggedReset, test.description);
        const auto fresh = journal.append(L"fresh");
        journal.retire(first);
        journal.retireThrough(second);
        journal.beginReplay(second);
        Check(fresh > second && Matches(journal, {{fresh, L"fresh"}}) && log.lines == 1,
              "a reset journal accepts new speech and ignores stale tickets");
    }
}

void ReclaimDamagedJournal() {
    JournalLog log;
    SpeechJournalMapping mapping;
    SpeechJournal journal;
    if (!mapping.create() || !journal.map(mapping.handle())) {
        Check(false, "create and map journal for damage during reclamation");
        return;
    }
    journal.recover(&log);
    const auto first = journal.append(L"first");
    const auto second = journal.append(L"second");
    journal.retire(second);
    auto* storage = OpenStorage(mapping.handle());
    if (!storage) {
        Check(false, "open the shared view to damage a retired record");
        return;
    }
    auto* header = Header(*storage, second);
    InterlockedOr64(header, 1LL << 17);
    UnmapViewOfFile(storage);

    journal.retire(first);
    Check(log.lines == 1 && log.loggedReset, "reclaim detects a damaged retired length and logs the reset");
    const auto fresh = journal.append(L"fresh");
    journal.retireThrough(second);
    Check(fresh > second && Matches(journal, {{fresh, L"fresh"}}), "reclaim leaves the journal ready for speech");
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR args, int) {
    if (std::strncmp(args, "commit ", 7) == 0) return CrashChild(args + 7, CrashMode::CommittedAppend);
    if (std::strncmp(args, "interrupt ", 10) == 0) return CrashChild(args + 10, CrashMode::InterruptedAppend);
    if (std::strncmp(args, "replay ", 7) == 0) return CrashChild(args + 7, CrashMode::StartedReplay);
    report.open("journal-test.log");
    if (!report) return 1;
    WrapAndOverflow();
    CancelAndRetire();
    RecoverAfterCrash();
    RecoverDamagedJournal();
    ReclaimDamagedJournal();
    return failures ? 1 : 0;
}
