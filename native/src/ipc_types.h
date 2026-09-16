#ifndef JUSTCEF_IPC_TYPES_H_
#define JUSTCEF_IPC_TYPES_H_

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ipc
{

#ifdef _WIN32
using NativeHandle = HANDLE;
#else
using NativeHandle = int;
#endif

constexpr uint32_t kProtocolVersion = 2;
constexpr size_t kHeaderSize = 10;
constexpr size_t kMaxBodySize = 256u * 1024u * 1024u;
constexpr uint32_t kStreamInitialCredit = 1024u * 1024u;
constexpr size_t kStreamMaxChunk = 256u * 1024u;
constexpr uint32_t kStreamCreditBatch = 64u * 1024u;
constexpr size_t kStreamMaxBuffered = 4u * 1024u * 1024u;

enum class PacketType : uint8_t
{
    Request = 0,
    Response = 1,
    Notification = 2,
    Cancel = 3
};

enum class StatusCode : uint8_t
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
    Timeout = 100,
    Disconnected = 101
};

struct Packet
{
    PacketType type = PacketType::Notification;
    uint8_t opcode = 0;
    uint32_t requestId = 0;
    std::vector<uint8_t> body;
};

inline std::vector<uint8_t> EncodePacket(PacketType type, uint8_t opcode, uint32_t requestId, const uint8_t* body, size_t size)
{
    std::vector<uint8_t> packet(kHeaderSize + size);
    const uint32_t length = static_cast<uint32_t>(packet.size() - sizeof(uint32_t));
    std::memcpy(packet.data(), &length, sizeof(uint32_t));
    std::memcpy(packet.data() + 4, &requestId, sizeof(uint32_t));
    packet[8] = static_cast<uint8_t>(type);
    packet[9] = opcode;
    if (size > 0)
        std::memcpy(packet.data() + kHeaderSize, body, size);
    return packet;
}

inline const char* StatusName(StatusCode status)
{
    switch (status)
    {
    case StatusCode::Ok:
        return "Ok";
    case StatusCode::Error:
        return "Error";
    case StatusCode::Canceled:
        return "Canceled";
    case StatusCode::NotFound:
        return "NotFound";
    case StatusCode::NotHandled:
        return "NotHandled";
    case StatusCode::Unsupported:
        return "Unsupported";
    case StatusCode::ShuttingDown:
        return "ShuttingDown";
    case StatusCode::TooLarge:
        return "TooLarge";
    case StatusCode::InvalidRequest:
        return "InvalidRequest";
    case StatusCode::Timeout:
        return "Timeout";
    case StatusCode::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

} // namespace ipc

#endif // JUSTCEF_IPC_TYPES_H_
