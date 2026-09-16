#include "JustCefWindow.h"

#include "WindowInternals.h"
#include "json.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace justcef
{
namespace
{

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

std::shared_ptr<WindowCommandTarget> RequireProcess(const std::weak_ptr<WindowCommandTarget>& command_target)
{
    auto process = command_target.lock();
    if (!process)
    {
        throw std::runtime_error("JustCefWindow is detached from its process.");
    }

    return process;
}

template <typename Result, typename... Parameters, typename... Arguments>
asio::awaitable<Result> CallProcess(std::weak_ptr<WindowCommandTarget> command_target, asio::awaitable<Result> (WindowCommandTarget::*method)(Parameters...),
                                    Arguments... arguments)
{
    auto process = RequireProcess(command_target);
    co_return co_await (process.get()->*method)(std::move(arguments)...);
}

asio::awaitable<void> LoadUrl(std::weak_ptr<WindowCommandTarget> command_target, std::shared_ptr<WindowShared> shared, int identifier, std::string url, bool wait_for_navigation)
{
    auto navigation = shared->loading.ArmNavigation();
    try
    {
        co_await CallProcess(std::move(command_target), &WindowCommandTarget::WindowLoadUrlAsync, identifier, std::move(url));
    }
    catch (...)
    {
        shared->loading.FailNavigation(navigation, std::current_exception());
        throw;
    }

    if (wait_for_navigation)
    {
        co_await navigation->AsyncWait(shared->executor);
    }
}

asio::awaitable<std::optional<IPCResponse>> MakeReadyProxyAwaitable(std::optional<IPCResponse> response)
{
    co_return response;
}

asio::awaitable<std::optional<IPCRequest>> MakeReadyModifierAwaitable(std::optional<IPCRequest> request)
{
    co_return request;
}

asio::awaitable<std::optional<std::string>> MakeReadyBridgeAwaitable(std::optional<std::string> response)
{
    co_return response;
}

asio::awaitable<void> MakeReadyViewCreatedAwaitable()
{
    co_return;
}

template <typename Bound, typename Target, typename Handler>
Bound BindToTarget(Target& target, Handler handler)
{
    if (!handler)
    {
        return {};
    }

    return [&target, handler = std::move(handler)](const IPCRequest& request) mutable
    {
        return handler(target, request);
    };
}

template <typename Bound, typename Target, typename Handler, typename Ready>
Bound BindSyncToTarget(Target& target, Handler handler, Ready ready)
{
    if (!handler)
    {
        return {};
    }

    return [&target, handler = std::move(handler), ready](const IPCRequest& request) mutable
    {
        return ready(handler(target, request));
    };
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    for (std::size_t index = 0; index < bytes.size(); index += 3)
    {
        const std::size_t remaining = bytes.size() - index;
        const std::uint32_t chunk = (static_cast<std::uint32_t>(bytes[index]) << 16) | ((remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) : 0U) << 8) |
                                    (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0U);

        encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        encoded.push_back(remaining > 1 ? kAlphabet[(chunk >> 6) & 0x3F] : '=');
        encoded.push_back(remaining > 2 ? kAlphabet[chunk & 0x3F] : '=');
    }

    return encoded;
}

int DecodeBase64Value(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    if (value == '+')
    {
        return 62;
    }
    if (value == '/')
    {
        return 63;
    }

    return -1;
}

std::vector<std::uint8_t> DecodeBase64(std::string_view encoded)
{
    std::vector<std::uint8_t> decoded;
    decoded.reserve((encoded.size() / 4) * 3);

    std::array<int, 4> chunk{};
    std::size_t chunk_size = 0;
    std::size_t padding = 0;

    for (const char value : encoded)
    {
        if (value == '=')
        {
            chunk[chunk_size++] = 0;
            ++padding;
        }
        else
        {
            const int decoded_value = DecodeBase64Value(value);
            if (decoded_value < 0)
            {
                throw std::runtime_error("Browser request returned invalid base64 data.");
            }

            chunk[chunk_size++] = decoded_value;
        }

        if (chunk_size == chunk.size())
        {
            decoded.push_back(static_cast<std::uint8_t>((chunk[0] << 2) | (chunk[1] >> 4)));
            if (padding < 2)
            {
                decoded.push_back(static_cast<std::uint8_t>(((chunk[1] & 0x0F) << 4) | (chunk[2] >> 2)));
            }
            if (padding == 0)
            {
                decoded.push_back(static_cast<std::uint8_t>(((chunk[2] & 0x03) << 6) | chunk[3]));
            }

            chunk_size = 0;
            padding = 0;
        }
    }

    if (chunk_size != 0)
    {
        throw std::runtime_error("Browser request returned truncated base64 data.");
    }

    return decoded;
}

nlohmann::json HeaderMapToJson(const HeaderMap& headers)
{
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [name, values] : headers)
    {
        json[name] = values;
    }
    return json;
}

