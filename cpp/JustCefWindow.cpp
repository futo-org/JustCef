#include "JustCefWindow.h"

#include "WindowInternals.h"
#include "json.hpp"

#include <array>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace justcef {
namespace {

constexpr std::string_view kBrowserRequestExpressionTemplate = R"js(
(async () => {
    try {
        const request = JSON.parse(__REQUEST_JSON__);

        const decodeBase64 = (value) => {
            if (typeof value !== "string" || value.length === 0) {
                return undefined;
            }

            const binary = atob(value);
            const bytes = new Uint8Array(binary.length);
            for (let index = 0; index < binary.length; ++index) {
                bytes[index] = binary.charCodeAt(index);
            }
            return bytes;
        };

        const encodeBase64 = (bytes) => {
            if (!bytes || bytes.length === 0) {
                return "";
            }

            let binary = "";
            const chunkSize = 0x8000;
            for (let offset = 0; offset < bytes.length; offset += chunkSize) {
                const slice = bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length));
                binary += String.fromCharCode(...slice);
            }
            return btoa(binary);
        };

        const headers = new Headers();
        for (const [name, values] of Object.entries(request.headers ?? {})) {
            for (const value of values ?? []) {
                headers.append(name, String(value));
            }
        }

        const init = {
            method: request.method ?? "GET",
            headers,
        };

        const body = decodeBase64(request.bodyBase64);
        if (body !== undefined) {
            init.body = body;
        }

        const response = await fetch(String(request.url), init);
        const responseHeaders = {};
        response.headers.forEach((value, key) => {
            if (!responseHeaders[key]) {
                responseHeaders[key] = [];
            }
            responseHeaders[key].push(value);
        });

        const bodyBytes = new Uint8Array(await response.arrayBuffer());
        return JSON.stringify({
            success: true,
            response: {
                ok: response.ok,
                statusCode: response.status,
                statusText: response.statusText,
                url: response.url,
                headers: responseHeaders,
                bodyBase64: encodeBase64(bodyBytes),
            },
        });
    } catch (error) {
        const message =
            error && typeof error === "object" && "message" in error
                ? String(error.message)
                : String(error);

        return JSON.stringify({
            success: false,
            error: message,
        });
    }
})()
)js";

std::shared_ptr<WindowCommandTarget> RequireProcess(const std::weak_ptr<WindowCommandTarget>& command_target) {
    auto process = command_target.lock();
    if (!process) {
        throw std::runtime_error("JustCefWindow is detached from its process.");
    }

    return process;
}

asio::awaitable<std::optional<IPCResponse>> MakeReadyProxyAwaitable(std::optional<IPCResponse> response) {
    co_return response;
}

asio::awaitable<std::optional<IPCRequest>> MakeReadyModifierAwaitable(std::optional<IPCRequest> request) {
    co_return request;
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const std::size_t remaining = bytes.size() - index;
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(bytes[index]) << 16) |
            ((remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) : 0U) << 8) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0U);

        encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        encoded.push_back(remaining > 1 ? kAlphabet[(chunk >> 6) & 0x3F] : '=');
        encoded.push_back(remaining > 2 ? kAlphabet[chunk & 0x3F] : '=');
    }

    return encoded;
}

int DecodeBase64Value(char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }

    return -1;
}

std::vector<std::uint8_t> DecodeBase64(std::string_view encoded) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve((encoded.size() / 4) * 3);

    std::array<int, 4> chunk{};
    std::size_t chunk_size = 0;
    std::size_t padding = 0;

    for (const char value : encoded) {
        if (value == '=') {
            chunk[chunk_size++] = 0;
            ++padding;
        } else {
            const int decoded_value = DecodeBase64Value(value);
            if (decoded_value < 0) {
                throw std::runtime_error("Browser request returned invalid base64 data.");
            }

            chunk[chunk_size++] = decoded_value;
        }

        if (chunk_size == chunk.size()) {
            decoded.push_back(static_cast<std::uint8_t>((chunk[0] << 2) | (chunk[1] >> 4)));
            if (padding < 2) {
                decoded.push_back(static_cast<std::uint8_t>(((chunk[1] & 0x0F) << 4) | (chunk[2] >> 2)));
            }
            if (padding == 0) {
                decoded.push_back(static_cast<std::uint8_t>(((chunk[2] & 0x03) << 6) | chunk[3]));
            }

            chunk_size = 0;
            padding = 0;
        }
    }

    if (chunk_size != 0) {
        throw std::runtime_error("Browser request returned truncated base64 data.");
    }

    return decoded;
}

