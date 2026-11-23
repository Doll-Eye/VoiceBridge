#include "../../src/common/speaker.h"
#include "observer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using vb::voice::test::Kind;
using vb::voice::test::NarratorSnapshot;
using vb::voice::test::Observation;

constexpr wchar_t JennySetting[] = L"Microsoft Jenny(Natural) - English (United States)";
constexpr wchar_t DavidSetting[] = L"Microsoft David Desktop - English (United States)";
constexpr wchar_t Repeated[] = L"Inventory.";
constexpr unsigned JennyHz = 24000;
constexpr unsigned DavidHz = 16000;

std::mutex snapshotMutex;
NarratorSnapshot snapshot{JennySetting, 14, 16};

NarratorSnapshot ReadSnapshot() {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return snapshot;
}

bool PublishSnapshot(HANDLE changed, NarratorSnapshot next) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    snapshot = std::move(next);
    return SetEvent(changed) != FALSE;
}

struct Phrase {
    std::size_t frames = 0;
    unsigned rate = 0;
    bool wrongFormat = false;
    bool complete = false;
    bool cached = false;
    bool synthesized = false;

    bool matches(unsigned expectedRate, bool cacheHit) const {
        return frames > 0 && complete && rate == expectedRate && !wrongFormat &&
               cached == cacheHit && synthesized != cacheHit;
    }
};

class Probe final : public vb::ILog {
public:
    void observe(const Observation& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (event.kind) {
        case Kind::Spoke:
            lastQueued_ = event.id;
            phrases_.emplace(event.id, Phrase{});
            return;
        case Kind::RawOnset: phrases_.at(event.id).synthesized = true; break;
        case Kind::CacheHit: phrases_.at(event.id).cached = true; break;
        case Kind::Complete: phrases_.at(event.id).complete = true; break;
        case Kind::Packet: {
            auto& phrase = phrases_.at(event.id);
            if (event.channels != 1 || (phrase.rate && phrase.rate != event.rate)) phrase.wrongFormat = true;
            phrase.frames += event.channels ? event.count / event.channels : 0;
            phrase.rate = event.rate;
            if (event.id < lastPlayed_) ordered_ = false;
            lastPlayed_ = event.id;
            break;
        }
        default: return;
        }
        changed_.notify_all();
    }

    void Write(vb::LogLevel level, const std::wstring& line) override {
        if (level != vb::LogLevel::Info || line.find(L"SAPI rate ") == std::wstring::npos) return;
        std::lock_guard<std::mutex> lock(mutex_);
        choices_.emplace_back(line, Clock::now());
        changed_.notify_all();
    }
    vb::LogLevel Level() const override { return vb::LogLevel::Info; }

    std::optional<Clock::time_point> waitChoice(const std::wstring& voice, int rate, unsigned hz,
                                                std::chrono::milliseconds timeout) {
        const auto details = L"SAPI rate " + std::to_wstring(rate) + L". PCM " + std::to_wstring(hz) + L" Hz";
        std::unique_lock<std::mutex> lock(mutex_);
        std::optional<Clock::time_point> chosenAt;
        changed_.wait_for(lock, timeout, [&] {
            for (const auto& choice : choices_) {
                if (choice.first.find(voice) == 0 && choice.first.find(details) != std::wstring::npos) {
                    chosenAt = choice.second;
                    return true;
                }
            }
            return false;
        });
        return chosenAt;
    }

    std::uint64_t speak(vb::ISpeaker& speaker, const wchar_t* text) {
        const auto previous = lastQueued();
        speaker.Speak(text);
        const auto current = lastQueued();
        return current == previous ? 0 : current;
    }

    bool waitForPackets(std::uint64_t id) {
        if (!id) return false;
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 3s, [&] { return phrases_.at(id).frames > 0; });
    }

    Phrase phrase(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = phrases_.find(id);
        return found == phrases_.end() ? Phrase{} : found->second;
    }

    bool ordered() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ordered_;
    }

private:
    std::uint64_t lastQueued() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastQueued_;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::pair<std::wstring, Clock::time_point>> choices_;
    std::map<std::uint64_t, Phrase> phrases_;
    std::uint64_t lastQueued_ = 0;
    std::uint64_t lastPlayed_ = 0;
    bool ordered_ = true;
};

Probe* probe = nullptr;
void Observe(const Observation& event) { probe->observe(event); }

bool Drain(vb::ISpeaker& speaker, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (speaker.IsSpeaking() && Clock::now() < deadline) Sleep(25);
    return !speaker.IsSpeaking();
}

bool Check(std::ostream& out, bool result, const std::string& description) {
    out << (result ? "PASS " : "FAIL ") << description << std::endl;
    return result;
}