HeaderMap HeaderMapFromJson(const nlohmann::json& json)
{
    if (!json.is_object())
    {
        throw std::runtime_error("Browser request headers must be a JSON object.");
    }

    HeaderMap headers;
    for (const auto& [name, value] : json.items())
    {
        if (!value.is_array())
        {
            throw std::runtime_error("Browser request header values must be arrays.");
        }

        std::vector<std::string> values;
        values.reserve(value.size());
        for (const auto& entry : value)
        {
            values.push_back(entry.get<std::string>());
        }

        headers.emplace(name, std::move(values));
    }

    return headers;
}

std::string ReplaceRequestJson(std::string template_text, std::string_view request_json)
{
    static constexpr std::string_view kMarker = "__REQUEST_JSON__";

    const auto marker_position = template_text.find(kMarker);
    if (marker_position == std::string::npos)
    {
        throw std::runtime_error("Browser request expression template is invalid.");
    }

    template_text.replace(marker_position, kMarker.size(), request_json);
    return template_text;
}

std::string BuildBrowserRequestExpression(const BrowserRequest& request)
{
    nlohmann::json request_json = {
        {"method", request.method},
        {"url", request.url},
        {"headers", HeaderMapToJson(request.headers)},
    };
    if (!request.body.empty())
    {
        request_json["bodyBase64"] = EncodeBase64(request.body);
    }

    return ReplaceRequestJson(std::string(kBrowserRequestExpressionTemplate), nlohmann::json(request_json.dump()).dump());
}

std::string ParseDevToolsStringResult(const DevToolsMethodResult& result)
{
    if (!result.success)
    {
        throw std::runtime_error("Browser request DevTools invocation failed.");
    }

    const std::string payload(result.data.begin(), result.data.end());
    if (payload.empty())
    {
        throw std::runtime_error("Browser request returned an empty DevTools payload.");
    }

    const auto root = nlohmann::json::parse(payload);

    const auto exception_it = root.find("exceptionDetails");
    if (exception_it != root.end())
    {
        const std::string message = exception_it->value("text", "Browser request execution failed.");
        throw std::runtime_error(message);
    }

    const auto result_it = root.find("result");
    if (result_it == root.end() || !result_it->is_object())
    {
        throw std::runtime_error("Browser request returned a malformed DevTools result.");
    }

    const auto value_it = result_it->find("value");
    if (value_it == result_it->end() || !value_it->is_string())
    {
        throw std::runtime_error("Browser request did not return a string payload.");
    }

    return value_it->get<std::string>();
}

