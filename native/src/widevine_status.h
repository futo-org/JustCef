#pragma once

#include <cstdint>
#include <string>

namespace shared
{

enum class WidevineState : int32_t
{
    Ready = 0,
    RestartRequired = 1,
    Unavailable = 2
};

enum class WidevineUnavailableReason : int32_t
{
    None = 0,
    NotSupported = 1,
    NoCachePath = 2,
    UpdaterUnavailable = 3,
    UpdateFailed = 4,
    NoUsableCdm = 5
};

struct WidevineStatus
{
    WidevineState state = WidevineState::Unavailable;
    WidevineUnavailableReason reason = WidevineUnavailableReason::None;
    std::string detail;
    std::string version;
};

inline WidevineStatus ResolveWidevineStatus(bool installed, bool requiresRestart, const std::string& version, WidevineUnavailableReason reason = WidevineUnavailableReason::None, const std::string& detail = {})
{
    if (installed)
        return {requiresRestart ? WidevineState::RestartRequired : WidevineState::Ready, WidevineUnavailableReason::None, {}, version};

    return {WidevineState::Unavailable, reason == WidevineUnavailableReason::None ? WidevineUnavailableReason::NoUsableCdm : reason, detail, version};
}

} // namespace shared
