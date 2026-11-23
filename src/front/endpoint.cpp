#include "controller_rpc.h"

namespace vb {

std::wstring EndpointName() {
    DWORD session;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        return {};
    }
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    DWORD bytes = 0;
    GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &bytes);
    if (!bytes) {
        return {};
    }
    std::wstring name(bytes / sizeof(wchar_t), L'\0');
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name.data(), bytes, nullptr)) {
        return {};
    }
    name.resize(wcslen(name.c_str()));
    return L"NvdaCtlr." + std::to_wstring(session) + L"." + name;
}

RPC_STATUS BindController(const std::wstring& endpoint, handle_t* binding) {
    auto spec = L"ncalrpc:[" + endpoint + L"]";
    return RpcBindingFromStringBindingW(reinterpret_cast<RPC_WSTR>(spec.data()), binding);
}

}
