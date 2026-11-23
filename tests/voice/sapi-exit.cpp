#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
#include <psapi.h>
#include <sapi.h>
#include <tlhelp32.h>
#include <werapi.h>
#include <winternl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace {

constexpr wchar_t LocalJenny[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\"
    L"TokenEnums\\NaturalVoiceEnumerator\\Local-MicrosoftWindows.Voice.en-US.Jenny.1";
// MinGW declares SPDFID_WaveFormatEx but its SAPI import library does not define it.
constexpr GUID WaveFormatId = {0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};

class ComApartment {
public:
    ComApartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    const HRESULT result;
};

template<class T>
class Interface {
public:
    Interface() = default;
    ~Interface() { if (pointer_) pointer_->Release(); }
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    T** out() {
        if (pointer_) pointer_->Release();
        pointer_ = nullptr;
        return &pointer_;
    }
    void** voidOut() { return reinterpret_cast<void**>(out()); }
private:
    T* pointer_ = nullptr;
};

void Mark(std::FILE* report, const char* line) {
    std::fputs(line, report);
    std::fputc('\n', report);
    std::fflush(report);
}

bool Check(std::FILE* report, HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return true;
    std::fprintf(report, "FAIL %s (0x%08lX).\n", operation, static_cast<unsigned long>(result));
    std::fflush(report);
    return false;
}

bool IsLocalJenny(ISpObjectToken& token) {
    wchar_t* value = nullptr;
    const auto hr = token.GetId(&value);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(value, CoTaskMemFree);
    return SUCCEEDED(hr) && id && _wcsicmp(id.get(), LocalJenny) == 0;
}

HRESULT FindLocalJenny(Interface<ISpObjectToken>& token) {
    Interface<ISpObjectTokenCategory> category;
    Interface<IEnumSpObjectTokens> tokens;
    auto hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_ISpObjectTokenCategory, category.voidOut());
    if (SUCCEEDED(hr)) hr = category->SetId(SPCAT_VOICES, FALSE);
    if (SUCCEEDED(hr)) hr = category->EnumTokens(nullptr, nullptr, tokens.out());
    if (FAILED(hr)) return hr;

    while ((hr = tokens->Next(1, token.out(), nullptr)) == S_OK) {
        if (!token.get()) return E_UNEXPECTED;
        if (IsLocalJenny(*token.get())) return S_OK;
    }
    return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

struct Voice {
    Interface<ISpObjectToken> token;
    Interface<IStream> memory;
    Interface<ISpStream> stream;
    Interface<ISpVoice> voice;
};

int Probe(std::FILE* report, Voice& held, bool async = false) {
    auto& [token, memory, stream, voice] = held;
    Interface<ISpObjectToken> selected;

    if (!Check(report, FindLocalJenny(token), "Find the installed local Natural Jenny token")) return 1;
    if (!Check(report, CreateStreamOnHGlobal(nullptr, TRUE, memory.out()), "Create the memory stream")) return 1;
    if (!Check(report, CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ISpStream, stream.voidOut()), "Create SpStream")) return 1;
    WAVEFORMATEX pcm{WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};
    if (!Check(report, stream->SetBaseStream(memory.get(), WaveFormatId, &pcm), "Attach the memory stream")) return 1;
    if (!Check(report, CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ISpVoice, voice.voidOut()), "Create SpVoice")) return 1;
    if (!Check(report, voice->SetOutput(stream.get(), FALSE), "Direct all synthesis to memory")) return 1;
    if (!Check(report, voice->SetVoice(token.get()), "Select local Natural Jenny")) return 1;
    if (async && !Check(report, voice->SetRate(4), "Set the comparison voice to SAPI rate 4")) return 1;
    if (!Check(report, voice->GetVoice(selected.out()), "Read back the selected voice")) return 1;
    if (!selected.get() || !IsLocalJenny(*selected.get())) {
        Mark(report, "FAIL SAPI did not return the exact local Natural Jenny token.");
        return 1;
    }
    std::fprintf(report, "Verified selected token: %ls\n", LocalJenny);
    Mark(report, async ? "Starting asynchronous synthesis into memory." : "Starting synchronous synthesis into memory.");

    const DWORD flags = SPF_IS_NOT_XML | (async ? SPF_ASYNC : 0);
    if (!Check(report, voice->Speak(L"The inventory is open.", flags, nullptr), "Synthesize the phrase")) return 1;
    if (async && voice->WaitUntilDone(10000) != S_OK) {
        Mark(report, "FAIL Asynchronous speech did not complete within ten seconds.");
        return 1;
    }
    STATSTG status{};
    if (!Check(report, memory->Stat(&status, STATFLAG_NONAME), "Count PCM bytes in memory")) return 1;
    if (!status.cbSize.QuadPart || status.cbSize.QuadPart % pcm.nBlockAlign) {
        Mark(report, "FAIL The memory stream did not contain complete PCM frames.");
        return 1;
    }
    std::fprintf(report, "PASS %s speech produced %llu bytes of 24 kHz, 16-bit mono PCM.\n",
                 async ? "Asynchronous" : "Synchronous",
                 static_cast<unsigned long long>(status.cbSize.QuadPart));
    return 0;
}

