#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace justcef
{

enum class Status : std::uint8_t
{
    Ok = 0,
    Error = 1,
    Canceled = 2,
    NotFound = 3,
    NotHandled = 4,
    Unsupported = 5,
    ShuttingDown = 6,
    TooLarge = 7,
    InvalidRequest = 8,
};

inline const char* StatusName(Status status)
{
    switch (status)
    {
    case Status::Ok:
        return "Ok";
    case Status::Error:
        return "Error";
    case Status::Canceled:
        return "Canceled";
    case Status::NotFound:
        return "NotFound";
    case Status::NotHandled:
        return "NotHandled";
    case Status::Unsupported:
        return "Unsupported";
    case Status::ShuttingDown:
        return "ShuttingDown";
    case Status::TooLarge:
        return "TooLarge";
    case Status::InvalidRequest:
        return "InvalidRequest";
    }
    return "Unknown";
}

class RemoteError : public std::runtime_error
{
public:
    RemoteError(Status status, const std::optional<std::string>& message)
        : std::runtime_error(message && !message->empty() ? *message : std::string("Remote call failed with status ") + StatusName(status) + "."), status_(status)
    {
    }

    Status GetStatus() const { return status_; }

private:
    Status status_;
};

} // namespace justcef