BrowserResponse ParseBrowserResponse(std::string_view payload)
{
    const auto root = nlohmann::json::parse(payload);
    if (!root.value("success", false))
    {
        throw std::runtime_error(root.value("error", "Browser request failed."));
    }

    const auto response_it = root.find("response");
    if (response_it == root.end() || !response_it->is_object())
    {
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

constexpr int kErrorAborted = -3;

std::exception_ptr ClosedError()
{
    return std::make_exception_ptr(std::runtime_error("Window was closed before loading completed."));
}

} // namespace

void LoadingState::Apply(bool is_loading, bool can_go_back, bool can_go_forward)
{
    std::shared_ptr<detail::AsyncSignal> navigation_done;
    std::shared_ptr<detail::AsyncSignal> idle_done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
        {
            return;
        }

        is_loading_ = is_loading;
        can_go_back_ = can_go_back;
        can_go_forward_ = can_go_forward;

        if (is_loading)
        {
            if (navigation_)
            {
                navigation_saw_loading_ = true;
            }
        }
        else
        {
            if (navigation_ && navigation_saw_loading_)
            {
                navigation_done = std::move(navigation_);
                navigation_.reset();
            }
            if (!navigation_)
            {
                idle_done = std::move(idle_);
                idle_.reset();
            }
        }
    }

    if (navigation_done)
    {
        navigation_done->SignalSuccess();
    }
    if (idle_done)
    {
        idle_done->SignalSuccess();
    }
}

void LoadingState::OnMainFrameLoadError(int error_code, const std::string& error_text)
{
    if (error_code == kErrorAborted)
    {
        return;
    }

    std::shared_ptr<detail::AsyncSignal> navigation_failed;
    std::shared_ptr<detail::AsyncSignal> idle_done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !navigation_)
        {
            return;
        }

        navigation_failed = std::move(navigation_);
        navigation_.reset();
        if (!is_loading_)
        {
            idle_done = std::move(idle_);
            idle_.reset();
        }
    }

    const std::string message = error_text.empty() ? "Navigation failed with error " + std::to_string(error_code) + "." : error_text;
    navigation_failed->SignalFailure(std::make_exception_ptr(std::runtime_error(message)));
    if (idle_done)
    {
        idle_done->SignalSuccess();
    }
}

void LoadingState::Close()
{
    std::shared_ptr<detail::AsyncSignal> navigation;
    std::shared_ptr<detail::AsyncSignal> idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
        {
            return;
        }

        closed_ = true;
        is_loading_ = false;
        navigation = std::move(navigation_);
        navigation_.reset();
        idle = std::move(idle_);
        idle_.reset();
    }

    if (navigation)
    {
        navigation->SignalFailure(ClosedError());
    }
    if (idle)
    {
        idle->SignalFailure(ClosedError());
    }
}

std::shared_ptr<detail::AsyncSignal> LoadingState::ArmNavigation()
{
    auto navigation = std::make_shared<detail::AsyncSignal>();
    std::shared_ptr<detail::AsyncSignal> superseded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
        {
            navigation->SignalFailure(ClosedError());
            return navigation;
        }

        superseded = std::move(navigation_);
        navigation_ = navigation;
        navigation_saw_loading_ = false;
    }

    if (superseded)
    {
        superseded->SignalFailure(std::make_exception_ptr(std::runtime_error("Navigation was superseded by a newer navigation.")));
    }
    return navigation;
}

void LoadingState::FailNavigation(const std::shared_ptr<detail::AsyncSignal>& navigation, std::exception_ptr exception)
{
    std::shared_ptr<detail::AsyncSignal> idle_done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (navigation_ == navigation)
        {
            navigation_.reset();
            if (!is_loading_)
            {
                idle_done = std::move(idle_);
                idle_.reset();
            }
        }
    }

    navigation->SignalFailure(std::move(exception));
    if (idle_done)
    {
        idle_done->SignalSuccess();
    }
}

std::shared_ptr<detail::AsyncSignal> LoadingState::IdleSignal()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || (!is_loading_ && !navigation_))
    {
        return nullptr;
    }

    if (!idle_)
    {
        idle_ = std::make_shared<detail::AsyncSignal>();
    }
    return idle_;
}

bool LoadingState::IsLoading() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return is_loading_;
}

