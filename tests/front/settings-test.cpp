#include "test.h"
#include "../../src/front/settings.h"

#include <fstream>

namespace {
namespace fs = std::filesystem;

struct TestDirectory {
    fs::path path = fs::current_path() / (L"Settings parser fixtures " + std::to_wstring(GetCurrentProcessId()));

    TestDirectory() { Require(fs::create_directory(path), "create settings fixture directory"); }

    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

vb::Settings ReadFixture(const fs::path& directory, const char* contents) {
    Require(fs::create_directory(directory), "create individual INI fixture directory");
    if (contents) {
        std::ofstream file(directory / L"VoiceBridge.ini");
        file << contents;
        file.close();
        Require(file.good(), "write test INI");
    }
    return vb::ReadSettings(directory);
}

void CheckDefaults(const vb::Settings& settings) {
    const vb::SpeakerConfig defaults;
    const auto& speaker = settings.speaker;
    Require(speaker.voice == defaults.voice, "default voice name");
    Require(speaker.fallbackVoice == defaults.fallbackVoice, "default fallback voice");
    Require(speaker.rate == defaults.rate, "default rate");
    Require(speaker.volume == defaults.volume, "default volume");
    Require(speaker.followNarrator == defaults.followNarrator, "default followNarrator");
    Require(speaker.keepAwakeSeconds == defaults.keepAwakeSeconds, "default keepAwakeSeconds");
    Require(speaker.cacheMegabytes == defaults.cacheMegabytes, "default cache megabytes");
    Require(settings.level == vb::LogLevel::Info, "default log level");
    Require(!settings.logText, "text logging off by default");
    const std::vector<std::wstring> defaultNames{L"nvda.exe", L"nvda_uiAccess.exe", L"nvda_noUIAccess.exe"};
    Require(settings.nvdaNames == defaultNames, "default NVDA process names");
    Require(settings.signInWaitSeconds == 120, "default NVDA sign-in wait");
    Require(settings.startsAtSignIn == vb::SignInStart::Auto, "default NVDA sign-in detection");
}

void CheckValid(const vb::Settings& settings) {
    const auto& speaker = settings.speaker;
    Require(speaker.voice == L"Microsoft Jenny Natural", "voice name with spaces");
    Require(speaker.fallbackVoice == L"Backup Voice", "quoted fallback voice name");
    Require(speaker.rate == -10, "signed rate");
    Require(speaker.volume == 0, "zero volume");
    Require(!speaker.followNarrator, "explicit followNarrator opt-out");
    Require(speaker.keepAwakeSeconds == 9, "custom keepAwakeSeconds");
    Require(speaker.cacheMegabytes == 0, "zero disables cache");
    Require(settings.level == vb::LogLevel::Debug, "case-insensitive debug level");
    Require(settings.logText, "explicit text logging opt-in");
    const std::vector<std::wstring> names{L"test NVDA.exe", L"second.exe"};
    Require(settings.nvdaNames == names, "trimmed process names with spaces and an invalid token");
    Require(settings.signInWaitSeconds == 600, "maximum NVDA sign-in wait");
    Require(settings.startsAtSignIn == vb::SignInStart::Yes, "case-insensitive NVDA sign-in override yes");
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return Test("settings-test.txt", [](FILE* result) {
        const TestDirectory directory;
        CheckDefaults(ReadFixture(directory.path / L"Missing file", nullptr));
        CheckDefaults(ReadFixture(directory.path / L"Malformed values", R"ini(
[voice]
name=
rate=-2suffix
volume=101
followNarrator=off
[audio]
keepAwakeSeconds=-1
[cache]
megabytes=2147483648
[log]
level=unknown
text=01
[nvda]
processNames=C:\invalid\nvda.exe;VoiceBridge.EXE
signInWaitSeconds=601
startsAtSignIn=true
)ini"));
        CheckValid(ReadFixture(directory.path / L"Valid values", R"ini(
[voice]
name=Microsoft Jenny Natural
fallback="Backup Voice"
rate=-10
volume=0
followNarrator=0
[audio]
keepAwakeSeconds=9
[cache]
megabytes=0
[log]
level=DEBUG
text=1
[nvda]
processNames= test NVDA.exe ; bad*.exe ; second.exe
signInWaitSeconds=600
startsAtSignIn=yEs
)ini"));
        const auto disabled = ReadFixture(directory.path / L"Disabled sign-in wait",
            "[nvda]\nsignInWaitSeconds=0\nstartsAtSignIn=nO\n");
        Require(disabled.signInWaitSeconds == 0, "zero disables the NVDA sign-in wait");
        Require(disabled.startsAtSignIn == vb::SignInStart::No, "case-insensitive NVDA sign-in override no");
        CheckDefaults(ReadFixture(directory.path / L"Negative sign-in wait",
            "[nvda]\nsignInWaitSeconds=-1\nstartsAtSignIn=aUtO\n"));
        CheckDefaults(ReadFixture(directory.path / L"Malformed sign-in wait",
            "[nvda]\nsignInWaitSeconds=3seconds\nstartsAtSignIn=\n"));
        Require(vb::NvdaInAccessibilityConfiguration(L"nvda_nvda_v1"), "single NVDA accessibility entry");
        Require(vb::NvdaInAccessibilityConfiguration(L"narrator, \tNvDa_NVDA_v1 \t,,magnifier"),
            "NVDA accessibility entry among other entries, with mixed case and spaces");
        Require(vb::NvdaInAccessibilityConfiguration(std::wstring(32768, L'x') + L",nvda_nvda_v1"),
            "NVDA accessibility entry after a long list prefix");
        Require(!vb::NvdaInAccessibilityConfiguration(L""), "empty accessibility configuration");
        Require(!vb::NvdaInAccessibilityConfiguration(L"narrator,xnvda_nvda_v1,nvda_nvda_v10,nvda_nvda_v1;magnifier"),
            "NVDA accessibility entry must be a complete comma-separated token");
        fputs("Missing and malformed INI values use the defaults. Valid INI values and explicit text opt-in are read from a path with spaces.\n", result);
        fputs("The NVDA sign-in wait accepts 0 to 600 seconds and case-insensitive overrides. Invalid values use the defaults.\n", result);
        fputs("Accessibility configuration matching accepts full NVDA entries without touching the registry.\n", result);
    });
}