void Resources(std::FILE* report, const char* label) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    std::fprintf(report, "%s:\n", label);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        std::fprintf(report, "  Private memory %.2f MiB.\n", memory.PrivateUsage / 1048576.0);
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) std::fprintf(report, "  Handles %lu.\n", handles);

    // The default heap has process lifetime. Other heaps may disappear before
    // they can be safely queried, and private memory also includes non-heap allocations.
    using Summary = decltype(&HeapSummary);
    static const auto summarize = reinterpret_cast<Summary>(reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary")));
    HEAP_SUMMARY heap{};
    heap.cb = sizeof(heap);
    if (summarize && summarize(GetProcessHeap(), 0, &heap)) {
        std::fprintf(report, "  Default heap %.2f MiB allocated, %.2f MiB committed, %lu process heaps.\n",
                     heap.cbAllocated / 1048576.0, heap.cbCommitted / 1048576.0, GetProcessHeaps(0, nullptr));
    } else {
        Mark(report, "  Default heap summary unavailable.");
    }
    std::fflush(report);
}

ULONGLONG Ticks(const FILETIME& time) {
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct Cpu {
    ULONGLONG kernel = 0;
    ULONGLONG user = 0;
};

bool ReadCpu(Cpu& cpu) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    cpu = {Ticks(kernel), Ticks(user)};
    return true;
}

using EntryPoint = std::pair<std::wstring, std::uintptr_t>;

EntryPoint ThreadEntry(HANDLE thread) {
    using Query = decltype(&NtQueryInformationThread);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    void* address = nullptr;
    HMODULE module = nullptr;
    if (!query || query(thread, ThreadQuerySetWin32StartAddress, &address, sizeof(address), nullptr) < 0 || !address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module)) return {L"unknown", 0};
    wchar_t path[MAX_PATH];
    const auto length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {L"unknown", 0};
    std::wstring name(path, length);
    name.erase(0, name.find_last_of(L"\\/") + 1);
    return {std::move(name), reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)};
}

struct Thread {
    ULONGLONG created = 0;
    Cpu cpu;
    ULONGLONG cycles = 0;
    bool haveCycles = false;
    EntryPoint entry;
};

std::map<DWORD, Thread> Threads() {
    std::map<DWORD, Thread> threads;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto owner = GetCurrentProcessId();
    for (BOOL found = Thread32First(snapshot, &entry); found; found = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != owner || entry.th32ThreadID == GetCurrentThreadId()) continue;
        const auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessIdOfThread(thread) == owner && GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            ULONGLONG cycles = 0;
            const bool haveCycles = QueryThreadCycleTime(thread, &cycles);
            threads.emplace(entry.th32ThreadID, Thread{Ticks(created), {Ticks(kernel), Ticks(user)},
                                                       cycles, haveCycles, ThreadEntry(thread)});
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return threads;
}

