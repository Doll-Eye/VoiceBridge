#include "controller_rpc.h"

namespace vb {

std::wstring DesktopName() {
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
    return name;
}

std::wstring EndpointName() {
    DWORD session;
    auto name = DesktopName();
    if (name.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    return L"NvdaCtlr." + std::to_wstring(session) + L"." + name;
}

RPC_STATUS BindController(const std::wstring& endpoint, handle_t* binding) {
    if (endpoint.empty()) return RPC_S_INVALID_ENDPOINT_FORMAT;
    RPC_WSTR spec = nullptr;
    auto status = RpcStringBindingComposeW(
        nullptr, reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")), nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(endpoint.c_str())), nullptr, &spec);
    if (status != RPC_S_OK) return status;
    status = RpcBindingFromStringBindingW(spec, binding);
    RpcStringFreeW(&spec);
    return status;
}

}
