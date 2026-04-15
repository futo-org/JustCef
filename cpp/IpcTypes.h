#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include "asio.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace justcef {

class JustCefWindow;

struct CaseInsensitiveLess {
    bool operator()(const std::string& left, const std::string& right) const {
        return std::lexicographical_compare(
            left.begin(),
            left.end(),
            right.begin(),
            right.end(),
            [](unsigned char lhs, unsigned char rhs) { return std::tolower(lhs) < std::tolower(rhs); });
    }
};

using HeaderMap = std::map<std::string, std::vector<std::string>, CaseInsensitiveLess>;

class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual std::size_t Read(std::uint8_t* buffer, std::size_t size) = 0;
    virtual void Close() {}
};

class MemoryByteStream final : public ByteStream {
public:
    explicit MemoryByteStream(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::size_t Read(std::uint8_t* buffer, std::size_t size) override {
        if (position_ >= bytes_.size()) {
            return 0;
        }

        const std::size_t remaining = bytes_.size() - position_;
        const std::size_t to_copy = std::min(size, remaining);
        std::copy_n(bytes_.data() + position_, to_copy, buffer);
        position_ += to_copy;
        return to_copy;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

enum class IPCProxyBodyElementType : std::uint8_t {
    Empty = 0,
    Bytes = 1,
    File = 2,
};

struct IPCProxyBodyElement {
    IPCProxyBodyElementType type = IPCProxyBodyElementType::Empty;
    std::vector<std::uint8_t> data;
    std::string file_name;

    static IPCProxyBodyElement Bytes(std::vector<std::uint8_t> bytes) {
        IPCProxyBodyElement element;
        element.type = IPCProxyBodyElementType::Bytes;
        element.data = std::move(bytes);
        return element;
    }

    static IPCProxyBodyElement File(std::string file_name) {
        IPCProxyBodyElement element;
        element.type = IPCProxyBodyElementType::File;
        element.file_name = std::move(file_name);
        return element;
    }
};

struct IPCRequest {
    std::string method;
    std::string url;
    HeaderMap headers;
    std::vector<IPCProxyBodyElement> elements;
};

struct IPCResponse {
    int status_code = 0;
    std::string status_text;
    HeaderMap headers;
    std::shared_ptr<ByteStream> body_stream;
};

struct Position {
    int x = 0;
    int y = 0;
};

struct Size {
    int width = 0;
    int height = 0;
};

struct FileFilter {
    std::string name;
    std::string pattern;
};

struct DevToolsMethodResult {
    bool success = false;
    std::vector<std::uint8_t> data;
};

struct BrowserRequest {
    std::string method = "GET";
    std::string url;
    HeaderMap headers;
    std::vector<std::uint8_t> body;
};

struct BrowserResponse {
    bool ok = false;
    int status_code = 0;
    std::string status_text;
    std::string url;
    HeaderMap headers;
    std::vector<std::uint8_t> body;
};

using SyncRequestModifier = std::function<std::optional<IPCRequest>(JustCefWindow&, const IPCRequest&)>;
using RequestModifier = std::function<asio::awaitable<std::optional<IPCRequest>>(JustCefWindow&, const IPCRequest&)>;
using SyncRequestProxy = std::function<std::optional<IPCResponse>(JustCefWindow&, const IPCRequest&)>;
using RequestProxy = std::function<asio::awaitable<std::optional<IPCResponse>>(JustCefWindow&, const IPCRequest&)>;

}  // namespace justcef
