#ifndef JUSTCEF_VIEW_COMMON_H_
#define JUSTCEF_VIEW_COMMON_H_

#include "custom_schemes.h"
#include "include/cef_values.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>

constexpr char kViewMessagePrefix[] = "JustCef.View.";
constexpr char kViewCreateMessageName[] = "JustCef.View.Create";
constexpr char kViewNavigateMessageName[] = "JustCef.View.Navigate";
constexpr char kViewCommandMessageName[] = "JustCef.View.Command";
constexpr char kViewDestroyMessageName[] = "JustCef.View.Destroy";
constexpr char kViewContextReleasedMessageName[] = "JustCef.View.ContextReleased";
constexpr char kViewEventMessageName[] = "JustCef.View.Event";

constexpr int kViewMaxPerDocument = 8;

inline bool IsAllowedViewSource(const std::string& src)
{
    auto startsWith = [&](const char* prefix)
    {
        const size_t length = std::strlen(prefix);
        if (src.size() < length)
            return false;
        for (size_t i = 0; i < length; ++i)
        {
            char c = src[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i])
                return false;
        }
        return true;
    };

    return IsCustomSchemeURL(src) || startsWith("https://") || startsWith("http://") || (src.size() == 11 && startsWith("about:blank"));
}

#endif // JUSTCEF_VIEW_COMMON_H_
