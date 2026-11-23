#include "controller_rpc.h"
#include "controller.h"

#include <excpt.h>

extern "C" int RPC_ENTRY RpcExceptionFilter(unsigned long exceptionCode);

// WIDL does not emit NVDA's ACF status-to-return handling. The MinGW SEH
// scope keeps transport faults (including the normal missing endpoint) local.
extern "C" int ControllerRpcFilter(EXCEPTION_POINTERS* fault, void*) {
    return RpcExceptionFilter(fault->ExceptionRecord->ExceptionCode);
}

[[gnu::noinline]] static bool Ping() {
    volatile bool answered = false;
    __try1(ControllerRpcFilter)
    answered = client_testIfRunning() == RPC_S_OK;
    __except1
    return answered;
}

bool vb::ControllerAnswers(const std::wstring& endpoint) {
    if (BindController(endpoint, &controllerBinding) != RPC_S_OK) return false;
    const bool answered = Ping();
    RpcBindingFree(&controllerBinding);
    return answered;
}
