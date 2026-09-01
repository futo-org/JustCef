#ifndef CEF_DOTCEF_WIDEVINE_UTIL_H_
#define CEF_DOTCEF_WIDEVINE_UTIL_H_

#include <cstdint>
#include <string>

#include "include/cef_command_line.h"

namespace shared
{

extern const char kWidevineComponentId[];

#if defined(OS_LINUX)
extern const char kWidevineCdmPathSwitch[];
#endif

struct WidevineStatus
{
    bool registered = false;
    bool installed = false;
    bool requiresRestart = false;
    int32_t state = 0;
    std::string version;
};

void InitializeWidevineState(const CefRefPtr<CefCommandLine>& command_line, const std::string& root_cache_path);
void RequestWidevineCdmUpdate();
WidevineStatus GetWidevineStatus();

} // namespace shared

#endif // CEF_DOTCEF_WIDEVINE_UTIL_H_
