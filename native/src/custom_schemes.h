#ifndef JUSTCEF_CUSTOM_SCHEMES_H_
#define JUSTCEF_CUSTOM_SCHEMES_H_
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_scheme.h"
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// Lowercase schemes, with a fixed conservative capability set. No CSP bypass.
inline std::vector<std::string> CustomSchemes()
{
    std::vector<std::string> result;
    auto command = CefCommandLine::GetGlobalCommandLine();
    if (!command)
        return result;
    std::istringstream input(command->GetSwitchValue("custom-schemes").ToString());
    std::string name;
    while (std::getline(input, name, ','))
    {
        const bool valid = !name.empty() && name[0] >= 'a' && name[0] <= 'z' &&
                           std::all_of(name.begin(), name.end(),
                                       [](char c)
                                       {
                                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                                       });
        if (!valid || name == "ftp" || name == "ws" || name == "wss" || name == "blob" || name == "filesystem" || name == "chrome-extension" || name == "chrome-untrusted" ||
            name == "view-source" || name == "http" || name == "https" || name == "file" || name == "about" || name == "data" || name == "javascript" || name == "chrome" ||
            name == "devtools" || name == "justcef")
            continue;
        if (std::find(result.begin(), result.end(), name) == result.end())
            result.push_back(name);
    }
    return result;
}
inline bool IsCustomSchemeURL(const std::string& url)
{
    const auto end = url.find(':');
    if (end == std::string::npos)
        return false;
    auto scheme = url.substr(0, end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c)
                   {
                       return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
                   });
    const auto schemes = CustomSchemes();
    return std::find(schemes.begin(), schemes.end(), scheme) != schemes.end();
}
inline void RegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
    for (const auto& scheme : CustomSchemes())
        registrar->AddCustomScheme(scheme, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}
#endif
