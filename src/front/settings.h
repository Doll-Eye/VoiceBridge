#pragma once
#include "../common/speaker.h"

#include <filesystem>
#include <vector>

namespace vb {
struct Settings {
    SpeakerConfig speaker;
    LogLevel level = LogLevel::Info;
    bool logText = false;
    std::vector<std::wstring> nvdaNames;
};

Settings ReadSettings(const std::filesystem::path& directory);
}
