#pragma once

#include "AsyncSignal.h"
#include "IpcTypes.h"
#include "asio.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace justcef {

class WindowCommandTarget {
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
    virtual asio::awaitable<void> WindowCloseAsync(int identifier, bool force_close) = 0;
    virtual asio::awaitable<void> WindowSetFullscreenAsync(int identifier, bool fullscreen) = 0;
    virtual asio::awaitable<void> RequestFocusAsync(int identifier) = 0;
    virtual asio::awaitable<void> WindowSetDevelopmentToolsEnabledAsync(int identifier, bool enabled) = 0;
    virtual asio::awaitable<void> WindowSetDevelopmentToolsVisibleAsync(int identifier, bool visible) = 0;
    virtual asio::awaitable<DevToolsMethodResult> WindowExecuteDevToolsMethodAsync(
        int identifier,
        std::string method_name,
        std::optional<std::string> json) = 0;
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
};

struct WindowShared {
    asio::any_io_executor executor;
    std::mutex request_mutex;
    RequestModifier request_modifier;
    RequestProxy request_proxy;
    detail::AsyncSignal close_signal;
    std::atomic<bool> close_signaled = false;
};

}  // namespace justcef
