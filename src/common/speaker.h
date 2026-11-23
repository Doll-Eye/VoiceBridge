// speaker.h: the fixed contract between VoiceBridge's two halves.
//
// The front half (src/front: RPC server, look-alike NVDA window, supervisor,
// settings, log) calls this. The voice half (src/voice: the natural voice,
// the audio device, the phrase cache) implements it. Neither half may change
// this file; say so in your report if you think it is wrong.
#pragma once

#include <memory>
#include <string>

namespace vb {

enum class LogLevel { Error = 0, Info = 1, Debug = 2 };

// Supplied by the front half. Thread-safe.
// Spoken text must never be written at Error or Info level.
struct ILog {
    virtual ~ILog() = default;
    virtual void Write(LogLevel level, const std::wstring& line) = 0;
    virtual LogLevel Level() const = 0;
};

// Filled by the front half from VoiceBridge.ini.
struct SpeakerConfig {
    std::wstring voice = L"Jenny";   // part of the SAPI voice's description; case is ignored
    std::wstring fallbackVoice;      // used when `voice` is not found; empty means any installed voice
    int rate = 0;                    // SAPI rate, -10 to 10
    int volume = 100;                // 0 to 100; the speaker applies it as gain
    int cacheMegabytes = 64;         // phrase cache; 0 switches it off
    int keepAwakeSeconds = 30;       // how long the audio stream keeps running after the last speech
    bool followNarrator = true;      // take voice and speed from Narrator's own settings where they can be read
    bool silentOutput = false;       // test mode: everything real, but samples are multiplied by zero before the device
    bool nullOutput = false;         // test mode: no audio device at all; audio is counted and discarded in real time
};

// Implemented by the voice half. The speaker owns its own threads and does
// its own COM initialisation on them; callers need no COM set-up.
struct ISpeaker {
    virtual ~ISpeaker() = default;

    // Speak, Cancel and IsSpeaking are thread-safe, may be called from any
    // thread (they arrive on RPC threads), and return within a millisecond.

    // Say `text` after whatever is already being said (NVDA's speakText).
    virtual void Speak(const std::wstring& text) = 0;

    // Fall silent now and forget everything queued (NVDA's cancelSpeech).
    virtual void Cancel() = 0;

    // True while anything is queued or sounding (NVDA's isSpeaking).
    virtual bool IsSpeaking() = 0;

    // Stop all sound at once and let go of the audio device.
    // Returns within 100 ms; the process may end straight afterwards.
    virtual void Shutdown() = 0;
};

// Never returns null. If the wanted voice or the audio device is missing, the
// speaker still starts, uses what it can, keeps trying, and logs it once.
std::unique_ptr<ISpeaker> CreateSpeaker(const SpeakerConfig& config, ILog& log);

}  // namespace vb