nlohmann::json HeaderMapToJson(const HeaderMap& headers) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [name, values] : headers) {
        json[name] = values;
    }
    return json;
}

HeaderMap HeaderMapFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        throw std::runtime_error("Browser request headers must be a JSON object.");
    }

    HeaderMap headers;
    for (const auto& [name, value] : json.items()) {
        if (!value.is_array()) {
            throw std::runtime_error("Browser request header values must be arrays.");
        }

        std::vector<std::string> values;
        values.reserve(value.size());
        for (const auto& entry : value) {
            values.push_back(entry.get<std::string>());
        }

        headers.emplace(name, std::move(values));
    }

    return headers;
}

std::string ReplaceRequestJson(std::string template_text, std::string_view request_json) {
    static constexpr std::string_view kMarker = "__REQUEST_JSON__";

    const auto marker_position = template_text.find(kMarker);
    if (marker_position == std::string::npos) {
        throw std::runtime_error("Browser request expression template is invalid.");
    }

    template_text.replace(marker_position, kMarker.size(), request_json);
    return template_text;
}

std::string BuildBrowserRequestExpression(const BrowserRequest& request) {
    nlohmann::json request_json = {
        {"method", request.method},
        {"url", request.url},
        {"headers", HeaderMapToJson(request.headers)},
    };
    if (!request.body.empty()) {
        request_json["bodyBase64"] = EncodeBase64(request.body);
    }

    return ReplaceRequestJson(
        std::string(kBrowserRequestExpressionTemplate),
        nlohmann::json(request_json.dump()).dump());
}

std::string ParseDevToolsStringResult(const DevToolsMethodResult& result) {
    if (!result.success) {
        throw std::runtime_error("Browser request DevTools invocation failed.");
    }

    const std::string payload(result.data.begin(), result.data.end());
    if (payload.empty()) {
        throw std::runtime_error("Browser request returned an empty DevTools payload.");
    }

    const auto root = nlohmann::json::parse(payload);

    const auto exception_it = root.find("exceptionDetails");
    if (exception_it != root.end()) {
        const std::string message = exception_it->value("text", "Browser request execution failed.");
        throw std::runtime_error(message);
    }

    const auto result_it = root.find("result");
    if (result_it == root.end() || !result_it->is_object()) {
        throw std::runtime_error("Browser request returned a malformed DevTools result.");
    }

    const auto value_it = result_it->find("value");
    if (value_it == result_it->end() || !value_it->is_string()) {
        throw std::runtime_error("Browser request did not return a string payload.");
    }

    return value_it->get<std::string>();
}

BrowserResponse ParseBrowserResponse(std::string_view payload) {
    const auto root = nlohmann::json::parse(payload);
    if (!root.value("success", false)) {
        throw std::runtime_error(root.value("error", "Browser request failed."));
    }

    const auto response_it = root.find("response");
    if (response_it == root.end() || !response_it->is_object()) {
        throw std::runtime_error("Browser request returned a malformed response payload.");
    }

    BrowserResponse response;
    response.ok = response_it->value("ok", false);
    response.status_code = response_it->value("statusCode", 0);
    response.status_text = response_it->value("statusText", std::string{});
    response.url = response_it->value("url", std::string{});
    response.headers = HeaderMapFromJson(response_it->value("headers", nlohmann::json::object()));
    response.body = DecodeBase64(response_it->value("bodyBase64", std::string{}));
    return response;
}

}  // namespace

