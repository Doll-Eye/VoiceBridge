#pragma once

#include "speaker.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vb {
namespace speech_journal {

constexpr std::size_t Capacity = 2 * 1024 * 1024;
constexpr std::size_t Word = sizeof(LONG64);

struct alignas(Word) Storage {
    volatile LONG64 begin;
    volatile LONG64 end;
    LONG64 ring[Capacity / Word];
};

static_assert(offsetof(Storage, ring) % Word == 0);
static_assert(Capacity % Word == 0);
static_assert(sizeof(wchar_t) == 2);

}  // namespace speech_journal

// The supervisor holds the unnamed section open between child processes.
// Windows backs it with the paging file. VoiceBridge never opens a data file.
class SpeechJournalMapping {
public:
    SpeechJournalMapping() = default;
    SpeechJournalMapping(const SpeechJournalMapping&) = delete;
    SpeechJournalMapping& operator=(const SpeechJournalMapping&) = delete;
    ~SpeechJournalMapping() { if (handle_) CloseHandle(handle_); }

    bool create() noexcept {
        if (handle_) return true;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                     sizeof(speech_journal::Storage), nullptr);
        return handle_ != nullptr;
    }

    HANDLE handle() const noexcept { return handle_; }

    // Call after the old child has exited. Fresh backing also discards the
    // old journal's contents without mapping or traversing it in the parent.
    bool reset() noexcept {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
        return create();
    }

private:
    HANDLE handle_ = nullptr;
};

// One child owns the view. Serialize all access through the speaker state lock.
// The positions and record flags are atomic across processes. End is published
// only after the whole record has been copied.
class SpeechJournal {
public:
    struct Entry {
        std::uint64_t cursor;
        std::wstring text;
    };

    SpeechJournal() = default;
    SpeechJournal(const SpeechJournal&) = delete;
    SpeechJournal& operator=(const SpeechJournal&) = delete;
    ~SpeechJournal() { if (storage_) UnmapViewOfFile(storage_); }

    bool map(HANDLE handle) noexcept {
        auto* storage = static_cast<speech_journal::Storage*>(
            MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(speech_journal::Storage)));
        if (!storage) return false;
        if (storage_) UnmapViewOfFile(storage_);
        storage_ = storage;
        committedEnd_ = 0;
        log_ = nullptr;
        return true;
    }

    std::uint64_t append(std::wstring_view text) {
        if (!storage_ || text.size() > MaxChars) throw std::bad_alloc();
        auto range = checkedRange();
        reclaim(range);
        const auto size = recordSize(text.size());
        if (size > Capacity - (range.end - range.begin) || size > MaxPosition - range.end) throw std::bad_alloc();

        store(header(range.end), text.size());
        writeText(range.end + Word, text);
#ifdef VB_VOICE_TEST
        if (beforePublish) beforePublish();
#endif
        committedEnd_ = range.end + size;
        store(&storage_->end, committedEnd_);
        return range.end + 1;
    }

    // The logger must outlive journal use by this speaker.
    std::vector<Entry> recover(ILog* log = nullptr) {
        if (log) log_ = log;
        std::vector<Entry> entries;
        if (!storage_) return entries;
        auto range = checkedRange();
        for (auto position = range.begin; position < range.end;) {
            Record record;
            if (!readRecord(position, range.end, record)) {
                resetDamaged();
                return {};
            }
            if (!(record.bits & Retired)) {
                if (record.bits & Replayed) {
                    store(header(position), record.bits | Retired);
                } else {
                    Entry entry{position + 1, std::wstring(record.length, L'\0')};
                    readText(position + Word, entry.text);
                    entries.push_back(std::move(entry));
                }
            }
            position = record.next;
        }
        reclaim(range);
        return entries;
    }

    // Charge a recovered phrase when it is dispatched. Phrases still waiting
    // behind it can survive another crash.
    void beginReplay(std::uint64_t cursor) noexcept {
        mark(cursor, Replayed);
    }

    // Accept tickets returned by this journal, including older tickets after
    // retirement or cancellation. Zero can be used for an absent ticket.
    void retire(std::uint64_t cursor) noexcept {
        mark(cursor, Retired);
    }

    void retireThrough(std::uint64_t cursor) noexcept {
        if (!storage_ || !cursor) return;
        const auto position = cursor - 1;
        auto range = checkedRange();
        if (!contains(range, position)) return;
        Record record;
        if (!readRecord(position, range.end, record)) {
            resetDamaged();
            return;
        }
        range.begin = record.next;
        store(&storage_->begin, range.begin);
        reclaim(range);
    }

    void clear() noexcept {
        if (storage_) store(&storage_->begin, checkedRange().end);
    }

#ifdef VB_VOICE_TEST
    inline static void (*beforePublish)() = nullptr;
