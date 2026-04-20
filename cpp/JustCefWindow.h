#pragma once

#include "Event.h"
#include "IpcTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace justcef {

class JustCefProcessImpl;
class WindowCommandTarget;
struct WindowShared;
class JustCefWindow;

using BridgeRpcHandler = std::function<asio::awaitable<std::optional<std::string>>(
    JustCefWindow&,
    std::string,
    std::string)>;
using SyncBridgeRpcHandler = std::function<std::optional<std::string>(
    JustCefWindow&,
    std::string,
    std::string)>;

struct FrameLoadStartInfo {
    std::optional<std::string> frame_identifier;
    bool is_main_frame = false;
    std::optional<std::string> url;
};

struct FrameLoadEndInfo {
    std::optional<std::string> frame_identifier;
    bool is_main_frame = false;
    std::optional<std::string> url;
    int http_status_code = 0;
};

struct FrameLoadErrorInfo {
    std::optional<std::string> frame_identifier;
    bool is_main_frame = false;
    int error_code = 0;
    std::optional<std::string> error_text;
    std::optional<std::string> failed_url;
};

struct LoadingStateChangedInfo {
    bool is_loading = false;
    bool can_go_back = false;
    bool can_go_forward = false;
};

class JustCefWindow {
public:
    Event<> OnClose;
    Event<> OnFocused;
    Event<> OnUnfocused;
    Event<bool> OnFullscreenChanged;

    Event<FrameLoadStartInfo> OnFrameLoadStart;
    Event<FrameLoadEndInfo> OnFrameLoadEnd;
    Event<FrameLoadErrorInfo> OnFrameLoadError;
    Event<LoadingStateChangedInfo> OnLoadingStateChanged;

    Event<std::optional<std::string>, std::vector<std::uint8_t>> OnDevToolsEvent;

    ~JustCefWindow();

    int Identifier() const;

    asio::awaitable<void> MaximizeAsync();
    asio::awaitable<void> MinimizeAsync();
    asio::awaitable<void> RestoreAsync();
    asio::awaitable<void> ShowAsync();
    asio::awaitable<void> HideAsync();
    asio::awaitable<void> ActivateAsync();
    asio::awaitable<void> BringToTopAsync();
    asio::awaitable<void> SetAlwaysOnTopAsync(bool always_on_top);
    asio::awaitable<void> LoadUrlAsync(std::string url);
    asio::awaitable<void> NavigateAsync(std::string url);
    asio::awaitable<void> SetPositionAsync(int x, int y);
    asio::awaitable<Position> GetPositionAsync();
    asio::awaitable<void> SetSizeAsync(int width, int height);
    asio::awaitable<Size> GetSizeAsync();
    asio::awaitable<void> SetZoomAsync(double zoom);
    asio::awaitable<double> GetZoomAsync();
    asio::awaitable<std::vector<std::string>> PickFileAsync(bool multiple, std::vector<FileFilter> filters);
    asio::awaitable<std::string> PickDirectoryAsync();
    asio::awaitable<std::string> SaveFileAsync(std::string default_name, std::vector<FileFilter> filters);
    asio::awaitable<void> CloseAsync(bool force_close = false);
    asio::awaitable<void> SetFullscreenAsync(bool fullscreen);
    asio::awaitable<void> RequestFocusAsync();
    asio::awaitable<void> SetDevelopmentToolsEnabledAsync(bool development_tools_enabled);
    asio::awaitable<void> SetDevelopmentToolsVisibleAsync(bool development_tools_visible);
    asio::awaitable<DevToolsMethodResult> ExecuteDevToolsMethodAsync(std::string method_name, std::optional<std::string> json = std::nullopt);
    asio::awaitable<std::string> CallBridgeRpcAsync(std::string method, std::optional<std::string> json = std::nullopt);
    asio::awaitable<BrowserResponse> ExecuteBrowserRequestAsync(BrowserRequest request);
    asio::awaitable<void> SetTitleAsync(std::string title);
    asio::awaitable<void> SetIconAsync(std::string icon_path);
    asio::awaitable<void> AddUrlToProxyAsync(std::string url);
    asio::awaitable<void> RemoveUrlToProxyAsync(std::string url);
    asio::awaitable<void> AddDomainToProxyAsync(std::string domain);
    asio::awaitable<void> RemoveDomainToProxyAsync(std::string domain);
    asio::awaitable<void> AddUrlToModifyAsync(std::string url);
    asio::awaitable<void> RemoveUrlToModifyAsync(std::string url);
    asio::awaitable<void> AddDevToolsEventMethod(std::string method);
    asio::awaitable<void> RemoveDevToolsEventMethod(std::string method);
    asio::awaitable<void> CenterSelfAsync();
    asio::awaitable<void> SetProxyRequestsAsync(bool proxy_requests);
    asio::awaitable<void> SetModifyRequestsAsync(bool modify_requests, bool modify_body);

    void SetRequestProxy(RequestProxy request_proxy);
    void SetRequestProxy(SyncRequestProxy request_proxy);
    void SetRequestModifier(RequestModifier request_modifier);
    void SetRequestModifier(SyncRequestModifier request_modifier);
    void SetBridgeRpcHandler(BridgeRpcHandler bridge_rpc_handler);
    void SetBridgeRpcHandler(SyncBridgeRpcHandler bridge_rpc_handler);

    bool IsLoading() const;
    bool CanGoBack() const;
    bool CanGoForward() const;
    void WaitUntilLoaded() const;
    asio::awaitable<void> WaitUntilLoadedAsync() const;

    void WaitForExit() const;
    asio::awaitable<void> WaitForExitAsync() const;

private:
    JustCefWindow(int identifier, std::weak_ptr<WindowCommandTarget> command_target, std::shared_ptr<WindowShared> shared);

    int identifier_ = 0;
    std::weak_ptr<WindowCommandTarget> command_target_;
    std::shared_ptr<WindowShared> shared_;

    friend class JustCefProcessImpl;
};

}  // namespace justcef
