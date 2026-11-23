#include "settings.h"

#include <windows.h>
#include <cerrno>
#include <climits>
#include <cwchar>

namespace vb {
namespace {
class Profile {
public:
    explicit Profile(std::filesystem::path file) : file_(std::move(file)) {}

    std::wstring String(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const {
        std::wstring value(32768, L'\0');
        const auto length = GetPrivateProfileStringW(
            section, key, fallback, value.data(), static_cast<DWORD>(value.size()), file_.c_str());
        if (length == value.size() - 1) return fallback;
        value.resize(length);
        return value;
    }

    int Integer(const wchar_t* section, const wchar_t* key, int fallback, int low, int high) const {
        const auto value = String(section, key, L"");
        wchar_t* end = nullptr;
        errno = 0;
        const auto parsed = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str() || *end || errno == ERANGE || parsed < low || parsed > high) return fallback;
        return static_cast<int>(parsed);
    }

    bool Boolean(const wchar_t* section, const wchar_t* key, bool fallback) const {
        const auto value = String(section, key, L"");
        if (value == L"1") return true;
        if (value == L"0") return false;
        return fallback;
    }

private:
    std::filesystem::path file_;
};

constexpr wchar_t DefaultNvdaNames[] = L"nvda.exe;nvda_uiAccess.exe;nvda_noUIAccess.exe";

std::vector<std::wstring> ProcessNames(const std::wstring& names) {
    std::vector<std::wstring> result;
    for (size_t start = 0; start < names.size();) {
        const auto separator = names.find(L';', start);
        const auto token = names.substr(start, separator - start);
        const auto first = token.find_first_not_of(L" \t");
        if (first != std::wstring::npos) {
            auto name = token.substr(first, token.find_last_not_of(L" \t") - first + 1);
            if (name.find_first_of(L"/\\:*?\"<>|") == std::wstring::npos && name.back() != L'.' &&
                _wcsicmp(name.c_str(), L"VoiceBridge.exe") != 0) {
                result.push_back(std::move(name));
            }
        }
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return result;
}
}

Settings ReadSettings(const std::filesystem::path& directory) {
    const Profile ini(directory / L"VoiceBridge.ini");
    Settings settings;
    auto& speaker = settings.speaker;
    auto voice = ini.String(L"voice", L"name", L"");
    if (!voice.empty()) speaker.voice = std::move(voice);
    speaker.fallbackVoice = ini.String(L"voice", L"fallback", speaker.fallbackVoice.c_str());
    speaker.rate = ini.Integer(L"voice", L"rate", speaker.rate, -10, 10);
    speaker.volume = ini.Integer(L"voice", L"volume", speaker.volume, 0, 100);
    speaker.followNarrator = ini.Boolean(L"voice", L"followNarrator", speaker.followNarrator);
    speaker.keepAwakeSeconds = ini.Integer(L"audio", L"keepAwakeSeconds", speaker.keepAwakeSeconds, 0, INT_MAX);
    speaker.cacheMegabytes = ini.Integer(L"cache", L"megabytes", speaker.cacheMegabytes, 0, INT_MAX);
    const auto level = ini.String(L"log", L"level", L"info");
    if (_wcsicmp(level.c_str(), L"error") == 0) {
        settings.level = LogLevel::Error;
    } else if (_wcsicmp(level.c_str(), L"debug") == 0) {
        settings.level = LogLevel::Debug;
    }
    settings.logText = ini.Boolean(L"log", L"text", settings.logText);

    settings.nvdaNames = ProcessNames(ini.String(L"nvda", L"processNames", DefaultNvdaNames));
    if (settings.nvdaNames.empty()) settings.nvdaNames = ProcessNames(DefaultNvdaNames);
    return settings;
}
}
