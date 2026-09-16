#pragma once

#include "JustCefErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace justcef::detail
{

enum class PacketType : std::uint8_t
{
    Request = 0,
    Response = 1,
    Notification = 2,
    Cancel = 3,
};

enum class OpcodeController : uint8_t
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowCreate = 3,
    // WindowCreatePositioned = 4,
    WindowSetDevelopmentToolsEnabled = 5,
    WindowLoadUrl = 6,
    // WindowLoadHtml = 7,
    // WindowExecuteJavascript = 8, //string js
    WindowSetZoom = 9, // double zoom
    // WindowSetResizable = 10, //bool value
    // WindowSetWindowless = 11, //bool value
    // WindowGetWindowSize = 12,
    // WindowSetWindowSize = 13, //Size size
    WindowGetPosition = 14,
    WindowSetPosition = 15, // Position value
    // WindowCenterWindow = 16,
    WindowMaximize = 17,
    WindowMinimize = 18,
    WindowRestore = 19,
    WindowShow = 20,
    WindowHide = 21,
    WindowClose = 22,
    // WindowSetRequestModificationEnabled = 23, //bool enabled
    // WindowModifyRequest = 24, //Request request -> Response
    WindowRequestFocus = 25,
    // WindowRegisterKeyboardListener = 26,
    // WindowSetTitle = 27,
    WindowActivate = 28,
    WindowBringToTop = 29,
    WindowSetAlwaysOnTop = 30,
    WindowSetFullscreen = 31,
    WindowCenterSelf = 32,
    WindowSetProxyRequests = 33,
    WindowSetModifyRequests = 34,
    PickFile = 39,
    PickDirectory = 40,
    SaveFile = 41,
    WindowExecuteDevToolsMethod = 42,
    WindowSetDevelopmentToolsVisible = 43,
    WindowSetTitle = 44,
    WindowSetIcon = 45,
    WindowAddUrlToProxy = 46,
    WindowRemoveUrlToProxy = 47,
    WindowAddUrlToModify = 48,
    WindowRemoveUrlToModify = 49,
    WindowGetSize = 50,
    WindowSetSize = 51,
    WindowAddDevToolsEventMethod = 52,
    WindowRemoveDevToolsEventMethod = 53,
    WindowAddDomainToProxy = 54,
    WindowRemoveDomainToProxy = 55,
    WindowGetZoom = 56,
    WindowBridgeRpc = 57,
    GetWidevineStatus = 59
};

// Notifications from controller
enum class OpcodeControllerNotification : uint8_t
{
    Exit = 0,
    StreamData = 1,
    StreamEnd = 2,
    StreamError = 3,
};

// Requests from client
enum class OpcodeClient : uint8_t
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowProxyRequest = 3,
    WindowModifyRequest = 4,
    WindowBridgeRpc = 9,
    WindowViewCreated = 11
};

// Notifications from client
enum class OpcodeClientNotification : uint8_t
{
    Ready = 0,
    Exit = 1,
    WindowOpened = 2,
    WindowClosed = 3,
    // WindowResized = 4,
    WindowFocused = 5,
    WindowUnfocused = 6,
    // WindowMinimized = 7,
    // WindowMaximized = 8,
    // WindowRestored = 9,
    // WindowMoved = 10,
    // WindowKeyboardEvent = 11,
    WindowFullscreenChanged = 12,
    WindowFrameLoadStart = 13,
    WindowFrameLoadEnd = 14,
    WindowFrameLoadError = 15,
    WindowDevToolsEvent = 16,
    WindowLoadingStateChanged = 17,
    StreamCredit = 18,
    StreamCancel = 19
};

constexpr std::uint32_t kProtocolVersion = 2;
constexpr std::size_t kMaxIpcSize = 256 * 1024 * 1024;
constexpr std::size_t kPacketHeaderSize = 10;
constexpr std::uint64_t kStreamInitialCredit = 1024 * 1024;
constexpr std::size_t kStreamMaxChunk = 256 * 1024;

class ProtocolError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct PacketHeader
{
    std::uint32_t size = 0;
    std::uint32_t request_id = 0;
    PacketType packet_type = PacketType::Request;
    std::uint8_t opcode = 0;

    std::size_t BodySize() const { return static_cast<std::size_t>(size) + sizeof(std::uint32_t) - kPacketHeaderSize; }
};

inline std::array<std::uint8_t, kPacketHeaderSize> EncodeHeader(std::size_t body_size, std::uint32_t request_id, PacketType packet_type, std::uint8_t opcode)
{
    if (body_size > kMaxIpcSize)
    {
        throw ProtocolError("Packet exceeds the maximum IPC size.");
    }

    const std::uint32_t packet_size = static_cast<std::uint32_t>(body_size + kPacketHeaderSize - sizeof(std::uint32_t));
    std::array<std::uint8_t, kPacketHeaderSize> header{};
    std::memcpy(header.data(), &packet_size, sizeof(packet_size));
    std::memcpy(header.data() + sizeof(packet_size), &request_id, sizeof(request_id));
    header[8] = static_cast<std::uint8_t>(packet_type);
    header[9] = opcode;
    return header;
}

inline PacketHeader DecodeHeader(const std::uint8_t* data)
{
    PacketHeader header;
    std::memcpy(&header.size, data, sizeof(header.size));
    std::memcpy(&header.request_id, data + sizeof(header.size), sizeof(header.request_id));
    header.packet_type = static_cast<PacketType>(data[8]);
    header.opcode = data[9];

    if (header.size < kPacketHeaderSize - sizeof(std::uint32_t))
    {
        throw ProtocolError("Received an IPC packet with an invalid size.");
    }
    if (header.BodySize() > kMaxIpcSize)
    {
        throw ProtocolError("Received an IPC packet larger than the supported maximum.");
    }
    if (static_cast<std::uint8_t>(header.packet_type) > static_cast<std::uint8_t>(PacketType::Cancel))
    {
        throw ProtocolError("Received an IPC packet with an unsupported type.");
    }
    return header;
}