bool LoadingState::CanGoBack() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return can_go_back_;
}

bool LoadingState::CanGoForward() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return can_go_forward_;
}

JustCefBrowser::JustCefBrowser(int identifier, std::weak_ptr<WindowCommandTarget> command_target, std::shared_ptr<WindowShared> shared)
    : identifier_(identifier), command_target_(std::move(command_target)), shared_(std::move(shared))
{
}

JustCefBrowser::~JustCefBrowser() = default;

JustCefWindow::JustCefWindow(int identifier, std::weak_ptr<WindowCommandTarget> command_target, std::shared_ptr<WindowShared> shared)
    : JustCefBrowser(identifier, std::move(command_target), std::move(shared))
{
}

JustCefView::JustCefView(int identifier, std::weak_ptr<WindowCommandTarget> command_target, std::shared_ptr<WindowShared> shared, std::weak_ptr<JustCefWindow> parent)
    : JustCefBrowser(identifier, std::move(command_target), std::move(shared)), parent_(std::move(parent))
{
}

int JustCefBrowser::Identifier() const
{
    return identifier_;
}

asio::awaitable<void> JustCefWindow::MaximizeAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowMaximizeAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::MinimizeAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowMinimizeAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::RestoreAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowRestoreAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::ShowAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowShowAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::HideAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowHideAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::ActivateAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowActivateAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::BringToTopAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowBringToTopAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::SetAlwaysOnTopAsync(bool always_on_top)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetAlwaysOnTopAsync, Identifier(), always_on_top);
}

asio::awaitable<void> JustCefBrowser::LoadUrlAsync(std::string url)
{
    return LoadUrl(command_target_, shared_, Identifier(), std::move(url), false);
}

asio::awaitable<void> JustCefBrowser::NavigateAsync(std::string url)
{
    return LoadUrl(command_target_, shared_, Identifier(), std::move(url), true);
}

asio::awaitable<void> JustCefWindow::SetPositionAsync(int x, int y)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetPositionAsync, Identifier(), x, y);
}

asio::awaitable<Position> JustCefWindow::GetPositionAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowGetPositionAsync, Identifier());
}

asio::awaitable<void> JustCefWindow::SetSizeAsync(int width, int height)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetSizeAsync, Identifier(), width, height);
}

asio::awaitable<Size> JustCefWindow::GetSizeAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowGetSizeAsync, Identifier());
}

asio::awaitable<void> JustCefBrowser::SetZoomAsync(double zoom)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetZoomAsync, Identifier(), zoom);
}

asio::awaitable<double> JustCefBrowser::GetZoomAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowGetZoomAsync, Identifier());
}

asio::awaitable<std::vector<std::string>> JustCefBrowser::PickFileAsync(bool multiple, std::vector<FileFilter> filters)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowPickFileAsync, Identifier(), multiple, std::move(filters));
}

asio::awaitable<std::string> JustCefBrowser::PickDirectoryAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowPickDirectoryAsync, Identifier());
}

asio::awaitable<std::string> JustCefBrowser::SaveFileAsync(std::string default_name, std::vector<FileFilter> filters)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSaveFileAsync, Identifier(), std::move(default_name), std::move(filters));
}

asio::awaitable<void> JustCefBrowser::CloseAsync(bool force_close)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowCloseAsync, Identifier(), force_close);
}

asio::awaitable<void> JustCefWindow::SetFullscreenAsync(bool fullscreen)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetFullscreenAsync, Identifier(), fullscreen);
}

asio::awaitable<void> JustCefBrowser::RequestFocusAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::RequestFocusAsync, Identifier());
}

asio::awaitable<void> JustCefBrowser::SetDevelopmentToolsEnabledAsync(bool development_tools_enabled)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetDevelopmentToolsEnabledAsync, Identifier(), development_tools_enabled);
}

asio::awaitable<void> JustCefBrowser::SetDevelopmentToolsVisibleAsync(bool development_tools_visible)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetDevelopmentToolsVisibleAsync, Identifier(), development_tools_visible);
}

