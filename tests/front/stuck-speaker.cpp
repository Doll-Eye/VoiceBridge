#include "../../src/common/speaker.h"
#include "../../src/front/handle.h"

namespace vb {
namespace {
void WaitForEvent(const wchar_t* name) {
    const Handle event(OpenEventW(SYNCHRONIZE, FALSE, name));
    if (event) WaitForSingleObject(event.get(), INFINITE);
}

class StuckSpeaker final : public ISpeaker {
public:
    void Speak(const std::wstring&) override {}
    void Cancel() override {}
    bool IsSpeaking() override { return false; }

    void Shutdown() override {
        const Handle entered(OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\VoiceBridge.Test.ShutdownEntered"));
        if (entered) SetEvent(entered.get());
        WaitForEvent(L"Local\\VoiceBridge.Test.StickShutdown");
    }
};
}

std::unique_ptr<ISpeaker> CreateSpeaker(const SpeakerConfig&, ILog&) {
    WaitForEvent(L"Local\\VoiceBridge.Test.DelayCreate");
    return std::make_unique<StuckSpeaker>();
}
}
