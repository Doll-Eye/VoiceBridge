#include "voice_internal.h"

#include <algorithm>
#include <cwctype>
#include <thread>
#ifdef VB_VOICE_TEST
#include <pthread.h>
#endif

namespace vb::voice {

#ifdef VB_VOICE_TEST
namespace {
std::mutex threadListMutex;
std::list<HANDLE> testThreads;
}

bool test::Flag(const char* name) {
    char value[2];
    return GetEnvironmentVariableA(name, value, sizeof(value)) > 0;
}

bool test::WaitForVoiceThreads(unsigned long milliseconds) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        HANDLE thread;
        {
            std::lock_guard<std::mutex> lock(threadListMutex);
            if (testThreads.empty()) return true;
            thread = testThreads.front();
        }
        if (WaitForSingleObject(thread, WaitTime(deadline)) != WAIT_OBJECT_0) return false;
        std::lock_guard<std::mutex> lock(threadListMutex);
        CloseHandle(thread);
        testThreads.pop_front();
    }
}
#endif

void StartVoiceThread(const wchar_t* name, std::function<void()> run) {
#ifdef VB_VOICE_TEST
    std::lock_guard<std::mutex> lock(threadListMutex);
    std::thread thread([name, run = std::move(run)] {
        SetThreadDescription(GetCurrentThread(), name);
        run();
    });
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), pthread_gethandle(thread.native_handle()),
                         GetCurrentProcess(), &handle, SYNCHRONIZE, FALSE, 0)) std::terminate();
    testThreads.push_back(handle);
    thread.detach();
#else
    (void)name;
    std::thread(std::move(run)).detach();
#endif
}

void State::report(LogLevel level, const std::wstring& line, const wchar_t* once) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (log && level <= log->Level() && (!once || logged.insert(once).second)) log->Write(level, line);
}

void State::disconnectLog() {
    std::lock_guard<std::mutex> lock(logMutex);
    log = nullptr;
}

void State::queueChunk(Chunk chunk) {
    waiting += chunk.format.seconds(chunk.count);
    chunk.job->bufferedFrames += chunk.count;
    chunk.job->published = true;
    const bool first = audio.empty();
    audio.push_back(std::move(chunk));
#ifdef VB_VOICE_TEST
    test::Observe({test::Kind::Buffered, audio.back().id, audio.back().generation, waiting * 1000});
#endif
    if (first) render.signal();
}

void State::consume(size_t frames) {
    auto& chunk = audio.front();
    waiting = std::max(0.0, waiting - chunk.format.seconds(frames));
    chunk.offset += frames;
    chunk.count -= frames;
    chunk.job->bufferedFrames -= frames;
    if (!chunk.job->bufferedFrames || !jobs.empty()) dispatch.signal();
    if (!chunk.count) audio.pop_front();
    space.notify_one();
}

class Speaker final : public ISpeaker {
public:
    Speaker(const SpeakerConfig& config, ILog& log) : state_(std::make_shared<State>(config, log)) {
        auto state = state_;
        try {
            StartVoiceThread(L"VoiceBridge audio", [state] {
                try {
                    RunAudio(state);
                } catch (...) {
                    state->report(LogLevel::Error, L"The audio thread stopped unexpectedly.", L"audio-thread");
                }
                state->audioDone.signal();
            });
        } catch (...) {
            state->audioDone.signal();
        }
        try {
            StartVoiceThread(L"VoiceBridge scheduler", [state] {
                try {
                    RunSynthesis(state);
                } catch (...) {
                    state->report(LogLevel::Error, L"The voice scheduler stopped unexpectedly.", L"voice-scheduler");
                }
            });
        } catch (...) {
            state->report(LogLevel::Error, L"The voice scheduler could not start.", L"voice-start");
        }
    }

    ~Speaker() override { Shutdown(); }

    void Speak(const std::wstring& text) override {
        if (std::all_of(text.begin(), text.end(), [](wchar_t ch) {
                return std::iswspace(ch) || ch == 0x0085 || ch == 0x00A0 ||
                       ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200A) ||
                       ch == 0x2028 || ch == 0x2029 || ch == 0x202F || ch == 0x205F || ch == 0x3000;
            })) return;
        std::wstring copy(text);
        auto& state = *state_;
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.stop) return;
        auto job = std::make_shared<Job>(++state.lastJob, state.generation.load(), std::move(copy));
#ifdef VB_VOICE_TEST
        test::Observe({test::Kind::Spoke, job->id, job->generation});
#endif
        state.jobs.push_back(std::move(job));
        state.dispatch.signal();
    }

    void Cancel() override {
        auto& state = *state_;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.stop) return;
            ++state.generation;
            clear(state);
            const auto now = Clock::now();
            state.audibleUntil = now < state.audibleUntil
                ? std::min(state.audibleUntil + 5ms, now + 25ms) : Time{};
#ifdef VB_VOICE_TEST
            test::Observe({test::Kind::Cancelled, 0, state.generation.load()});
#endif
        }
        wake(state);
    }

    bool IsSpeaking() override {
        auto& state = *state_;
        std::lock_guard<std::mutex> lock(state.mutex);
        return !state.stop && (state.active || !state.jobs.empty() || !state.audio.empty() ||
                              Clock::now() < state.audibleUntil);
    }

    void Shutdown() override {
        auto& state = *state_;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.stop.exchange(true)) return;
            ++state.generation;
            clear(state);
            state.audibleUntil = {};
        }
        wake(state);
        WaitForSingleObject(state.audioDone.get(), 80);
        state.disconnectLog();
    }

private:
    static void clear(State& state) {
        state.retiredJobs.splice(state.retiredJobs.end(), state.jobs);
        state.retiredAudio.splice(state.retiredAudio.end(), state.audio);
        state.active.reset();
        state.waiting = 0;
    }
    static void wake(State& state) {
        state.space.notify_all();
        state.dispatch.signal();
        state.render.signal();
    }

    std::shared_ptr<State> state_;
};

}  // namespace vb::voice

namespace vb {

std::unique_ptr<ISpeaker> CreateSpeaker(const SpeakerConfig& config, ILog& log) {
    return std::make_unique<voice::Speaker>(config, log);
}

}  // namespace vb
