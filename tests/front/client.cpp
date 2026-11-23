#include "test.h"
#include "controller_rpc.h"
#include "controller.h"

#include <array>
#include <chrono>
#include <future>
#include <thread>

#ifdef _WIN64
constexpr char Result[] = "client64.txt";
constexpr wchar_t Phrase[] = L"WIDL x64";
#else
constexpr char Result[] = "client32.txt";
constexpr wchar_t Phrase[] = L"WIDL x86";
#endif

void ConcurrentCalls() {
    constexpr unsigned CallerCount = 8;
    constexpr unsigned CallsPerCaller = 8;
    const auto start = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    std::array<std::future<bool>, CallerCount> callers;
    for (unsigned caller = 0; caller < CallerCount; ++caller) {
        callers[caller] = std::async(std::launch::async, [start, caller] {
            std::this_thread::sleep_until(start);
            for (unsigned call = 0; call < CallsPerCaller; ++call) {
                const auto text = L"WIDL concurrent " + std::to_wstring(caller) +
                                  L"." + std::to_wstring(call);
                if (client_speakText(text.c_str()) != RPC_S_OK) return false;
            }
            return true;
        });
    }
    for (auto& caller : callers) Require(caller.get(), "concurrent speakText");
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    return Test(Result, [args](FILE* file) {
        Require(vb::BindController(vb::EndpointName(), &controllerBinding) == RPC_S_OK, "bind controller");
        if (std::wstring(args) == L"--concurrent") {
            ConcurrentCalls();
            RpcBindingFree(&controllerBinding);
            fputs("Eight WIDL callers each sent eight phrases\n", file);
            return;
        }
        controller3Binding = controllerBinding;
        Require(client_testIfRunning() == RPC_S_OK, "testIfRunning");
        Require(client_speakText(Phrase) == RPC_S_OK, "speakText");
        Require(client_cancelSpeech() == RPC_S_OK, "cancelSpeech");
        Require(client_brailleMessage(L"Braille is ignored") == RPC_S_OK, "brailleMessage");
        boolean speaking = TRUE;
        Require(client_isSpeaking(&speaking) == RPC_S_OK && !speaking, "isSpeaking");
#ifdef _WIN64
        auto longText = std::wstring(100001, L'y');
        Require(client_speakText(longText.c_str()) == RPC_S_OK, "long speech");
        const auto splitPair = std::wstring(99999, L's') + L"\U0001F600";
        Require(client_speakText(splitPair.c_str()) == RPC_S_OK, "surrogate pair crossing the limit");
        const auto fittingPair = std::wstring(99998, L'f') + L"\U0001F600!";
        Require(client_speakText(fittingPair.c_str()) == RPC_S_OK, "surrogate pair ending at the limit");
#endif
        RpcBindingFree(&controllerBinding);
        fprintf(file, "%u-bit WIDL client called all five methods\n", static_cast<unsigned>(sizeof(void*) * 8));
    });
}
