#include "settings.h"

#include <windows.h>
#include <algorithm>

namespace vb {
namespace {
class Profile {
public:
    explicit Profile(std::filesystem::path file) : file_(std::move(file)) {}

    std::wstring String(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const {
        std::wstring value(32768, L'\0');
        value.resize(GetPrivateProfileStringW(
            section, key, fallback, value.data(), static_cast<DWORD>(value.size()), file_.c_str()));
        return value;
    }

    int Integer(const wchar_t* section, const wchar_t* key, int fallback) const {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, file_.c_str()));
    }

private:
    std::filesystem::path file_;
};
}

Settings ReadSettings(const std::filesystem::path& directory) {
    const Profile ini(directory / L"VoiceBridge.ini");
    Settings settings;
    auto& speaker = settings.speaker;
    speaker.voice = ini.String(L"voice", L"name", speaker.voice.c_str());
    speaker.fallbackVoice = ini.String(L"voice", L"fallback", speaker.fallbackVoice.c_str());
    speaker.rate = std::clamp(ini.Integer(L"voice", L"rate", speaker.rate), -10, 10);
    speaker.volume = std::clamp(ini.Integer(L"voice", L"volume", speaker.volume), 0, 100);
    speaker.followNarrator = ini.Integer(L"voice", L"followNarrator", speaker.followNarrator) != 0;
    speaker.keepAwakeSeconds = std::max(0, ini.Integer(L"audio", L"keepAwakeSeconds", speaker.keepAwakeSeconds));
    speaker.cacheMegabytes = std::max(0, ini.Integer(L"cache", L"megabytes", speaker.cacheMegabytes));
    const auto level = ini.String(L"log", L"level", L"info");
    if (_wcsicmp(level.c_str(), L"error") == 0) {
        settings.level = LogLevel::Error;
    } else if (_wcsicmp(level.c_str(), L"debug") == 0) {
        settings.level = LogLevel::Debug;
    }
    settings.logText = ini.Integer(L"log", L"text", 0) == 1;

    const auto names = ini.String(L"nvda", L"processNames",
                                  L"nvda.exe;nvda_uiAccess.exe;nvda_noUIAccess.exe");
    for (size_t start = 0; start < names.size();) {
        const auto separator = names.find(L';', start);
        const auto token = names.substr(start, separator - start);
        const auto first = token.find_first_not_of(L" \t");
        if (first != std::wstring::npos) {
            settings.nvdaNames.push_back(token.substr(first, token.find_last_not_of(L" \t") - first + 1));
        }
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return settings;
}
}
