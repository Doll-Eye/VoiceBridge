#include "test.h"
#include "controller_rpc.h"
#include "controller.h"

#ifdef _WIN64
constexpr char Result[] = "client64.txt";
constexpr wchar_t Phrase[] = L"WIDL x64";
#else
constexpr char Result[] = "client32.txt";
constexpr wchar_t Phrase[] = L"WIDL x86";
#endif

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return Test(Result, [](FILE* file) {
        Require(vb::BindController(vb::EndpointName(), &controllerBinding) == RPC_S_OK, "bind controller");
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
#endif
        RpcBindingFree(&controllerBinding);
        fprintf(file, "%u-bit WIDL client called all five methods\n", static_cast<unsigned>(sizeof(void*) * 8));
    });
}