bool ReportChoice(std::ostream& out, const std::optional<Clock::time_point>& selected,
                  Clock::time_point began, const std::string& description) {
    const auto delay = selected ? std::chrono::duration_cast<std::chrono::milliseconds>(*selected - began).count() : 0;
    return Check(out, selected.has_value(), description +
                 (selected ? " after " + std::to_string(delay) + " ms." : " was not selected before the deadline."));
}

bool Exercise(std::ostream& out, Probe& trace, vb::ISpeaker& speaker, HANDLE changed) {
    const auto jenny = trace.waitChoice(L"Voice: Microsoft Jenny (Natural)", 4, JennyHz, 8s);
    if (!Check(out, jenny.has_value(), "Initial Narrator Jenny at neural rate 4 and 24000 Hz.")) return false;

    const auto prime = trace.speak(speaker, Repeated);
    const bool primed = trace.waitForPackets(prime) && Drain(speaker, 3s) && trace.phrase(prime).matches(JennyHz, false);
    const auto reused = trace.speak(speaker, Repeated);
    const bool reusedCache = trace.waitForPackets(reused) && Drain(speaker, 3s) && trace.phrase(reused).matches(JennyHz, true);
    if (!Check(out, primed && reusedCache, "Initial Jenny phrase synthesized and then replayed from cache.")) return false;

    const auto old = trace.speak(speaker,
        L"The bridge crosses the river below the old town. Lanterns light the path to the gate. "
        L"The gate is still open, and someone is waiting on the far side with a map and a letter for the next rider.");
    if (!Check(out, trace.waitForPackets(old) && speaker.IsSpeaking(), "Jenny was speaking when settings changed.")) return false;
    const auto changedAt = Clock::now();
    const bool signaledDavid = PublishSnapshot(changed, {DavidSetting, 19, 13});
    const auto david = signaledDavid ? trace.waitChoice(L"Voice: Microsoft David Desktop", 3, DavidHz, 4s) : std::nullopt;
    if (!ReportChoice(out, david, changedAt, "Narrator David at speech rate 3 and 16000 Hz")) return false;

    const auto davidId = trace.speak(speaker, Repeated);
    const bool davidPlayed = Drain(speaker, 12s);
    const auto oldPhrase = trace.phrase(old);
    const auto davidPhrase = trace.phrase(davidId);
    const bool changedFormat = Check(out, davidPlayed && oldPhrase.matches(JennyHz, false) &&
                                     davidPhrase.matches(DavidHz, false) && trace.ordered(),
                                     "The old Jenny phrase finished in order and David synthesized the cached text afresh. " +
                                     std::to_string(oldPhrase.frames) + " Jenny frames and " +
                                     std::to_string(davidPhrase.frames) + " David frames.");

    const auto absentAt = Clock::now();
    const bool signaledMissing = PublishSnapshot(changed, {});
    const auto configured = signaledMissing ? trace.waitChoice(L"Voice: Microsoft Jenny (Natural)", -2, JennyHz, 4s) : std::nullopt;
    if (!ReportChoice(out, configured, absentAt, "Configured Jenny at rate -2 and 24000 Hz after Narrator values disappeared"))
        return false;
    const auto last = trace.speak(speaker, Repeated);
    const bool rendered = trace.waitForPackets(last) && Drain(speaker, 3s);
    const bool fresh = Check(out, rendered && trace.phrase(last).matches(JennyHz, false),
                             "Jenny synthesized the repeated phrase again at the configured rate.");
    return changedFormat && fresh;
}

int RunTests() {
    std::ofstream out("settings-results.txt");
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!Check(out, changed != nullptr, "Created the test Narrator notification event.")) return 1;

    Probe trace;
    probe = &trace;
    const vb::voice::test::NarratorSource source{changed, ReadSnapshot};
    vb::voice::test::narratorSource = &source;
    vb::SpeakerConfig config;
    config.voice = L"Jenny (Natural)";
    config.rate = -2;
    config.cacheMegabytes = 2;
    config.followNarrator = true;
    config.nullOutput = true;
    auto speaker = vb::CreateSpeaker(config, trace);
    const bool passed = Exercise(out, trace, *speaker, changed);
    speaker->Shutdown();
    speaker.reset();
    if (!Check(out, vb::voice::test::WaitForVoiceThreads(5000), "Voice threads exited.")) {
        TerminateProcess(GetCurrentProcess(), 1);
        return 1;
    }
    vb::voice::test::narratorSource = nullptr;
    probe = nullptr;
    CloseHandle(changed);
    out << "Failures: " << (passed ? 0 : 1) << std::endl;
    return passed ? 0 : 1;
}

}  // namespace

namespace vb::voice::test {
Sink observer = ::Observe;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int code = RunTests();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
