// Extra lifetime and recovery services available to a supervised RPC speaker.
#pragma once
#include "speaker.h"
#include <windows.h>

namespace vb {
class SpeechJournal;
inline constexpr DWORD SpeechWorkersStuck = 72;
std::unique_ptr<ISpeaker> CreateServerSpeaker(const SpeakerConfig& config, ILog& log,
                                             SpeechJournal& journal, HANDLE exhausted);
}
