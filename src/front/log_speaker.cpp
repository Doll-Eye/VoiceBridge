#include "../common/server_speaker.h"

namespace vb {
namespace {
class LogSpeaker final : public ISpeaker {
public:
    explicit LogSpeaker(ILog& log) : log_(log) {}

    void Speak(const std::wstring& text) override {
        if (log_.Level() == LogLevel::Debug) {
            log_.Write(LogLevel::Debug, L"Speak: " + text);
        }
    }

    void Cancel() override { log_.Write(LogLevel::Debug, L"Cancel"); }
    bool IsSpeaking() override {
        log_.Write(LogLevel::Debug, L"IsSpeaking: false");
        return false;
    }
    void Shutdown() override {}

private:
    ILog& log_;
};
}

std::unique_ptr<ISpeaker> CreateSpeaker(const SpeakerConfig&, ILog& log) {
    return std::make_unique<LogSpeaker>(log);
}

std::unique_ptr<ISpeaker> CreateServerSpeaker(const SpeakerConfig& config, ILog& log, SpeechJournal&, HANDLE) {
    return CreateSpeaker(config, log);
}
}
