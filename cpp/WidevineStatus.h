#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace justcef
{

enum class WidevineState : std::int32_t
{
    Ready = 0,
    RestartRequired = 1,
    Unavailable = 2
};

enum class WidevineUnavailableReason : std::int32_t
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
    std::optional<std::string> detail;
    std::optional<std::string> version;
};

} // namespace justcef
