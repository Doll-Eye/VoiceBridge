#pragma once

#include <windows.h>
#include <rpc.h>

#include <string>

#include "../common/speaker.h"

namespace vb {
std::wstring DesktopName();
std::wstring EndpointName();
RPC_STATUS BindController(const std::wstring& endpoint, handle_t* binding);
RPC_STATUS RegisterController(const std::wstring& endpoint);
void SetControllerSpeaker(ISpeaker* speaker);
bool ControllerAnswers(const std::wstring& endpoint);
}
