#pragma once

#include "AsyncSignal.h"
#include "IpcTypes.h"
#include "JustCefWindow.h"
#include <asio.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace justcef
{

using BoundRequestModifier = std::function<asio::awaitable<std::optional<IPCRequest>>(const IPCRequest&)>;
using BoundRequestProxy = std::function<asio::awaitable<std::optional<IPCResponse>>(const IPCRequest&)>;

class WindowCommandTarget
{
public:
    virtual ~WindowCommandTarget() = default;

    virtual asio::awaitable<void> WindowMaximizeAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowMinimizeAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowRestoreAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowShowAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowHideAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowActivateAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowBringToTopAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetAlwaysOnTopAsync(int identifier, bool always_on_top) = 0;
    virtual asio::awaitable<void> WindowLoadUrlAsync(int identifier, std::string url) = 0;
    virtual asio::awaitable<void> WindowSetPositionAsync(int identifier, int x, int y) = 0;
    virtual asio::awaitable<Position> WindowGetPositionAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetSizeAsync(int identifier, int width, int height) = 0;
    virtual asio::awaitable<Size> WindowGetSizeAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetZoomAsync(int identifier, double zoom) = 0;
    virtual asio::awaitable<double> WindowGetZoomAsync(int identifier) = 0;
    virtual asio::awaitable<std::vector<std::string>> WindowPickFileAsync(int identifier, bool multiple, std::vector<FileFilter> filters) = 0;
    virtual asio::awaitable<std::string> WindowPickDirectoryAsync(int identifier) = 0;
    virtual asio::awaitable<std::string> WindowSaveFileAsync(int identifier, std::string default_name, std::vector<FileFilter> filters) = 0;
    virtual asio::awaitable<void> WindowCloseAsync(int identifier, bool force_close) = 0;
    virtual asio::awaitable<void> WindowSetFullscreenAsync(int identifier, bool fullscreen) = 0;
    virtual asio::awaitable<void> RequestFocusAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetDevelopmentToolsEnabledAsync(int identifier, bool enabled) = 0;
    virtual asio::awaitable<void> WindowSetDevelopmentToolsVisibleAsync(int identifier, bool visible) = 0;
    virtual asio::awaitable<DevToolsMethodResult> WindowExecuteDevToolsMethodAsync(int identifier, std::string method_name, std::optional<std::string> json) = 0;
    virtual asio::awaitable<std::string> WindowBridgeRpcAsync(int identifier, std::string method, std::optional<std::string> json) = 0;
    virtual asio::awaitable<void> WindowSetTitleAsync(int identifier, std::string title) = 0;
    virtual asio::awaitable<void> WindowSetIconAsync(int identifier, std::string icon_path) = 0;
    virtual asio::awaitable<void> WindowAddUrlToProxyAsync(int identifier, std::string url) = 0;
    virtual asio::awaitable<void> WindowRemoveUrlToProxyAsync(int identifier, std::string url) = 0;
    virtual asio::awaitable<void> WindowAddDomainToProxyAsync(int identifier, std::string domain) = 0;
    virtual asio::awaitable<void> WindowRemoveDomainToProxyAsync(int identifier, std::string domain) = 0;
    virtual asio::awaitable<void> WindowAddUrlToModifyAsync(int identifier, std::string url) = 0;
    virtual asio::awaitable<void> WindowRemoveUrlToModifyAsync(int identifier, std::string url) = 0;
    virtual asio::awaitable<void> WindowAddDevToolsEventMethod(int identifier, std::string method) = 0;
    virtual asio::awaitable<void> WindowRemoveDevToolsEventMethod(int identifier, std::string method) = 0;
    virtual asio::awaitable<void> WindowCenterSelfAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetProxyRequestsAsync(int identifier, bool enable_proxy_requests) = 0;
    virtual asio::awaitable<void> WindowSetModifyRequestsAsync(int identifier, bool enable_modify_requests, bool enable_modify_body) = 0;
    virtual std::vector<std::shared_ptr<JustCefView>> WindowViews(int identifier) const = 0;
};

class LoadingState
{
public:
    void Apply(bool is_loading, bool can_go_back, bool can_go_forward);
    void OnMainFrameLoadError(int error_code, const std::string& error_text);
    void Close();

    std::shared_ptr<detail::AsyncSignal> ArmNavigation();
    void FailNavigation(const std::shared_ptr<detail::AsyncSignal>& navigation, std::exception_ptr exception);
    std::shared_ptr<detail::AsyncSignal> IdleSignal();

    bool IsLoading() const;
    bool CanGoBack() const;
    bool CanGoForward() const;

private:
    mutable std::mutex mutex_;
    bool is_loading_ = false;
    bool can_go_back_ = false;
    bool can_go_forward_ = false;
    bool closed_ = false;
    std::shared_ptr<detail::AsyncSignal> navigation_;
    bool navigation_saw_loading_ = false;
    std::shared_ptr<detail::AsyncSignal> idle_;
};

struct WindowShared
{
    asio::any_io_executor executor;
    std::mutex request_mutex;
    BoundRequestModifier request_modifier;
    BoundRequestProxy request_proxy;
    BridgeRpcHandler bridge_rpc_handler;
    ViewCreatedHandler view_created_handler;
    detail::AsyncSignal close_signal;
    std::atomic<bool> close_signaled = false;

    LoadingState loading;
};

} // namespace justcef
