#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace justcef::detail {

enum class PacketType : std::uint8_t {
    Request = 0,
    Response = 1,
    Notification = 2,
};

enum class OpcodeController : uint8_t {
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowCreate = 3,
    //WindowCreatePositioned = 4,
    WindowSetDevelopmentToolsEnabled = 5,
    WindowLoadUrl = 6,
    //WindowLoadHtml = 7,
    //WindowExecuteJavascript = 8, //string js
    WindowSetZoom = 9, //double zoom
    //WindowSetResizable = 10, //bool value
    //WindowSetWindowless = 11, //bool value
    //WindowGetWindowSize = 12,
    //WindowSetWindowSize = 13, //Size size
    WindowGetPosition = 14,
    WindowSetPosition = 15, //Position value
    //WindowCenterWindow = 16,
    WindowMaximize = 17,
    WindowMinimize = 18,
    WindowRestore = 19,
    WindowShow = 20,
    WindowHide = 21,
    WindowClose = 22,
    //WindowSetRequestModificationEnabled = 23, //bool enabled
    //WindowModifyRequest = 24, //Request request -> Response
    WindowRequestFocus = 25,
    //WindowRegisterKeyboardListener = 26,
    //WindowSetTitle = 27,
    WindowActivate = 28,
    WindowBringToTop = 29,
    WindowSetAlwaysOnTop = 30,
    WindowSetFullscreen = 31,
    WindowCenterSelf = 32,
    WindowSetProxyRequests = 33,
    WindowSetModifyRequests = 34,
    StreamOpen = 35,
    StreamClose = 36,
    StreamData = 37,
    StreamCancel = 38,
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
    WindowBridgeRpc = 57
};

//Notifications from controller
enum class OpcodeControllerNotification : uint8_t {
    Exit = 0
};

//Requests from client
enum class OpcodeClient : uint8_t {
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowProxyRequest = 3,
    WindowModifyRequest = 4,
    StreamOpen = 5,
    StreamData = 6,
    StreamClose = 7,
    StreamCancel = 8,
    WindowBridgeRpc = 9
};

//Notifications from client
enum class OpcodeClientNotification : uint8_t {
    Ready = 0,
    Exit = 1,
    WindowOpened = 2,
    WindowClosed = 3,
    //WindowResized = 4,
    WindowFocused = 5,
    WindowUnfocused = 6,
    //WindowMinimized = 7,
    //WindowMaximized = 8,
    //WindowRestored = 9,
    //WindowMoved = 10,
    //WindowKeyboardEvent = 11,
    WindowFullscreenChanged = 12,
    WindowFrameLoadStart = 13,
    WindowFrameLoadEnd = 14,
    WindowFrameLoadError = 15,
    WindowDevToolsEvent = 16,
    WindowLoadingStateChanged = 17
};

constexpr std::size_t kMaxIpcSize = 10 * 1024 * 1024;
constexpr std::size_t kPacketHeaderSize = 10;

struct PacketHeader {
    std::uint32_t size = 0;
    std::uint32_t request_id = 0;
    PacketType packet_type = PacketType::Request;
    std::uint8_t opcode = 0;
};

class PacketReader {
public:
    PacketReader() = default;
    explicit PacketReader(std::vector<std::uint8_t> buffer) : buffer_(std::move(buffer)) {}
    PacketReader(const std::uint8_t* data, std::size_t size) : buffer_(data, data + size) {}

    template <typename T>
    std::optional<T> Read() {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        if (!HasAvailable(sizeof(T))) {
            return std::nullopt;
        }

        T value{};
        std::memcpy(&value, buffer_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }

    std::optional<std::string> ReadString(std::size_t size) {
        if (!HasAvailable(size)) {
            return std::nullopt;
        }

        std::string value(reinterpret_cast<const char*>(buffer_.data() + position_), size);
        position_ += size;
        return value;
    }

    std::optional<std::string> ReadSizePrefixedString() {
        const std::optional<std::int32_t> size = Read<std::int32_t>();
        if (!size) {
            return std::nullopt;
        }

        if (*size < 0) {
            return std::nullopt;
        }

        return ReadString(static_cast<std::size_t>(*size));
    }

    std::vector<std::uint8_t> ReadBytes(std::size_t size) {
        if (!HasAvailable(size)) {
            throw std::runtime_error("Attempted to read past the end of a packet.");
        }

        std::vector<std::uint8_t> bytes(buffer_.begin() + static_cast<std::ptrdiff_t>(position_),
                                        buffer_.begin() + static_cast<std::ptrdiff_t>(position_ + size));
        position_ += size;
        return bytes;
    }

    bool HasAvailable(std::size_t size) const {
        return position_ + size <= buffer_.size();
    }

    std::size_t RemainingSize() const {
        return buffer_.size() - position_;
    }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t position_ = 0;
};

class PacketWriter {
public:
    explicit PacketWriter(std::size_t max_size = kMaxIpcSize) : max_size_(max_size) {
        buffer_.reserve(std::min<std::size_t>(max_size_, 512));
    }

    template <typename T>
    PacketWriter& Write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        WriteBytes(reinterpret_cast<const std::uint8_t*>(&value), sizeof(T));
        return *this;
    }

    PacketWriter& WriteSizePrefixedString(const std::optional<std::string>& value) {
        if (!value) {
            Write<std::int32_t>(-1);
            return *this;
        }

        return WriteSizePrefixedString(*value);
    }

    PacketWriter& WriteSizePrefixedString(const std::string& value) {
        Write<std::int32_t>(static_cast<std::int32_t>(value.size()));
        WriteBytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
        return *this;
    }

    PacketWriter& WriteString(const std::string& value) {
        WriteBytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
        return *this;
    }

    PacketWriter& WriteBytes(const std::vector<std::uint8_t>& bytes) {
        return WriteBytes(bytes.data(), bytes.size());
    }

    PacketWriter& WriteBytes(const std::uint8_t* data, std::size_t size) {
        if (size == 0) {
            return *this;
        }

        if (buffer_.size() + size > max_size_) {
            throw std::runtime_error("Packet exceeds the maximum IPC size.");
        }

        buffer_.insert(buffer_.end(), data, data + size);
        return *this;
    }

    std::size_t Size() const {
        return buffer_.size();
    }

    const std::uint8_t* Data() const {
        return buffer_.data();
    }

    const std::vector<std::uint8_t>& Buffer() const {
        return buffer_;
    }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t max_size_;
};

}  // namespace justcef::detail