#endif

private:
    static constexpr auto Capacity = speech_journal::Capacity;
    static constexpr auto Word = speech_journal::Word;
    static constexpr std::size_t MaxChars = 100000;
    static constexpr std::uint64_t Retired = std::uint64_t{1} << 32;
    static constexpr std::uint64_t Replayed = std::uint64_t{1} << 33;
    static constexpr std::uint64_t MaxPosition = (std::numeric_limits<LONG64>::max)();

    struct Range {
        std::uint64_t begin;
        std::uint64_t end;
    };

    struct Record {
        std::uint64_t bits;
        std::uint64_t length;
        std::uint64_t next;
    };

    static std::uint64_t load(volatile LONG64* value) noexcept {
        return InterlockedCompareExchange64(value, 0, 0);
    }

    static void store(volatile LONG64* value, std::uint64_t next) noexcept {
        InterlockedExchange64(value, static_cast<LONG64>(next));
    }

    static std::size_t recordSize(std::size_t length) noexcept {
        const auto bytes = length * sizeof(wchar_t);
        return Word + ((bytes + Word - 1) / Word) * Word;
    }

    volatile LONG64* header(std::uint64_t position) const noexcept {
        return &storage_->ring[(position % Capacity) / Word];
    }

    Range resetDamaged() noexcept {
        // Keep every ticket already issued by this view below the new begin.
        // The invalid intermediate begin makes a crash during reset detectable.
        store(&storage_->begin, MaxPosition);
        store(&storage_->end, committedEnd_);
        store(&storage_->begin, committedEnd_);
        if (log_) {
            try {
                log_->Write(LogLevel::Error, L"The speech journal was damaged and has been reset.");
            } catch (...) {
                // A logging failure must leave the journal usable.
            }
        }
        return {committedEnd_, committedEnd_};
    }

    Range checkedRange() noexcept {
        const auto begin = load(&storage_->begin);
        const auto end = load(&storage_->end);
        if (begin > end || end > MaxPosition - Capacity || end - begin > Capacity ||
            begin % Word || end % Word || end < committedEnd_)
            return resetDamaged();
        committedEnd_ = end;
        return {begin, end};
    }

    static bool contains(Range range, std::uint64_t position) noexcept {
        return position >= range.begin && position < range.end && position % Word == 0;
    }

    bool readRecord(std::uint64_t position, std::uint64_t end, Record& record) const noexcept {
        record.bits = load(header(position));
        record.length = record.bits & ~(Retired | Replayed);
        if (record.length > MaxChars) return false;
        const auto size = recordSize(record.length);
        if (size > end - position) return false;
        record.next = position + size;
        return true;
    }

    void mark(std::uint64_t cursor, std::uint64_t flag) noexcept {
        if (!storage_ || !cursor) return;
        const auto position = cursor - 1;
        auto range = checkedRange();
        if (!contains(range, position)) return;
        Record record;
        if (!readRecord(position, range.end, record)) {
            resetDamaged();
            return;
        }
        store(header(position), record.bits | flag);
        if (position == range.begin && flag == Retired) reclaim(range);
    }

    void reclaim(Range& range) noexcept {
        auto position = range.begin;
        while (position < range.end) {
            Record record;
            if (!readRecord(position, range.end, record)) {
                range = resetDamaged();
                return;
            }
            if (!(record.bits & Retired)) break;
            position = record.next;
        }
        if (position != range.begin) {
            range.begin = position;
            store(&storage_->begin, position);
        }
    }

    void writeText(std::uint64_t position, std::wstring_view text) noexcept {
        if (text.empty()) return;
        auto* ring = reinterpret_cast<unsigned char*>(storage_->ring);
        const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
        const auto offset = static_cast<std::size_t>(position % Capacity);
        const auto length = text.size() * sizeof(wchar_t);
        const auto first = (std::min)(length, Capacity - offset);
        std::memcpy(ring + offset, bytes, first);
        std::memcpy(ring, bytes + first, length - first);
    }

    void readText(std::uint64_t position, std::wstring& text) const noexcept {
        if (text.empty()) return;
        const auto* ring = reinterpret_cast<const unsigned char*>(storage_->ring);
        auto* bytes = reinterpret_cast<unsigned char*>(text.data());
        const auto offset = static_cast<std::size_t>(position % Capacity);
        const auto length = text.size() * sizeof(wchar_t);
        const auto first = (std::min)(length, Capacity - offset);
        std::memcpy(bytes, ring + offset, first);
        std::memcpy(bytes + first, ring, length - first);
    }

    speech_journal::Storage* storage_ = nullptr;
    std::uint64_t committedEnd_ = 0;
    ILog* log_ = nullptr;
};

}  // namespace vb