asio::awaitable<DevToolsMethodResult> JustCefBrowser::ExecuteDevToolsMethodAsync(std::string method_name, std::optional<std::string> json)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowExecuteDevToolsMethodAsync, Identifier(), std::move(method_name), std::move(json));
}

asio::awaitable<std::string> JustCefWindow::CallBridgeRpcAsync(std::string method, std::optional<std::string> json)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowBridgeRpcAsync, Identifier(), std::move(method), std::move(json));
}

asio::awaitable<BrowserResponse> JustCefBrowser::ExecuteBrowserRequestAsync(BrowserRequest request)
{
    const std::string expression = BuildBrowserRequestExpression(request);
    const nlohmann::json devtools_request = {
        {"expression", expression},
        {"awaitPromise", true},
        {"returnByValue", true},
    };

    const auto result = co_await ExecuteDevToolsMethodAsync("Runtime.evaluate", devtools_request.dump());
    co_return ParseBrowserResponse(ParseDevToolsStringResult(result));
}

asio::awaitable<void> JustCefWindow::SetTitleAsync(std::string title)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetTitleAsync, Identifier(), std::move(title));
}

asio::awaitable<void> JustCefWindow::SetIconAsync(std::string icon_path)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetIconAsync, Identifier(), std::move(icon_path));
}

asio::awaitable<void> JustCefBrowser::AddUrlToProxyAsync(std::string url)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowAddUrlToProxyAsync, Identifier(), std::move(url));
}

asio::awaitable<void> JustCefBrowser::RemoveUrlToProxyAsync(std::string url)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowRemoveUrlToProxyAsync, Identifier(), std::move(url));
}

asio::awaitable<void> JustCefBrowser::AddDomainToProxyAsync(std::string domain)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowAddDomainToProxyAsync, Identifier(), std::move(domain));
}

asio::awaitable<void> JustCefBrowser::RemoveDomainToProxyAsync(std::string domain)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowRemoveDomainToProxyAsync, Identifier(), std::move(domain));
}

asio::awaitable<void> JustCefBrowser::AddUrlToModifyAsync(std::string url)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowAddUrlToModifyAsync, Identifier(), std::move(url));
}

asio::awaitable<void> JustCefBrowser::RemoveUrlToModifyAsync(std::string url)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowRemoveUrlToModifyAsync, Identifier(), std::move(url));
}

asio::awaitable<void> JustCefBrowser::AddDevToolsEventMethod(std::string method)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowAddDevToolsEventMethod, Identifier(), std::move(method));
}

asio::awaitable<void> JustCefBrowser::RemoveDevToolsEventMethod(std::string method)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowRemoveDevToolsEventMethod, Identifier(), std::move(method));
}

asio::awaitable<void> JustCefWindow::CenterSelfAsync()
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowCenterSelfAsync, Identifier());
}

asio::awaitable<void> JustCefBrowser::SetProxyRequestsAsync(bool proxy_requests)
{
    if (proxy_requests)
    {
        std::lock_guard<std::mutex> lock(shared_->request_mutex);
        if (!shared_->request_proxy)
        {
            throw std::invalid_argument("When proxy_requests is true, request_proxy must be set.");
        }
    }

    return CallProcess(command_target_, &WindowCommandTarget::WindowSetProxyRequestsAsync, Identifier(), proxy_requests);
}

asio::awaitable<void> JustCefBrowser::SetModifyRequestsAsync(bool modify_requests, bool modify_body)
{
    return CallProcess(command_target_, &WindowCommandTarget::WindowSetModifyRequestsAsync, Identifier(), modify_requests, modify_body);
}

void JustCefWindow::SetRequestProxy(RequestProxy request_proxy)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = BindToTarget<BoundRequestProxy>(*this, std::move(request_proxy));
}

