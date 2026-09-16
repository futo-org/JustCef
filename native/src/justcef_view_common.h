#ifndef JUSTCEF_VIEW_COMMON_H_
#define JUSTCEF_VIEW_COMMON_H_

#include "include/cef_values.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>

constexpr char kViewMessagePrefix[] = "JustCef.View.";
constexpr char kViewCreateMessageName[] = "JustCef.View.Create";
constexpr char kViewUpdateMessageName[] = "JustCef.View.Update";
constexpr char kViewNavigateMessageName[] = "JustCef.View.Navigate";
constexpr char kViewCommandMessageName[] = "JustCef.View.Command";
constexpr char kViewDestroyMessageName[] = "JustCef.View.Destroy";
constexpr char kViewContextReleasedMessageName[] = "JustCef.View.ContextReleased";
constexpr char kViewEventMessageName[] = "JustCef.View.Event";
constexpr char kViewSnapshotMessageName[] = "JustCef.View.Snapshot";
constexpr char kViewTakeFocusMessageName[] = "JustCef.View.TakeFocus";
constexpr char kViewPingMessageName[] = "JustCef.View.Ping";
constexpr char kViewPongMessageName[] = "JustCef.View.Pong";
constexpr char kViewWheelMessageName[] = "JustCef.View.Wheel";
constexpr char kViewKeyScrollMessageName[] = "JustCef.View.KeyScroll";
constexpr char kViewHostActivityMessageName[] = "JustCef.View.HostActivity";
constexpr char kViewLatchMessageName[] = "JustCef.View.Latch";

enum class ViewScrollPhase : int
{
    Wheel = 0,
    PreciseWheel = 1,
    TouchMove = 2,
    TouchEnd = 3,
    Cancel = 4
};
constexpr char kViewContentExtraInfoKey[] = "justcefViewContent";

constexpr int kViewMaxPerDocument = 8;

enum class ViewAnchor : int
{
    Start = 0,
    End = 1,
    Stretch = 2,
    Center = 3
};

struct ViewUpdate
{
    int32_t seq = 0;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double clipX = 0;
    double clipY = 0;
    double clipWidth = 0;
    double clipHeight = 0;
    double innerWidth = 0;
    double innerHeight = 0;
    double devicePixelRatio = 1;
    bool visible = false;
    int32_t anchorH = static_cast<int32_t>(ViewAnchor::Start);
    int32_t anchorV = static_cast<int32_t>(ViewAnchor::Start);
    int32_t order = 0;
    bool frozen = false;
};

constexpr size_t kViewUpdateFieldCount = 17;

inline void WriteViewUpdate(CefRefPtr<CefListValue> args, size_t index, const ViewUpdate& update)
{
    args->SetInt(index + 0, update.seq);
    args->SetDouble(index + 1, update.x);
    args->SetDouble(index + 2, update.y);
    args->SetDouble(index + 3, update.width);
    args->SetDouble(index + 4, update.height);
    args->SetDouble(index + 5, update.clipX);
    args->SetDouble(index + 6, update.clipY);
    args->SetDouble(index + 7, update.clipWidth);
    args->SetDouble(index + 8, update.clipHeight);
    args->SetDouble(index + 9, update.innerWidth);
    args->SetDouble(index + 10, update.innerHeight);
    args->SetDouble(index + 11, update.devicePixelRatio);
    args->SetBool(index + 12, update.visible);
    args->SetInt(index + 13, update.anchorH);
    args->SetInt(index + 14, update.anchorV);
    args->SetInt(index + 15, update.order);
    args->SetBool(index + 16, update.frozen);
}

constexpr double kViewMaxCoordinate = 1000000.0;

inline double ReadViewNumber(CefRefPtr<CefListValue> args, size_t index)
{
    double value = 0;
    switch (args->GetType(index))
    {
    case VTYPE_INT:
        value = args->GetInt(index);
        break;
    case VTYPE_DOUBLE:
        value = args->GetDouble(index);
        break;
    default:
        return 0;
    }

    if (!std::isfinite(value))
        return 0;
    return std::clamp(value, -kViewMaxCoordinate, kViewMaxCoordinate);
}

inline bool ReadViewUpdate(CefRefPtr<CefListValue> args, size_t index, ViewUpdate& update)
{
    if (!args || args->GetSize() < index + kViewUpdateFieldCount)
        return false;

    if (args->GetType(index) != VTYPE_INT)
        return false;
    update.seq = args->GetInt(index);
    update.x = ReadViewNumber(args, index + 1);
    update.y = ReadViewNumber(args, index + 2);
    update.width = ReadViewNumber(args, index + 3);
    update.height = ReadViewNumber(args, index + 4);
    update.clipX = ReadViewNumber(args, index + 5);
    update.clipY = ReadViewNumber(args, index + 6);
    update.clipWidth = ReadViewNumber(args, index + 7);
    update.clipHeight = ReadViewNumber(args, index + 8);
    update.innerWidth = ReadViewNumber(args, index + 9);
    update.innerHeight = ReadViewNumber(args, index + 10);
    update.devicePixelRatio = ReadViewNumber(args, index + 11);
    update.visible = args->GetType(index + 12) == VTYPE_BOOL && args->GetBool(index + 12);
    update.anchorH = static_cast<int32_t>(ReadViewNumber(args, index + 13));
    update.anchorV = static_cast<int32_t>(ReadViewNumber(args, index + 14));
    update.order = static_cast<int32_t>(ReadViewNumber(args, index + 15));
    update.frozen = args->GetType(index + 16) == VTYPE_BOOL && args->GetBool(index + 16);
    return true;
}

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

    return startsWith("https://") || startsWith("http://");
}

#endif // JUSTCEF_VIEW_COMMON_H_
