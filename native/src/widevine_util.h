#ifndef CEF_DOTCEF_WIDEVINE_UTIL_H_
#define CEF_DOTCEF_WIDEVINE_UTIL_H_

#include <cstdint>
#include <string>

#include "include/cef_command_line.h"
#include "widevine_status.h"

namespace shared
{

extern const char kWidevineComponentId[];

#if defined(OS_LINUX)
extern const char kWidevineCdmPathSwitch[];
#endif

void InitializeWidevineState(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path);
void RequestWidevineCdmUpdate();
void PublishWidevineStatus(WidevineUnavailableReason reason = WidevineUnavailableReason::NotApplicable, const std::string& detail = {});

} // namespace shared

#endif // CEF_DOTCEF_WIDEVINE_UTIL_H_