void JustCefWindow::SetRequestProxy(SyncRequestProxy request_proxy)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = BindSyncToTarget<BoundRequestProxy>(*this, std::move(request_proxy), MakeReadyProxyAwaitable);
}

void JustCefWindow::SetBridgeRpcHandler(BridgeRpcHandler bridge_rpc_handler)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->bridge_rpc_handler = std::move(bridge_rpc_handler);
}

void JustCefWindow::SetBridgeRpcHandler(SyncBridgeRpcHandler bridge_rpc_handler)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    if (!bridge_rpc_handler)
    {
        shared_->bridge_rpc_handler = {};
        return;
    }

    shared_->bridge_rpc_handler = [handler = std::move(bridge_rpc_handler)](JustCefWindow& window, std::string method, std::string json) mutable
    {
        return MakeReadyBridgeAwaitable(handler(window, std::move(method), std::move(json)));
    };
}

void JustCefWindow::SetRequestModifier(RequestModifier request_modifier)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = BindToTarget<BoundRequestModifier>(*this, std::move(request_modifier));
}

void JustCefWindow::SetRequestModifier(SyncRequestModifier request_modifier)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = BindSyncToTarget<BoundRequestModifier>(*this, std::move(request_modifier), MakeReadyModifierAwaitable);
}

void JustCefWindow::SetViewCreatedHandler(ViewCreatedHandler view_created_handler)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->view_created_handler = std::move(view_created_handler);
}

void JustCefWindow::SetViewCreatedHandler(SyncViewCreatedHandler view_created_handler)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    if (!view_created_handler)
    {
        shared_->view_created_handler = {};
        return;
    }

    shared_->view_created_handler = [handler = std::move(view_created_handler)](JustCefView& view) mutable
    {
        handler(view);
        return MakeReadyViewCreatedAwaitable();
    };
}

std::vector<std::shared_ptr<JustCefView>> JustCefWindow::Views() const
{
    return RequireProcess(command_target_)->WindowViews(Identifier());
}

std::shared_ptr<JustCefWindow> JustCefView::Parent() const
{
    return parent_.lock();
}

void JustCefView::SetRequestProxy(ViewRequestProxy request_proxy)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = BindToTarget<BoundRequestProxy>(*this, std::move(request_proxy));
}

void JustCefView::SetRequestProxy(SyncViewRequestProxy request_proxy)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_proxy = BindSyncToTarget<BoundRequestProxy>(*this, std::move(request_proxy), MakeReadyProxyAwaitable);
}

void JustCefView::SetRequestModifier(ViewRequestModifier request_modifier)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = BindToTarget<BoundRequestModifier>(*this, std::move(request_modifier));
}

void JustCefView::SetRequestModifier(SyncViewRequestModifier request_modifier)
{
    std::lock_guard<std::mutex> lock(shared_->request_mutex);
    shared_->request_modifier = BindSyncToTarget<BoundRequestModifier>(*this, std::move(request_modifier), MakeReadyModifierAwaitable);
}

bool JustCefBrowser::IsLoading() const
{
    return shared_->loading.IsLoading();
}

bool JustCefBrowser::CanGoBack() const
{
    return shared_->loading.CanGoBack();
}

bool JustCefBrowser::CanGoForward() const
{
    return shared_->loading.CanGoForward();
}

void JustCefBrowser::WaitUntilLoaded() const
{
    if (auto idle = shared_->loading.IdleSignal())
    {
        idle->Wait();
    }
}

asio::awaitable<void> JustCefBrowser::WaitUntilLoadedAsync() const
{
    if (auto idle = shared_->loading.IdleSignal())
    {
        co_await idle->AsyncWait(shared_->executor);
    }
}

void JustCefBrowser::WaitForExit() const
{
    shared_->close_signal.Wait();
}

asio::awaitable<void> JustCefBrowser::WaitForExitAsync() const
{
    co_await shared_->close_signal.AsyncWait(shared_->executor);
}

} // namespace justcef