JustCefWindow::JustCefWindow(
    int identifier,
    std::weak_ptr<WindowCommandTarget> command_target,
    std::shared_ptr<WindowShared> shared)
    : identifier_(identifier),
      command_target_(std::move(command_target)),
      shared_(std::move(shared)) {}

JustCefWindow::~JustCefWindow() = default;

int JustCefWindow::Identifier() const {
    return identifier_;
}

asio::awaitable<void> JustCefWindow::MaximizeAsync() {
    return RequireProcess(command_target_)->WindowMaximizeAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::MinimizeAsync() {
    return RequireProcess(command_target_)->WindowMinimizeAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::RestoreAsync() {
    return RequireProcess(command_target_)->WindowRestoreAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::ShowAsync() {
    return RequireProcess(command_target_)->WindowShowAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::HideAsync() {
    return RequireProcess(command_target_)->WindowHideAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::ActivateAsync() {
    return RequireProcess(command_target_)->WindowActivateAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::BringToTopAsync() {
    return RequireProcess(command_target_)->WindowBringToTopAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::SetAlwaysOnTopAsync(bool always_on_top) {
    return RequireProcess(command_target_)->WindowSetAlwaysOnTopAsync(Identifier(), always_on_top);
}

asio::awaitable<void> JustCefWindow::LoadUrlAsync(std::string url) {
    return RequireProcess(command_target_)->WindowLoadUrlAsync(Identifier(), std::move(url));
}

asio::awaitable<void> JustCefWindow::SetPositionAsync(int x, int y) {
    return RequireProcess(command_target_)->WindowSetPositionAsync(Identifier(), x, y);
}

asio::awaitable<Position> JustCefWindow::GetPositionAsync() {
    return RequireProcess(command_target_)->WindowGetPositionAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::SetSizeAsync(int width, int height) {
    return RequireProcess(command_target_)->WindowSetSizeAsync(Identifier(), width, height);
}

asio::awaitable<Size> JustCefWindow::GetSizeAsync() {
    return RequireProcess(command_target_)->WindowGetSizeAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::SetZoomAsync(double zoom) {
    return RequireProcess(command_target_)->WindowSetZoomAsync(Identifier(), zoom);
}

asio::awaitable<double> JustCefWindow::GetZoomAsync() {
    return RequireProcess(command_target_)->WindowGetZoomAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::CloseAsync(bool force_close) {
    return RequireProcess(command_target_)->WindowCloseAsync(Identifier(), force_close);
}

asio::awaitable<void> JustCefWindow::SetFullscreenAsync(bool fullscreen) {
    return RequireProcess(command_target_)->WindowSetFullscreenAsync(Identifier(), fullscreen);
}

asio::awaitable<void> JustCefWindow::RequestFocusAsync() {
    return RequireProcess(command_target_)->RequestFocusAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::SetDevelopmentToolsEnabledAsync(bool development_tools_enabled) {
    return RequireProcess(command_target_)->WindowSetDevelopmentToolsEnabledAsync(Identifier(), development_tools_enabled);
}

asio::awaitable<void> JustCefWindow::SetDevelopmentToolsVisibleAsync(bool development_tools_visible) {
    return RequireProcess(command_target_)->WindowSetDevelopmentToolsVisibleAsync(Identifier(), development_tools_visible);
}

asio::awaitable<DevToolsMethodResult> JustCefWindow::ExecuteDevToolsMethodAsync(
    std::string method_name,
    std::optional<std::string> json) {
    return RequireProcess(command_target_)->WindowExecuteDevToolsMethodAsync(Identifier(), std::move(method_name), std::move(json));
}

asio::awaitable<BrowserResponse> JustCefWindow::ExecuteBrowserRequestAsync(BrowserRequest request) {
    const std::string expression = BuildBrowserRequestExpression(request);
    const nlohmann::json devtools_request = {
        {"expression", expression},
        {"awaitPromise", true},
        {"returnByValue", true},
    };

    const auto result = co_await ExecuteDevToolsMethodAsync("Runtime.evaluate", devtools_request.dump());
    co_return ParseBrowserResponse(ParseDevToolsStringResult(result));
}

asio::awaitable<void> JustCefWindow::SetTitleAsync(std::string title) {
    return RequireProcess(command_target_)->WindowSetTitleAsync(Identifier(), std::move(title));
}

asio::awaitable<void> JustCefWindow::SetIconAsync(std::string icon_path) {
    return RequireProcess(command_target_)->WindowSetIconAsync(Identifier(), std::move(icon_path));
}

asio::awaitable<void> JustCefWindow::AddUrlToProxyAsync(std::string url) {
    return RequireProcess(command_target_)->WindowAddUrlToProxyAsync(Identifier(), std::move(url));
}

asio::awaitable<void> JustCefWindow::RemoveUrlToProxyAsync(std::string url) {
    return RequireProcess(command_target_)->WindowRemoveUrlToProxyAsync(Identifier(), std::move(url));
}

asio::awaitable<void> JustCefWindow::AddDomainToProxyAsync(std::string domain) {
    return RequireProcess(command_target_)->WindowAddDomainToProxyAsync(Identifier(), std::move(domain));
}

asio::awaitable<void> JustCefWindow::RemoveDomainToProxyAsync(std::string domain) {
    return RequireProcess(command_target_)->WindowRemoveDomainToProxyAsync(Identifier(), std::move(domain));
}

asio::awaitable<void> JustCefWindow::AddUrlToModifyAsync(std::string url) {
    return RequireProcess(command_target_)->WindowAddUrlToModifyAsync(Identifier(), std::move(url));
}

asio::awaitable<void> JustCefWindow::RemoveUrlToModifyAsync(std::string url) {
    return RequireProcess(command_target_)->WindowRemoveUrlToModifyAsync(Identifier(), std::move(url));
}

asio::awaitable<void> JustCefWindow::AddDevToolsEventMethod(std::string method) {
    return RequireProcess(command_target_)->WindowAddDevToolsEventMethod(Identifier(), std::move(method));
}

asio::awaitable<void> JustCefWindow::RemoveDevToolsEventMethod(std::string method) {
    return RequireProcess(command_target_)->WindowRemoveDevToolsEventMethod(Identifier(), std::move(method));
}

asio::awaitable<void> JustCefWindow::CenterSelfAsync() {
    return RequireProcess(command_target_)->WindowCenterSelfAsync(Identifier());
}

asio::awaitable<void> JustCefWindow::SetProxyRequestsAsync(bool proxy_requests) {
    if (proxy_requests) {
        std::lock_guard<std::mutex> lock(shared_->request_mutex);
        if (!shared_->request_proxy) {
            throw std::invalid_argument("When proxy_requests is true, request_proxy must be set.");
        }
    }

    return RequireProcess(command_target_)->WindowSetProxyRequestsAsync(Identifier(), proxy_requests);
}

asio::awaitable<void> JustCefWindow::SetModifyRequestsAsync(bool modify_requests, bool modify_body) {
    return RequireProcess(command_target_)->WindowSetModifyRequestsAsync(Identifier(), modify_requests, modify_body);
}

void JustCefWindow::SetRequestProxy(RequestProxy request_proxy) {
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = std::move(request_proxy);
}

void JustCefWindow::SetRequestProxy(SyncRequestProxy request_proxy) {
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = [handler = std::move(request_proxy)](JustCefWindow& window, const IPCRequest& request) mutable {
        return MakeReadyProxyAwaitable(handler(window, request));
    };
}

void JustCefWindow::SetRequestModifier(RequestModifier request_modifier) {
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = std::move(request_modifier);
}

void JustCefWindow::SetRequestModifier(SyncRequestModifier request_modifier) {
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = [handler = std::move(request_modifier)](JustCefWindow& window, const IPCRequest& request) mutable {
        return MakeReadyModifierAwaitable(handler(window, request));
    };
}

void JustCefWindow::WaitForExit() const {
    shared_->close_signal.Wait();
}

asio::awaitable<void> JustCefWindow::WaitForExitAsync() const {
    co_await shared_->close_signal.AsyncWait(shared_->executor);
}

}  // namespace justcef
