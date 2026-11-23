#define NOMINMAX
#include <windows.h>

#include "../../src/common/speaker.h"
#include "observer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vb::voice::test::Kind;
using vb::voice::test::Observation;
constexpr size_t Capacity = 1024 * 1024;

struct Phrase {
    bool complete = false;
    bool hit = false;
    bool played = false;
};

struct Trace {
    unsigned warm = 0;
    std::uint64_t last = 0;
    size_t peak = 0;
    size_t charges = 0;
    std::vector<std::uint64_t> evicted;
    std::vector<std::uint64_t> playback;
    std::map<std::uint64_t, Phrase> phrases;
};

Trace trace;
std::mutex traceMutex;

void Observe(const Observation& event) {
    const std::lock_guard<std::mutex> lock(traceMutex);
    switch (event.kind) {
    case Kind::Spoke:
        trace.last = event.id;
        trace.phrases.try_emplace(event.id);
        break;
    case Kind::Warmed: ++trace.warm; break;
    case Kind::Complete: trace.phrases[event.id].complete = true; break;
    case Kind::CacheHit: trace.phrases[event.id].hit = true; break;
    case Kind::CacheBytes:
        trace.peak = std::max(trace.peak, static_cast<size_t>(event.value));
        ++trace.charges;
        break;
    case Kind::CacheEvicted: trace.evicted.push_back(event.id); break;
    case Kind::Packet: {
        auto& phrase = trace.phrases[event.id];
        if (!phrase.played) trace.playback.push_back(event.id);
        phrase.played = true;
        break;
    }
    default: break;
    }
}

template<class Read>
auto ReadTrace(Read read) {
    const std::lock_guard<std::mutex> lock(traceMutex);
    return read(trace);
}

template<class Condition>
bool Wait(std::chrono::milliseconds timeout, Condition condition) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (condition()) return true;
        Sleep(5);
    } while (std::chrono::steady_clock::now() < end);
    return condition();
}

class Log final : public vb::ILog {
public:
    void Write(vb::LogLevel, const std::wstring&) override {}
    vb::LogLevel Level() const override { return vb::LogLevel::Error; }
};

std::uint64_t Speak(vb::ISpeaker& speaker, const std::wstring& text) {
    speaker.Speak(text);
    return ReadTrace([](const Trace& value) { return value.last; });
}

Phrase ReadPhrase(std::uint64_t id) {
    return ReadTrace([id](const Trace& value) {
        const auto phrase = value.phrases.find(id);
        return phrase == value.phrases.end() ? Phrase{} : phrase->second;
    });
}

std::uint64_t Play(vb::ISpeaker& speaker, const std::wstring& text, bool expectHit) {
    const auto id = Speak(speaker, text);
    if (!id || !Wait(15s, [&] {
            const auto phrase = ReadPhrase(id);
            return phrase.complete && phrase.played && !speaker.IsSpeaking();
        })) return 0;
    return ReadPhrase(id).hit == expectHit ? id : 0;
}

std::wstring PhraseFor(unsigned number) {
    return L"The lantern glows at the edge of the forest. Walk beside the river until you "
           L"reach the stone bridge. Open gate " + std::to_wstring(number) + L".";
}

bool CheckCancellation(vb::ISpeaker& speaker) {
    const auto text = PhraseFor(100);
    const auto cancelled = Speak(speaker, text);
    if (!Wait(8s, [&] { return ReadPhrase(cancelled).complete; }) || !speaker.IsSpeaking()) return false;
    if (!ReadTrace([](const Trace& value) { return value.peak > 0; })) return false;
    speaker.Cancel();
    return Wait(3s, [&] { return !speaker.IsSpeaking(); }) && Play(speaker, text, false) != 0;
}

bool CheckLru(vb::ISpeaker& speaker) {
    const auto second = Play(speaker, PhraseFor(2), false);
    const auto touched = Play(speaker, PhraseFor(100), true);
    if (!second || !touched || ReadTrace([](const Trace& value) { return !value.evicted.empty(); })) return false;

    for (unsigned number = 3; number <= 10; ++number) {
        if (!Play(speaker, PhraseFor(number), false)) return false;
        const auto firstEvicted = ReadTrace([](const Trace& value) {
            return value.evicted.empty() ? std::uint64_t{} : value.evicted.front();
        });
        if (firstEvicted) return firstEvicted == second && Play(speaker, PhraseFor(2), false) != 0;
    }
    return false;
}

bool CheckFullQueue(vb::ISpeaker& speaker) {
    std::vector<std::uint64_t> queued;
    for (unsigned number = 20; number < 38; ++number) {
        queued.push_back(Speak(speaker, L"Take the wooden bridge to gate " + std::to_wstring(number) + L"."));
    }
    if (!Wait(55s, [&] { return !speaker.IsSpeaking(); })) return false;
    return ReadTrace([&](const Trace& value) {
        const auto first = std::find(value.playback.begin(), value.playback.end(), queued.front());
        return std::equal(first, value.playback.end(), queued.begin(), queued.end()) &&
               std::all_of(queued.begin(), queued.end(), [&](auto id) {
                   const auto phrase = value.phrases.find(id);
                   return phrase != value.phrases.end() && phrase->second.complete;
               });
    });
}

unsigned Run() {
    std::ofstream results("cache-results.txt");
    if (!results) return 2;
    Log log;
    vb::SpeakerConfig config;
    config.followNarrator = false;
    config.voice = L"Jenny (Natural)";
    config.fallbackVoice = L"David";
    config.rate = 4;
    config.cacheMegabytes = 1;
    config.nullOutput = true;
    auto speaker = vb::CreateSpeaker(config, log);
    const bool warmed = Wait(12s, [] { return ReadTrace([](const Trace& value) { return value.warm >= 2; }); });
    const bool cancelled = warmed && CheckCancellation(*speaker);
    const bool lru = cancelled && CheckLru(*speaker);
    const bool queue = lru && CheckFullQueue(*speaker);
    const bool recovery = queue && Play(*speaker, L"Inventory is ready.", false) &&
                          Play(*speaker, L"Inventory is ready.", true);
    speaker->Shutdown();
    const auto peak = ReadTrace([](const Trace& value) { return value.peak; });
    const auto evictions = ReadTrace([](const Trace& value) { return value.evicted.size(); });
    const auto charges = ReadTrace([](const Trace& value) { return value.charges; });
    const bool budget = peak > 0 && peak <= Capacity;
    results << (warmed ? "PASS" : "FAIL") << " Warmed two voices.\n"
            << (cancelled ? "PASS" : "FAIL") << " A cancelled complete phrase missed the cache on replay.\n"
            << (lru ? "PASS" : "FAIL") << " A hit protected the newer entry and the evicted phrase missed.\n"
            << (queue ? "PASS" : "FAIL") << " All 18 queued phrases completed and played in order.\n"
            << (recovery ? "PASS" : "FAIL") << " A new phrase played and became a hit after queue pressure.\n"
            << (budget ? "PASS" : "FAIL") << " Peak charged bytes " << peak << " of " << Capacity
            << ". Charge observations " << charges << ". Evictions " << evictions << ".\n";
    return warmed && cancelled && lru && queue && recovery && budget ? 0 : 1;
}

}  // namespace

namespace vb::voice::test {
Sink observer = ::Observe;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto result = Run();
    TerminateProcess(GetCurrentProcess(), result);
    return static_cast<int>(result);
}
