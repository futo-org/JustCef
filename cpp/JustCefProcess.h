#pragma once

#include "asio.h"
#include "JustCefWindow.h"
#include "JustCefLogger.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace justcef {

class JustCefProcessImpl;

struct StartOptions {
    std::string arguments;
    std::optional<std::filesystem::path> native_executable_path;
    std::optional<std::filesystem::path> working_directory;
};

struct WindowCreateOptions {
    std::string url;
    int minimum_width = 0;
    int minimum_height = 0;
    int preferred_width = 0;
    int preferred_height = 0;
    bool fullscreen = false;
    bool context_menu_enable = false;
    bool shown = true;
    bool developer_tools_enabled = false;
    bool resizable = true;
    bool frameless = false;
    bool centered = true;
    bool proxy_requests = false;
    bool log_console = false;
    RequestProxy request_proxy;
    bool modify_requests = false;
    RequestModifier request_modifier;
    bool modify_request_body = false;
    std::optional<std::string> title;
    std::optional<std::string> icon_path;
    std::optional<std::string> app_id;
    bool bridge_enabled = false;
    BridgeRpcHandler bridge_rpc_handler;
};

class JustCefProcess {
public:
    JustCefProcess();
    explicit JustCefProcess(asio::any_io_executor executor);
    ~JustCefProcess();

    JustCefProcess(const JustCefProcess&) = delete;
    JustCefProcess& operator=(const JustCefProcess&) = delete;
    JustCefProcess(JustCefProcess&&) = delete;
    JustCefProcess& operator=(JustCefProcess&&) = delete;

    void Start(const std::string& args = {});
    void Start(const StartOptions& options);

    bool HasExited() const;
    std::vector<std::shared_ptr<JustCefWindow>> Windows() const;
    std::shared_ptr<JustCefWindow> GetWindow(int identifier) const;

    void WaitForExit() const;
    asio::awaitable<void> WaitForExitAsync() const;
    void WaitForReady() const;
    asio::awaitable<void> WaitForReadyAsync() const;

    asio::awaitable<void> EchoAsync(std::vector<std::uint8_t> data);
    asio::awaitable<void> PingAsync();
    asio::awaitable<void> PrintAsync(std::string message);

    asio::awaitable<std::shared_ptr<JustCefWindow>> CreateWindowAsync(const WindowCreateOptions& options);
    asio::awaitable<std::shared_ptr<JustCefWindow>> CreateWindowAsync(
        std::string url,
        int minimum_width,
        int minimum_height,
        int preferred_width = 0,
        int preferred_height = 0,
        bool fullscreen = false,
        bool context_menu_enable = false,
        bool shown = true,
        bool developer_tools_enabled = false,
        bool resizable = true,
        bool frameless = false,
        bool centered = true,
        bool proxy_requests = false,
        bool log_console = false,
        RequestProxy request_proxy = {},
        bool modify_requests = false,
        RequestModifier request_modifier = {},
        bool modify_request_body = false,
        std::optional<std::string> title = std::nullopt,
        std::optional<std::string> icon_path = std::nullopt,
        std::optional<std::string> app_id = std::nullopt,
        bool bridge_enabled = false,
        BridgeRpcHandler bridge_rpc_handler = {});

    asio::awaitable<void> NotifyExitAsync();

    asio::awaitable<void> StreamOpenAsync(std::uint32_t identifier);
    asio::awaitable<bool> StreamDataAsync(std::uint32_t identifier, std::vector<std::uint8_t> data);
    asio::awaitable<void> StreamCloseAsync(std::uint32_t identifier);

    // Legacy helpers. Prefer the window-scoped picker methods.
    asio::awaitable<std::vector<std::string>> PickFileAsync(bool multiple, std::vector<FileFilter> filters);
    asio::awaitable<std::string> PickDirectoryAsync();
    asio::awaitable<std::string> SaveFileAsync(std::string default_name, std::vector<FileFilter> filters);

    void Dispose();

    static std::vector<std::filesystem::path> GenerateSearchPaths();
    static std::filesystem::path ResolveNativeExecutablePath(
        const std::optional<std::filesystem::path>& native_executable_path = std::nullopt);

private:
    std::shared_ptr<JustCefProcessImpl> impl_;
};

}  // namespace justcef
