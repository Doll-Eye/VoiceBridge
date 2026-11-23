#pragma once
#include "../common/speaker.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace vb {
enum class SignInStart { Auto, Yes, No };

struct Settings {
    SpeakerConfig speaker;
    LogLevel level = LogLevel::Info;
    bool logText = false;
    std::vector<std::wstring> nvdaNames;
    int signInWaitSeconds = 120;
    SignInStart startsAtSignIn = SignInStart::Auto;
};

Settings ReadSettings(const std::filesystem::path& directory);
bool NvdaInAccessibilityConfiguration(std::wstring_view configuration);
std::optional<bool> NvdaConfiguredAtSignIn();
}