bool MeasureIdle(std::FILE* report) {
    Resources(report, "Idle start with two warmed local Jenny voices");
    const auto before = Threads();
    Cpu start, end;
    const bool haveStart = ReadCpu(start);
    const auto began = GetTickCount64();
    Sleep(60000);
    const auto elapsed = GetTickCount64() - began;
    const bool haveEnd = ReadCpu(end);
    const auto after = Threads();

    if (haveStart && haveEnd) {
        const double kernel = (end.kernel - start.kernel) / 10000.0;
        const double user = (end.user - start.user) / 10000.0;
        std::fprintf(report, "Idle process CPU %.2f ms in %llu ms (kernel %.2f ms, user %.2f ms).\n",
                     kernel + user, static_cast<unsigned long long>(elapsed), kernel, user);
    } else {
        Mark(report, "FAIL Idle process timing unavailable.");
    }
    std::size_t matched = 0;
    for (const auto& [id, first] : before) {
        const auto found = after.find(id);
        if (found == after.end() || found->second.created != first.created) continue;
        ++matched;
        const auto& last = found->second;
        const auto kernel = last.cpu.kernel - first.cpu.kernel;
        const auto user = last.cpu.user - first.cpu.user;
        const bool haveCycles = first.haveCycles && last.haveCycles;
        const auto cycles = haveCycles ? last.cycles - first.cycles : 0;
        if (!kernel && !user && haveCycles && !cycles) continue;
        std::fprintf(report, "Idle thread %lu %ls+0x%llX: kernel %.2f ms, user %.2f ms, cycles ",
                     id, first.entry.first.c_str(), static_cast<unsigned long long>(first.entry.second),
                     kernel / 10000.0, user / 10000.0);
        if (haveCycles) std::fprintf(report, "%llu.\n", static_cast<unsigned long long>(cycles));
        else Mark(report, "unavailable.");
    }
    std::fprintf(report, "Matched %zu worker threads at both endpoints, %zu start only and %zu end only.\n",
                 matched, before.size() - matched, after.size() - matched);
    return haveStart && haveEnd;
}

int IdleProbe(std::FILE* report) {
    Resources(report, "Baseline after COM initialization");
    for (int cycle = 0; cycle < 3; ++cycle) {
        Voice temporary;
        if (Probe(report, temporary, true)) return 1;
    }
    Resources(report, "After warming and releasing three temporary voices");
    bool measured = false;
    {
        Voice first, second;
        if (Probe(report, first, true) || Probe(report, second, true)) return 1;
        measured = MeasureIdle(report);
        Resources(report, "Idle end while voices remain alive");
    }
    Resources(report, "After releasing the two held voices");
    return measured ? 0 : 1;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR args, int) {
    constexpr UINT errorMode = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    const auto length = std::strcspn(args, " \t\r\n");
    if (length == 9 && std::strncmp(args, "idle-cost", length) == 0) {
        SetErrorMode(errorMode | SEM_NOGPFAULTERRORBOX);
        std::FILE* report = std::fopen("sapi-idle-results.txt", "w");
        if (!report) return 2;
        ComApartment apartment;
        const auto result = Check(report, apartment.result, "Initialize COM") ? IdleProbe(report) : 1;
        std::fclose(report);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
        return result;
    }
    if (length != 0) return 2;
    SetErrorMode(errorMode);
    if (FAILED(WerSetFlags(WER_FAULT_REPORTING_NO_UI))) SetErrorMode(errorMode | SEM_NOGPFAULTERRORBOX);

    const std::unique_ptr<std::FILE, decltype(&std::fclose)> report(
        std::fopen("sapi-exit-results.txt", "w"), std::fclose);
    if (!report) return 2;
    Mark(report.get(), "Starting the standalone SAPI normal-exit probe.");
    int result;
    {
        ComApartment apartment;
        if (!Check(report.get(), apartment.result, "Initialize COM")) return 1;
        {
            Voice voice;
            result = Probe(report.get(), voice);
            Mark(report.get(), "Releasing the probe's COM references on the calling thread.");
        }
        Mark(report.get(), "All probe-owned COM references released. Entering CoUninitialize.");
    }
    Mark(report.get(), "CoUninitialize returned. Returning normally from WinMain.");
    return std::ferror(report.get()) ? 2 : result;
}