class PacketReader
{
public:
    PacketReader() = default;
    explicit PacketReader(std::vector<std::uint8_t> buffer) : buffer_(std::move(buffer)) {}
    PacketReader(const std::uint8_t* data, std::size_t size) : buffer_(data, data + size) {}

    template <typename T> std::optional<T> Read()
    {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        if (!HasAvailable(sizeof(T)))
        {
            return std::nullopt;
        }

        T value{};
        std::memcpy(&value, buffer_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }

    std::optional<std::string> ReadString(std::size_t size)
    {
        if (!HasAvailable(size))
        {
            return std::nullopt;
        }

        std::string value(reinterpret_cast<const char*>(buffer_.data() + position_), size);
        position_ += size;
        return value;
    }

    std::optional<std::string> ReadSizePrefixedString()
    {
        const std::optional<std::int32_t> size = Read<std::int32_t>();
        if (!size)
        {
            return std::nullopt;
        }

        if (*size < 0)
        {
            return std::nullopt;
        }

        return ReadString(static_cast<std::size_t>(*size));
    }

    std::vector<std::uint8_t> ReadBytes(std::size_t size)
    {
        if (!HasAvailable(size))
        {
            throw ProtocolError("Attempted to read past the end of a packet.");
        }

        std::vector<std::uint8_t> bytes(buffer_.begin() + static_cast<std::ptrdiff_t>(position_), buffer_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
        position_ += size;
        return bytes;
    }

    bool HasAvailable(std::size_t size) const { return position_ + size <= buffer_.size(); }

    std::size_t RemainingSize() const { return buffer_.size() - position_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t position_ = 0;
};

class PacketWriter
{
public:
    explicit PacketWriter(std::size_t max_size = kMaxIpcSize) : max_size_(max_size) { buffer_.reserve(std::min<std::size_t>(max_size_, 512)); }

    template <typename T> PacketWriter& Write(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        WriteBytes(reinterpret_cast<const std::uint8_t*>(&value), sizeof(T));
        return *this;
    }

    PacketWriter& WriteSizePrefixedString(const std::optional<std::string>& value)
    {
        if (!value)
        {
            Write<std::int32_t>(-1);
            return *this;
        }

        return WriteSizePrefixedString(*value);
    }

    PacketWriter& WriteSizePrefixedString(const std::string& value)
    {
        Write<std::int32_t>(static_cast<std::int32_t>(value.size()));
        WriteBytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
        return *this;
    }

    PacketWriter& WriteString(const std::string& value)
    {
        WriteBytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
        return *this;
    }

    PacketWriter& WriteBytes(const std::vector<std::uint8_t>& bytes) { return WriteBytes(bytes.data(), bytes.size()); }

    PacketWriter& WriteBytes(const std::uint8_t* data, std::size_t size)
    {
        if (size == 0)
        {
            return *this;
        }

        if (buffer_.size() + size > max_size_)
        {
            throw ProtocolError("Packet exceeds the maximum IPC size.");
        }

        buffer_.insert(buffer_.end(), data, data + size);
        return *this;
    }

    std::size_t Size() const { return buffer_.size(); }

    const std::uint8_t* Data() const { return buffer_.data(); }

    const std::vector<std::uint8_t>& Buffer() const { return buffer_; }

    std::vector<std::uint8_t> Release() { return std::move(buffer_); }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t max_size_;
};

struct OutgoingPacket
{
    std::vector<std::uint8_t> head;
    std::vector<std::uint8_t> body;

    std::vector<std::uint8_t> Flatten() const
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(head.size() + body.size());
        bytes.insert(bytes.end(), head.begin(), head.end());
        bytes.insert(bytes.end(), body.begin(), body.end());
        return bytes;
    }
};

inline OutgoingPacket MakePacket(PacketType packet_type, std::uint8_t opcode, std::uint32_t request_id, std::vector<std::uint8_t> body,
                                 const std::vector<std::uint8_t>& prefix = {})
{
    const auto header = EncodeHeader(prefix.size() + body.size(), request_id, packet_type, opcode);
    OutgoingPacket packet;
    packet.head.reserve(header.size() + prefix.size());
    packet.head.insert(packet.head.end(), header.begin(), header.end());
    packet.head.insert(packet.head.end(), prefix.begin(), prefix.end());
    packet.body = std::move(body);
    return packet;
}

inline OutgoingPacket MakeOkResponse(std::uint8_t opcode, std::uint32_t request_id, std::vector<std::uint8_t> payload)
{
    return MakePacket(PacketType::Response, opcode, request_id, std::move(payload), {static_cast<std::uint8_t>(Status::Ok)});
}

inline OutgoingPacket MakeStatusResponse(std::uint8_t opcode, std::uint32_t request_id, Status status, const std::optional<std::string>& message)
{
    PacketWriter writer;
    writer.Write<std::uint8_t>(static_cast<std::uint8_t>(status));
    writer.WriteSizePrefixedString(message);
    return MakePacket(PacketType::Response, opcode, request_id, writer.Release());
}

inline OutgoingPacket MakeNotification(std::uint8_t opcode, std::vector<std::uint8_t> body)
{
    return MakePacket(PacketType::Notification, opcode, 0, std::move(body));
}

inline OutgoingPacket MakeCancel(std::uint8_t opcode, std::uint32_t request_id)
{
    return MakePacket(PacketType::Cancel, opcode, request_id, {});
}

} // namespace justcef::detail
