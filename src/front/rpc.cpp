#include "controller_rpc.h"

#include <sddl.h>
#include "controller.h"

#include <atomic>

// MinGW exports this Windows API in librpcrt4, but omits its declaration.
extern "C" RPC_STATUS RPC_ENTRY RpcServerRegisterIf3(
    RPC_IF_HANDLE, UUID*, RPC_MGR_EPV*, unsigned int, unsigned int, unsigned int,
    RPC_IF_CALLBACK_FN*, void*);

namespace {
std::atomic<vb::ISpeaker*> activeSpeaker;
}

namespace vb {
void SetControllerSpeaker(ISpeaker* speaker) {
    activeSpeaker.store(speaker, std::memory_order_release);
}

RPC_STATUS RegisterController(const std::wstring& endpoint) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)(A;;GA;;;AC)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return GetLastError();
    }
    const RPC_STATUS bindStatus = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(endpoint.c_str())), descriptor);
    RPC_STATUS status = bindStatus;
    if (status == RPC_S_OK) {
        for (const auto iface : {NvdaController_v1_0_s_ifspec, NvdaController3_v1_0_s_ifspec}) {
            status = RpcServerRegisterIf3(
                iface, nullptr, nullptr, RPC_IF_AUTOLISTEN | RPC_IF_ALLOW_CALLBACKS_WITH_NO_AUTH,
                RPC_C_LISTEN_MAX_CALLS_DEFAULT, 0, nullptr, descriptor);
            if (status != RPC_S_OK) {
                break;
            }
        }
    }
    LocalFree(descriptor);
    return status;
}
}

error_status_t __stdcall testIfRunning() { return ERROR_SUCCESS; }

error_status_t __stdcall speakText(const wchar_t* text) {
    if (auto* speaker = activeSpeaker.load(std::memory_order_acquire); speaker && text && *text) {
        size_t length = wcsnlen(text, 100000);
        // Keep a surrogate pair together at the truncation boundary.
        if (length == 100000 && text[length - 1] >= 0xD800 && text[length - 1] <= 0xDBFF) {
            --length;
        }
        try {
            speaker->Speak(std::wstring(text, length));
        } catch (const std::bad_alloc&) {
            return ERROR_OUTOFMEMORY;
        }
    }
    return ERROR_SUCCESS;
}

error_status_t __stdcall cancelSpeech() {
    if (auto* speaker = activeSpeaker.load(std::memory_order_acquire)) {
        speaker->Cancel();
    }
    return ERROR_SUCCESS;
}

error_status_t __stdcall brailleMessage(const wchar_t*) { return ERROR_SUCCESS; }

error_status_t __stdcall isSpeaking(boolean* speaking) {
    auto* speaker = activeSpeaker.load(std::memory_order_acquire);
    *speaking = speaker && speaker->IsSpeaking();
    return ERROR_SUCCESS;
}
