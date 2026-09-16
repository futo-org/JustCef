#include "client.h"

#include "bridge.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include "client_manager.h"
#include "client_util.h"
#include "devtoolsclient.h"
#include "ipc.h"
#include "justcef_view_common.h"
#include "justcef_view_host.h"
#include "stb_image.h"
#include "steam.h"

#include <cctype>
#include <cstring>

#ifdef _WIN32
typedef HRESULT(WINAPI* DwmSetWindowAttributeProc)(HWND, DWORD, LPCVOID, DWORD);

typedef struct _MARGINS
{
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
} MARGINS, *PMARGINS;
typedef HRESULT(WINAPI* DwmExtendFrameIntoClientAreaProc)(HWND, const MARGINS*);

const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;
const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

bool IsWindows10OrGreater(int build = -1)
{
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = 10;
    osvi.dwBuildNumber = build;

    DWORDLONG const dwlConditionMask =
        VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_BUILDNUMBER, build == -1 ? VER_EQUAL : VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | (build == -1 ? 0 : VER_BUILDNUMBER), dwlConditionMask);
}

bool UseImmersiveDarkMode(HWND hwnd, bool enabled)
{
    static HMODULE hDwmapi = LoadLibraryW(L"dwmapi.dll");
    static DwmSetWindowAttributeProc DwmSetWindowAttribute = nullptr;
    static DwmExtendFrameIntoClientAreaProc DwmExtendFrameIntoClientArea = nullptr;

    if (IsWindows10OrGreater(17763))
    {
        if (hDwmapi)
        {
            if (!DwmSetWindowAttribute)
            {
                DwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeProc>(GetProcAddress(hDwmapi, "DwmSetWindowAttribute"));
            }

            if (!DwmExtendFrameIntoClientArea)
            {
                DwmExtendFrameIntoClientArea = reinterpret_cast<DwmExtendFrameIntoClientAreaProc>(GetProcAddress(hDwmapi, "DwmExtendFrameIntoClientArea"));
            }

            if (DwmSetWindowAttribute)
            {
                DWORD attribute = DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;
                if (IsWindows10OrGreater(18985))
                {
                    attribute = DWMWA_USE_IMMERSIVE_DARK_MODE;
                }

                int useImmersiveDarkMode = enabled ? 1 : 0;
                HRESULT result = DwmSetWindowAttribute(hwnd, attribute, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode));
                LOG(INFO) << "DwmSetWindowAttribute result: " << (int)result;

                if (SUCCEEDED(result))
                {
                    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    if (DwmExtendFrameIntoClientArea)
                    {
                        MARGINS margins = {-1};
                        result = DwmExtendFrameIntoClientArea(hwnd, &margins);
                        LOG(INFO) << "DwmExtendFrameIntoClientArea result: " << (int)result;
                    }
                    else
                    {
                        LOG(WARNING) << "Windows failed to find DwmExtendFrameIntoClientArea in 'dwmapi.dll'.";
                    }
                }

                return SUCCEEDED(result);
            }
            else
            {
                LOG(WARNING) << "Windows failed to find DwmSetWindowAttribute in 'dwmapi.dll'.";
            }
        }
        else
        {
            LOG(WARNING) << "Windows failed to load 'dwmapi.dll'.";
        }
    }
    else
    {
        LOG(WARNING) << "Windows build not high enough for immersive dark mode feature.";
    }

    return false;
}

struct WindowData
{
    int identifier;
    WNDPROC originalWndProc;
};

std::map<HWND, WindowData> hwndMap;

static LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto it = hwndMap.find(hwnd);
    if (it != hwndMap.end())
    {
        if (uMsg == WM_CLOSE || uMsg == WM_DESTROY)
        {
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)it->second.originalWndProc);
            hwndMap.erase(it);
            LOG(INFO) << "Unhooked window procedure for identifier: " << it->second.identifier;
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(it->second.identifier);
        if (!browser)
        {
            LOG(ERROR) << "WindowProcHook called while CefBrowser is already closed. Ignored.";
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
        Client* pClient = (Client*)client.get();
        if (!pClient)
        {
            LOG(ERROR) << "WindowProcHook client is null. Ignored.";
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        if (uMsg == WM_GETMINMAXINFO)
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = pClient->settings.minimumWidth;
            mmi->ptMinTrackSize.y = pClient->settings.minimumHeight;
        }
        else if (uMsg == WM_SETTINGCHANGE)
        {
            if (wParam == SPI_SETCLIENTAREAANIMATION)
            {
                UseImmersiveDarkMode(hwnd, true);
            }
        }

        return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

#endif

inline bool StartsWith(const std::string& str, const std::string& prefix)
{
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

inline bool EndsWith(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void QueueClientBridgeRpcResponse(ipc::Reply reply, bool success, const std::string& result_json, const std::string& error)
{
    PacketWriter writer;
    if (!success)
        reply.Fail(ipc::StatusCode::Error, error);
    else if (!writer.write<uint32_t>(static_cast<uint32_t>(result_json.size())) || !writer.writeBytes(reinterpret_cast<const uint8_t*>(result_json.data()), result_json.size()))
        reply.Fail(ipc::StatusCode::TooLarge, "The bridge RPC result is too large.");
    else
        reply.Ok(writer.data(), writer.size());
}

std::string ExtractHostFromURL(const std::string& url)
{
    // Find scheme
    size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos)
    {
        LOG(ERROR) << "URL without scheme: " << url;
        return CefString();
    }
    size_t after_scheme = scheme_pos + 3;

    // Find end of authority
    size_t authority_end = url.find_first_of("/?#", after_scheme);
    if (authority_end == std::string::npos)
    {
        authority_end = url.length();
    }

    // Extract authority and find last '@' to handle userinfo
    std::string authority = url.substr(after_scheme, authority_end - after_scheme);
    size_t at_pos = authority.rfind('@');
    size_t host_start = (at_pos != std::string::npos) ? (after_scheme + at_pos + 1) : after_scheme;

    // Extract host
    size_t end;
    if (url[host_start] == '[')
    { // IPv6
        end = url.find(']', host_start + 1);
        if (end == std::string::npos || end > authority_end)
        {
            LOG(ERROR) << "Invalid IPv6 URL: " << url;
            return CefString();
        }
        end++; // Include ']'
    }
    else
    {
        end = url.find_first_of(":/?#", host_start);
        if (end == std::string::npos || end > authority_end)
        {
            end = authority_end;
        }
    }

    return url.substr(host_start, end - host_start);
}

bool MatchesDomain(const std::string& request_host, const std::string& cookie_domain)
{
    if (cookie_domain.size() < 2 || cookie_domain[0] != '.')
    {
        return false; // Invalid cookie domain
    }
    size_t norm_size = cookie_domain.size() - 1; // Length without the leading dot

    // Exact match: request_host equals cookie_domain without the leading dot
    if (request_host.size() == norm_size && request_host.compare(0, norm_size, cookie_domain, 1, norm_size) == 0)
    {
        return true;
    }

    // Subdomain match: request_host ends with '.' + cookie_domain without leading dot
    if (request_host.size() > norm_size + 1 && request_host[request_host.size() - norm_size - 1] == '.' &&
        request_host.compare(request_host.size() - norm_size, norm_size, cookie_domain, 1, norm_size) == 0)
    {
        return true;
    }
    return false;
}

Client::Client(const IPCWindowCreate& settings) : settings(settings)
{
    _proxyRequests = settings.proxyRequests;
    _modifyRequests = settings.modifyRequests;
    _modifyRequestBody = settings.modifyRequestBody;
}

void Client::SetProxyRequests(bool proxyRequests)
{
    settings.proxyRequests = proxyRequests;
    _proxyRequests = proxyRequests;
}

void Client::SetModifyRequests(bool modifyRequests, bool modifyRequestBody)
{
    settings.modifyRequests = modifyRequests;
    settings.modifyRequestBody = modifyRequestBody;
    _modifyRequests = modifyRequests;
    _modifyRequestBody = modifyRequestBody;
}

void Client::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title)
{
    CEF_REQUIRE_UI_THREAD();

    if (_titleOverride.size() > 0)
        SetTitle(browser, _titleOverride);
    else
        SetTitle(browser, title.ToString());
}

void Client::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
    bool isViewsEnabled = CefBrowserView::GetForBrowser(browser) ? true : false;
    if (!isViewsEnabled)
        shared::PlatformSetFullscreen(browser, fullscreen);

    IPC::Singleton.NotifyWindowFullscreenChanged(browser, fullscreen);
}

bool Client::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id, const CefString& target_url, const CefString& target_frame_name,
                           CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture, const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                           CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings, CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access)
{
    extra_info = CreateBridgeExtraInfo(false, extra_info);
    return false;
}

void Client::OnBeforeDevToolsPopup(CefRefPtr<CefBrowser> browser, CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings,
                                   CefRefPtr<CefDictionaryValue>& extra_info, bool* use_default_window)
{
    extra_info = CreateBridgeExtraInfo(false, extra_info);
}

void Client::TrackBrowser(CefRefPtr<CefBrowser> browser)
{
    _identifier = browser->GetIdentifier();
    if (_primaryIdentifier == 0)
        _primaryIdentifier = _identifier;

    // Add to the list of existing browsers.
    ClientManager::GetInstance()->OnAfterCreated(browser);
    LOG(INFO) << "Browser opened " << browser->GetIdentifier();
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    TrackBrowser(browser);

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    CefRefPtr<CefWindow> window = browser_view ? browser_view->GetWindow() : nullptr;
    if (window)
    {
        window->SetFullscreen(settings.fullscreen);
        if (settings.centered && settings.shown)
        {
            window->CenterWindow(window->GetSize());
        }

        if (settings.shown)
        {
            window->Show();
            window->RequestFocus();
        }
        else
        {
            window->Hide();
        }
    }
    else if (!browser_view)
    {
        if (settings.shown)
        {
            shared::PlatformShow(browser);
            shared::PlatformSetFullscreen(browser, settings.fullscreen);
            shared::PlatformSetFrameless(browser, settings.frameless);
            shared::PlatformSetResizable(browser, settings.resizable);
            if (settings.centered)
            {
                shared::PlatformCenterWindow(browser, shared::PlatformGetWindowSize(browser));
            }
            shared::PlatformWindowRequestFocus(browser);
            shared::PlatformSetMinimumWindowSize(browser, settings.minimumWidth, settings.minimumHeight);
        }
        else
        {
            shared::PlatformHide(browser);
        }

#ifdef _WIN32
        HWND hwnd = browser->GetHost()->GetWindowHandle();
        WindowData data;
        data.identifier = _identifier;
        data.originalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)WindowProcHook);
        hwndMap[hwnd] = data;

        UseImmersiveDarkMode(hwnd, true);
        // SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif
    }

    if (settings.title)
        OverrideTitle(browser, *settings.title);

    if (settings.iconPath)
        OverrideIcon(browser, *settings.iconPath);

    IPC::Singleton.NotifyWindowOpened(browser);
}

bool Client::DoClose(CefRefPtr<CefBrowser> browser)
{
    LOG(INFO) << "DoClose called " << browser->GetIdentifier();

    CEF_REQUIRE_UI_THREAD();
    shared::CancelPendingFileDialogs(browser->GetIdentifier());

    // Closing the main window requires special handling. See the DoClose()
    // documentation in the CEF header for a detailed destription of this
    // process.
    ClientManager::GetInstance()->DoClose(browser);

    // Allow the close. For windowed browsers this will result in the OS close
    // event being sent.
    LOG(INFO) << "DoClose finished " << browser->GetIdentifier();
    return false;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    LOG(INFO) << "OnBeforeClose called " << browser->GetIdentifier();

    CEF_REQUIRE_UI_THREAD();
    shared::CancelPendingFileDialogs(browser->GetIdentifier());

#if _WIN32
    HWND hwnd = browser->GetHost()->GetWindowHandle();
    auto it = hwndMap.find(hwnd);
    if (it != hwndMap.end())
    {
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)it->second.originalWndProc);
        hwndMap.erase(it);
        LOG(INFO) << "Unhooked window procedure for identifier: " << it->second.identifier;
    }
#endif

    if (IsPrimaryBrowser(browser))
    {
        auto devToolsMethodCallbacks = std::move(_devToolsMethodCallbacks);
        _devToolsMethodCallbacks.clear();
        for (auto& itr : devToolsMethodCallbacks)
            itr.second(false, std::string());
        FailAllBridgeRpcCalls("Bridge RPC failed because the browser is closing.");
        CancelHostCalls();
        CancelPendingModifies();
        _devToolsRegistration = nullptr;
        _identifier = 0;
    }

    justcef_view::OnHostBrowserGone(browser);

    // Remove from the list of existing browsers.
    ClientManager::GetInstance()->OnBeforeClose(browser);

    LOG(INFO) << "Browser closed " << browser->GetIdentifier();

    IPC::Singleton.NotifyWindowClosed(browser);

    LOG(INFO) << "OnBeforeClose finished " << browser->GetIdentifier();
}

void Client::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward)
{
    IPC::Singleton.NotifyWindowLoadingStateChanged(browser, isLoading, canGoBack, canGoForward);
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
    IPC::Singleton.NotifyWindowFrameLoadEnd(browser, frame, httpStatusCode);
}

void Client::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
    IPC::Singleton.NotifyWindowFrameLoadStart(browser, frame);
}

void Client::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl)
{
    LOG(ERROR) << "Failed to load URL (" << errorCode << ") '" << failedUrl << "': " << errorText;

    IPC::Singleton.NotifyWindowFrameLoadError(browser, frame, errorCode, errorText, failedUrl);
}

void Client::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    LOG(INFO) << "Browser unfocused " << browser->GetIdentifier();

    IPC::Singleton.NotifyWindowUnfocused(browser);
}

void Client::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
    LOG(INFO) << "Browser focused " << browser->GetIdentifier();

    justcef_view::OnHostGotFocus(browser);

    IPC::Singleton.NotifyWindowFocused(browser);
}

void Client::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model)
{
    if (!settings.contextMenuEnable)
        model->Clear();
}

bool Client::OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event)
{
    if (event.type == KEYEVENT_RAWKEYDOWN)
    {
        switch (event.windows_key_code)
        {
        case 0x74: // F5
            if (settings.developerToolsEnabled)
            {
                browser->Reload();
            }
            return true;
        case 0x7B: // F12
            if (settings.developerToolsEnabled)
            {
                if (browser->GetHost()->HasDevTools())
                {
                    browser->GetHost()->CloseDevTools();
                }
                else
                {
                    CefBrowserSettings browser_settings;
                    CefWindowInfo window_info;
                    CefPoint inspect_element_at;
                    browser->GetHost()->ShowDevTools(window_info, new DevToolsClient(), browser_settings, inspect_element_at);
                }

                return true;
            }
            return false;
        case 0x7A: // F11
            CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
            if (browser_view)
            {
                CefRefPtr<CefWindow> window = browser_view->GetWindow();
                if (window)
                    window->SetFullscreen(!window->IsFullscreen());
                return true;
            }
            else
            {
                shared::PlatformSetFullscreen(browser, !shared::PlatformGetFullscreen(browser));
            }
            return false;
        }
    }
    return false;
}

bool Client::EnsureDevToolsRegistration(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    if (!_devToolsRegistration)
    {
        _devToolsRegistration = browser->GetHost()->AddDevToolsMessageObserver(this);
        if (!_devToolsRegistration)
        {
            LOG(ERROR) << "Failed to attach DevToolsMessageObserver";
            return false;
        }
        LOG(INFO) << "EnsureDevToolsRegistration new registration added (identifier = " << browser->GetIdentifier()
                  << ", _devToolsRegistration = " << (size_t)_devToolsRegistration.get() << ")";
    }

    return true;
}

bool Client::ExecuteDevToolsMethodAsync(CefRefPtr<CefBrowser> browser, const std::string& method, CefRefPtr<CefDictionaryValue> params,
                                        std::function<void(bool success, std::string result)> callback)
{
    CEF_REQUIRE_UI_THREAD();

    if (!EnsureDevToolsRegistration(browser))
        return false;

    int messageId = ++_messageIdGenerator;
    _devToolsMethodCallbacks[messageId] = std::move(callback);
    if (browser->GetHost()->ExecuteDevToolsMethod(messageId, method, params) == 0)
    {
        _devToolsMethodCallbacks.erase(messageId);
        return false;
    }

    return true;
}

void Client::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id, bool success, const void* result, size_t result_size)
{
    LOG(INFO) << "OnDevToolsMethodResult (identifier = " << browser->GetIdentifier() << ", message_id = " << message_id << ", success = " << success << ")";

    CEF_REQUIRE_UI_THREAD();

    auto callbackItr = _devToolsMethodCallbacks.find(message_id);
    if (callbackItr != _devToolsMethodCallbacks.end())
    {
        auto callback = std::move(callbackItr->second);
        _devToolsMethodCallbacks.erase(callbackItr);
        callback(success, std::string(static_cast<const char*>(result), result_size));
    }
}

void Client::OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void* params, size_t params_size)
{
    LOG(INFO) << "OnDevToolsEvent (identifier = " << browser->GetIdentifier() << ", method = " << method << ")";

    {
        std::lock_guard<std::mutex> lk(_devToolsEventMethodsSetMutex);
        if (_devToolsEventMethodsSet.find(method) == _devToolsEventMethodsSet.end())
        {
            return;
        }
    }

    IPC::Singleton.NotifyWindowDevToolsEvent(browser, method, static_cast<const uint8_t*>(params), params_size);
}


static bool FindHeaderCI(const std::multimap<std::string, std::string>& headers, const char* name, std::string& out)
{
    for (const auto& [k, v] : headers)
    {
        if (k.size() != std::strlen(name))
            continue;
        bool eq = true;
        for (std::size_t i = 0; i < k.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(k[i])) != std::tolower(static_cast<unsigned char>(name[i])))
            {
                eq = false;
                break;
            }
        }
        if (eq)
        {
            out = v;
            return true;
        }
    }
    return false;
}

static bool ParseU64(const std::string& s, std::size_t b, std::size_t e, int64_t& out)
{
    if (b >= e)
        return false;
    int64_t v = 0;
    for (std::size_t i = b; i < e; ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

static bool ParseContentRange(const std::string& value, int64_t& start, int64_t& end, int64_t& total)
{
    std::size_t p = value.find("bytes");
    if (p == std::string::npos)
        return false;
    p += 5;
    while (p < value.size() && value[p] == ' ')
        ++p;
    const std::size_t dash = value.find('-', p);
    if (dash == std::string::npos || dash <= p)
        return false;
    const std::size_t slash = value.find('/', dash);
    if (slash == std::string::npos || slash <= dash + 1)
        return false;
    if (!ParseU64(value, p, dash, start) || !ParseU64(value, dash + 1, slash, end))
        return false;
    const std::string totalStr = value.substr(slash + 1);
    if (totalStr == "*")
        total = -1;
    else if (!ParseU64(totalStr, 0, totalStr.size(), total))
        return false;
    if (start < 0 || end < start)
        return false;
    return true;
}

class ProxyResourceHandler : public CefResourceHandler
{
public:
    ProxyResourceHandler(int32_t identifier, CefRefPtr<CefRequest> request, std::chrono::milliseconds openTimeout)
        : _identifier(identifier), _request(request), _offset(0), _openTimeout(openTimeout)
    {
    }

    bool Open(CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) override
    {
        handle_request = false;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _openCb = callback;
        }

        CefRefPtr<ProxyResourceHandler> self(this);
        ipc::CallHandle call = IPC::Singleton.WindowProxyRequest(_identifier, request, _openTimeout,
                                                                 [self](ipc::StatusCode status, std::unique_ptr<IPCProxyResponse> response)
                                                                 {
                                                                     auto shared = std::make_shared<std::unique_ptr<IPCProxyResponse>>(std::move(response));
                                                                     CefPostTask(TID_IO, base::BindOnce(&ProxyResourceHandler::OnResponseOnIo, self, status, shared));
                                                                 });

        bool canceled = false;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _call = call;
            canceled = _canceled;
        }
        if (canceled)
            call.Cancel();
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length, CefString& redirectUrl) override
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!_response)
            return;

        if (_response->media_type)
            response->SetMimeType(*_response->media_type);

        const bool hasRange = (_entityTotal >= 0);

        response->SetStatus(_response->status_code);
        response->SetStatusText(_response->status_text);

        CefResponse::HeaderMap headerMap;
        for (auto& header : _response->headers)
            headerMap.insert({header.first, header.second});
        response->SetHeaderMap(headerMap);

        if (hasRange)
            response_length = _entityTotal;
        else if (_response->body)
            response_length = static_cast<int64_t>((*_response->body).size());
        else
            response_length = _response->bodyLength;
    }

    bool Skip(int64_t bytes_to_skip, int64_t& bytes_skipped, CefRefPtr<CefResourceSkipCallback>) override
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!_response || _canceled || bytes_to_skip < 0)
        {
            bytes_skipped = -2;
            return false;
        }

        int64_t skipped = std::min<int64_t>(bytes_to_skip, _skipRemaining);
        _skipRemaining -= skipped;

        int64_t remaining = bytes_to_skip - skipped;
        if (remaining > 0 && _response->body)
        {
            const int64_t available = static_cast<int64_t>((*_response->body).size() - _offset);
            const int64_t count = std::min<int64_t>(remaining, available);
            _offset += static_cast<size_t>(count);
            skipped += count;
            remaining -= count;
        }
        else if (remaining > 0 && _response->bodyStream)
        {
            _discardRemaining += remaining;
            skipped += remaining;
            remaining = 0;
        }

        if (skipped == 0)
        {
            bytes_skipped = -2;
            return false;
        }

        bytes_skipped = skipped;
        return true;
    }

    bool Read(void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback) override
    {
        bytes_read = 0;

        std::shared_ptr<ipc::IncomingStream> stream;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            if (!_response || _canceled)
                return false;

            if (_response->body)
            {
                if (_offset < (*_response->body).size())
                {
                    size_t bytes_to_copy = std::min(static_cast<size_t>(bytes_to_read), (*_response->body).size() - _offset);
                    memcpy(data_out, (*_response->body).data() + _offset, bytes_to_copy);
                    _offset += bytes_to_copy;
                    bytes_read = static_cast<int>(bytes_to_copy);
                    return true;
                }
                return false;
            }

            stream = _response->bodyStream;
        }

        if (stream)
        {
            CefRefPtr<ProxyResourceHandler> self(this);
            while (true)
            {
                size_t n = 0;
                switch (PumpOnce(stream, data_out, static_cast<size_t>(bytes_to_read), n))
                {
                case PumpState::Delivered:
                    bytes_read = static_cast<int>(n);
                    return true;
                case PumpState::NeedMore:
                {
                    {
                        std::lock_guard<std::mutex> lk(_mutex);
                        if (_canceled)
                            return false;
                        _pendingData = data_out;
                        _pendingSize = static_cast<size_t>(bytes_to_read);
                        _pendingCb = callback;
                    }
                    if (stream->SetWakeup([self]() { self->PumpAsync(); }))
                        return true;

                    std::lock_guard<std::mutex> lk(_mutex);
                    _pendingCb = nullptr;
                    break;
                }
                case PumpState::Eof:
                    return false;
                case PumpState::Failed:
                    bytes_read = -2;
                    return false;
                }
            }
        }

        return false;
    }

    void Cancel() override
    {
        ipc::CallHandle call;
        std::shared_ptr<ipc::IncomingStream> stream;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _canceled = true;
            call = _call;
            _openCb = nullptr;
            _pendingCb = nullptr;
            if (_response)
                stream = _response->bodyStream;
        }

        call.Cancel();
        if (stream)
        {
            LOG(INFO) << "Canceling stream " << stream->Id() << ".";
            stream->Cancel();
        }
    }

private:
    void OnResponseOnIo(ipc::StatusCode status, std::shared_ptr<std::unique_ptr<IPCProxyResponse>> response)
    {
        OnResponse(status, std::move(*response));
    }

    void OnResponse(ipc::StatusCode status, std::unique_ptr<IPCProxyResponse> response)
    {
        CefRefPtr<CefCallback> callback;
        bool handled = false;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            callback = _openCb;
            _openCb = nullptr;
            if (_canceled)
            {
                if (response && response->bodyStream)
                    response->bodyStream->Cancel();
                return;
            }

            _response = std::move(response);
            handled = status == ipc::StatusCode::Ok && _response;
            InitRangeState();
        }

        if (!callback)
            return;

        if (handled)
        {
            callback->Continue();
            return;
        }

        if (status != ipc::StatusCode::NotHandled && status != ipc::StatusCode::Canceled)
            LOG(ERROR) << "Proxy request failed (" << ipc::StatusName(status) << ").";
        callback->Cancel();
    }

    void InitRangeState()
    {
        if (!_response)
            return;
        std::string crVal;
        int64_t crStart = 0, crEnd = 0, crTotal = -1;
        const bool hasRange =
            FindHeaderCI(_response->headers, "Content-Range", crVal) && ParseContentRange(crVal, crStart, crEnd, crTotal) && crTotal >= 0;
        if (hasRange)
        {
            _entityStart = crStart;
            _entityTotal = crTotal;
            _skipRemaining = crStart;
        }
    }

    enum class PumpState
    {
        Delivered,
        NeedMore,
        Eof,
        Failed
    };

    PumpState PumpOnce(const std::shared_ptr<ipc::IncomingStream>& stream, void* out, size_t size, size_t& n)
    {
        while (_discardRemaining > 0)
        {
            uint8_t scratch[16384];
            const size_t want = static_cast<size_t>(std::min<int64_t>(_discardRemaining, static_cast<int64_t>(sizeof(scratch))));
            size_t dropped = 0;
            const ipc::IncomingStream::ReadResult result = stream->Read(scratch, want, dropped);
            if (result == ipc::IncomingStream::ReadResult::Data)
            {
                _streamed += dropped;
                _discardRemaining -= static_cast<int64_t>(dropped);
                continue;
            }
            if (result == ipc::IncomingStream::ReadResult::Pending)
                return PumpState::NeedMore;
            if (result == ipc::IncomingStream::ReadResult::End)
            {
                CheckStreamIntegrity(stream);
                return PumpState::Eof;
            }
            return PumpState::Failed;
        }

        switch (stream->Read(reinterpret_cast<uint8_t*>(out), size, n))
        {
        case ipc::IncomingStream::ReadResult::Data:
            _streamed += n;
            return PumpState::Delivered;
        case ipc::IncomingStream::ReadResult::Pending:
            return PumpState::NeedMore;
        case ipc::IncomingStream::ReadResult::End:
            CheckStreamIntegrity(stream);
            return PumpState::Eof;
        case ipc::IncomingStream::ReadResult::Error:
            break;
        }
        return PumpState::Failed;
    }

    void PumpAsync()
    {
        CefRefPtr<ProxyResourceHandler> self(this);
        CefRefPtr<CefResourceReadCallback> cb;
        int result = 0;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            auto stream = _response ? _response->bodyStream : nullptr;
            if (_canceled || !_pendingCb || !stream)
                return;

            while (true)
            {
                size_t n = 0;
                const PumpState state = PumpOnce(stream, _pendingData, _pendingSize, n);
                if (state == PumpState::NeedMore)
                {
                    if (stream->SetWakeup([self]() { self->PumpAsync(); }))
                        return;
                    continue;
                }

                result = state == PumpState::Delivered ? static_cast<int>(n) : state == PumpState::Failed ? -2 : 0;
                break;
            }

            cb = _pendingCb;
            _pendingCb = nullptr;
        }

        cb->Continue(result);
    }

    void CheckStreamIntegrity(const std::shared_ptr<ipc::IncomingStream>& stream)
    {
        if (stream->Length() < 0)
            return;
        const uint64_t got = _streamed;
        const uint64_t want = static_cast<uint64_t>(stream->Length());
        if (got != want)
            LOG(ERROR) << "Stream " << stream->Id() << " truncated: consumed " << got << " of declared " << want << " bytes.";
    }

    int32_t _identifier;
    CefRefPtr<CefRequest> _request;
    std::unique_ptr<IPCProxyResponse> _response;
    size_t _offset;
    std::chrono::milliseconds _openTimeout;
    std::mutex _mutex;
    ipc::CallHandle _call;
    CefRefPtr<CefCallback> _openCb;
    bool _canceled = false;
    std::atomic<uint64_t> _streamed{0};
    std::atomic<int64_t> _discardRemaining{0};

    void* _pendingData = nullptr;
    size_t _pendingSize = 0;
    CefRefPtr<CefResourceReadCallback> _pendingCb;

    int64_t _entityStart = 0;
    int64_t _entityTotal = -1;
    int64_t _skipRemaining = 0;

    IMPLEMENT_REFCOUNTING(ProxyResourceHandler);
};

CefRefPtr<CefResourceHandler> Client::GetResourceHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request)
{
    if (_proxyRequests)
        return new ProxyResourceHandler(browser->GetIdentifier(), request, std::chrono::milliseconds(settings.proxyOpenTimeoutMs));

    {
        std::lock_guard<std::mutex> lk(_proxyRequestsSetMutex);
        if (_proxyRequestsSet.find(request->GetURL()) != _proxyRequestsSet.end())
            return new ProxyResourceHandler(browser->GetIdentifier(), request, std::chrono::milliseconds(settings.proxyOpenTimeoutMs));
    }

    {
        std::lock_guard<std::mutex> lk(_proxyDomainsMutex);
        bool hasProxyDomains = _exactProxyDomains.size() > 0 || _leadingDotProxyDomains.size() > 0;
        if (!hasProxyDomains)
            return nullptr;
    }

    std::string req_host = ExtractHostFromURL(request->GetURL());
    if (req_host.size() < 1)
        return nullptr;

    // Check caches first
    {
        std::lock_guard<std::mutex> lk(_proxyCacheMutex);
        if (_proxyCache.find(req_host) != _proxyCache.end())
            return new ProxyResourceHandler(browser->GetIdentifier(), request, std::chrono::milliseconds(settings.proxyOpenTimeoutMs));
        if (_negativeProxyCache.find(req_host) != _negativeProxyCache.end())
            return nullptr; // Known non-matching host
    }

    // Check exact matches and leading-dot domains
    bool matchedDomain = false;
    {
        std::lock_guard<std::mutex> lk(_proxyDomainsMutex);
        // Check exact matches
        if (_exactProxyDomains.find(req_host) != _exactProxyDomains.end())
            matchedDomain = true;
        else
        {
            // Check leading-dot domains for cookie domain matches
            for (const auto& domain : _leadingDotProxyDomains)
            {
                if (MatchesDomain(req_host, domain))
                {
                    matchedDomain = true;
                    break;
                }
            }
        }
    }

    // Cache the result
    {
        std::lock_guard<std::mutex> lk(_proxyCacheMutex);
        if (matchedDomain)
        {
            _proxyCache.insert(req_host);
        }
        else
        {
            _negativeProxyCache.insert(req_host);
        }
    }

    return matchedDomain ? new ProxyResourceHandler(browser->GetIdentifier(), request, std::chrono::milliseconds(settings.proxyOpenTimeoutMs)) : nullptr;
}

cef_return_value_t Client::OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback)
{
    int requestIdentifier = (int)request->GetIdentifier();
    bool isPending = false;
    auto modifyRequestIfNeeded = [&](const std::string& url)
    {
        bool isModified = false;
        {
            std::lock_guard<std::mutex> lock(_modifiedRequestsMutex);
            isModified = _modifiedRequests.find(requestIdentifier) != _modifiedRequests.end();
            if (!isModified)
                _modifiedRequests.insert(requestIdentifier);
        }

        if (!isModified)
        {
            const bool cancelOnTimeout = settings.modifyTimeoutPolicy == ModifyTimeoutPolicy::Cancel;
            ipc::CallHandle call = IPC::Singleton.WindowModifyRequest(browser->GetIdentifier(), request, _modifyRequestBody,
                                                                      std::chrono::milliseconds(settings.modifyTimeoutMs),
                                                                      [callback, cancelOnTimeout](ipc::StatusCode status)
                                                                      {
                                                                          if (status == ipc::StatusCode::Canceled)
                                                                              return;
                                                                          if (status == ipc::StatusCode::Timeout && cancelOnTimeout)
                                                                              callback->Cancel();
                                                                          else
                                                                              callback->Continue();
                                                                      });
            std::lock_guard<std::mutex> lock(_pendingModifiesMutex);
            _pendingModifies[requestIdentifier] = call;
            isPending = true;
        }
    };

    if (_modifyRequests)
    {
        modifyRequestIfNeeded(request->GetURL());
    }

    {
        std::lock_guard<std::mutex> lk(_modifyRequestsSetMutex);
        if (_modifyRequestsSet.find(request->GetURL()) != _modifyRequestsSet.end())
        {
            modifyRequestIfNeeded(request->GetURL());
        }
    }

    return isPending ? RV_CONTINUE_ASYNC : RV_CONTINUE;
}

void Client::OnResourceLoadComplete(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response,
                                    URLRequestStatus status, int64_t received_content_length)
{
    int requestIdentifier = (int)request->GetIdentifier();
    ipc::CallHandle pendingModify;
    {
        std::lock_guard<std::mutex> lock(_pendingModifiesMutex);
        auto it = _pendingModifies.find(requestIdentifier);
        if (it != _pendingModifies.end())
        {
            pendingModify = it->second;
            _pendingModifies.erase(it);
        }
    }
    pendingModify.Cancel();

    std::lock_guard<std::mutex> lock(_modifiedRequestsMutex);
    _modifiedRequests.erase(requestIdentifier);
}

void Client::OverrideTitle(CefRefPtr<CefBrowser> browser, const std::string& title)
{
    LOG(INFO) << "Override title: " << title;
    _titleOverride = title;
    SetTitle(browser, title);
}

void Client::SetTitle(CefRefPtr<CefBrowser> browser, const std::string& title)
{
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        if (window)
            window->SetTitle(title);
    }
    else
        shared::PlatformTitleChange(browser, title);
}

void Client::OverrideIcon(CefRefPtr<CefBrowser> browser, const std::string& iconPath)
{
    LOG(INFO) << "Override icon: " << iconPath;
    CefRefPtr<CefBrowserView> browserView = CefBrowserView::GetForBrowser(browser);
    if (browserView)
    {
        CefRefPtr<CefWindow> window = browserView->GetWindow();
        int width, height, channels;
        unsigned char* image = stbi_load(iconPath.c_str(), &width, &height, &channels, 4);
        if (!image)
        {
            return;
        }

        CefRefPtr<CefImage> cefImage = CefImage::CreateImage();
        if (cefImage)
        {
            cefImage->AddBitmap(1.0, width, height, CEF_COLOR_TYPE_RGBA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, image, width * height * 4);

            if (window)
            {
                window->SetWindowIcon(cefImage);
                window->SetWindowAppIcon(cefImage);
            }
        }

        stbi_image_free(image);
    }
    else
    {
        shared::PlatformIconChange(browser, iconPath);
    }
}

void Client::AddUrlToProxy(const std::string& url)
{
    std::lock_guard<std::mutex> lk(_proxyRequestsSetMutex);
    _proxyRequestsSet.insert(url);
}

void Client::RemoveUrlToProxy(const std::string& url)
{
    std::lock_guard<std::mutex> lk(_proxyRequestsSetMutex);
    _proxyRequestsSet.erase(url);
}

void Client::AddDomainToProxy(const std::string& domain)
{
    // Normalize for consistency
    bool is_leading_dot = StartsWith(domain, ".");
    std::string normalized_domain = is_leading_dot ? domain.substr(1) : domain;

    {
        std::lock_guard<std::mutex> lk(_proxyDomainsMutex);
        if (is_leading_dot)
        {
            _leadingDotProxyDomains.insert(domain);
            _exactProxyDomains.erase(normalized_domain); // Avoid duplication
        }
        else
        {
            _exactProxyDomains.insert(domain);
            _leadingDotProxyDomains.erase("." + domain); // Avoid duplication
        }
    }

    // Invalidate negative cache for this domain and its subdomains
    {
        std::lock_guard<std::mutex> cache_lk(_proxyCacheMutex);
        std::vector<std::string> to_remove;
        for (const auto& cached_host : _negativeProxyCache)
        {
            if (cached_host == normalized_domain || // Exact match
                (is_leading_dot && EndsWith(cached_host, "." + normalized_domain)))
            { // Subdomain match
                to_remove.push_back(cached_host);
            }
        }
        for (const auto& host : to_remove)
        {
            _negativeProxyCache.erase(host);
        }
    }
}

void Client::RemoveDomainToProxy(const std::string& domain)
{
    // Normalize for consistency
    bool is_leading_dot = StartsWith(domain, ".");
    std::string normalized_domain = is_leading_dot ? domain.substr(1) : domain;

    {
        std::lock_guard<std::mutex> lk(_proxyDomainsMutex);
        if (is_leading_dot)
        {
            _leadingDotProxyDomains.erase(domain);
        }
        else
        {
            _exactProxyDomains.erase(domain);
        }
    }

    // Invalidate positive cache for this domain and its subdomains
    {
        std::lock_guard<std::mutex> cache_lk(_proxyCacheMutex);
        std::vector<std::string> to_remove;
        for (const auto& cached_host : _proxyCache)
        {
            if (cached_host == normalized_domain || // Exact match
                (is_leading_dot && EndsWith(cached_host, "." + normalized_domain)))
            { // Subdomain match
                to_remove.push_back(cached_host);
            }
        }
        for (const auto& host : to_remove)
        {
            _proxyCache.erase(host);
        }
    }
}

void Client::AddUrlToModify(const std::string& url)
{
    std::lock_guard<std::mutex> lk(_modifyRequestsSetMutex);
    _modifyRequestsSet.insert(url);
}

void Client::RemoveUrlToModify(const std::string& url)
{
    std::lock_guard<std::mutex> lk(_modifyRequestsSetMutex);
    _modifyRequestsSet.erase(url);
}

void Client::AddDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method)
{
    EnsureDevToolsRegistration(browser);

    {
        std::lock_guard<std::mutex> lk(_devToolsEventMethodsSetMutex);
        _devToolsEventMethodsSet.insert(method);
    }
}

void Client::RemoveDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method)
{
    EnsureDevToolsRegistration(browser);

    {
        std::lock_guard<std::mutex> lk(_devToolsEventMethodsSetMutex);
        _devToolsEventMethodsSet.erase(method);
    }
}

void Client::StartBridgeRpcCall(CefRefPtr<CefBrowser> browser, const std::string& method, const std::string& payload_json, ipc::Reply reply)
{
    CEF_REQUIRE_UI_THREAD();

    if (!settings.bridgeEnabled)
    {
        QueueClientBridgeRpcResponse(std::move(reply), false, "null", "Bridge RPC is not enabled for this window.");
        return;
    }

    if (!browser)
    {
        QueueClientBridgeRpcResponse(std::move(reply), false, "null", "Bridge RPC browser is not available.");
        return;
    }

    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    if (!frame)
    {
        QueueClientBridgeRpcResponse(std::move(reply), false, "null", "Bridge RPC main frame is not available.");
        return;
    }

    const int32_t request_id = ++_bridgeRpcRequestIdGenerator;
    {
        std::lock_guard<std::mutex> lk(_bridgeRpcResultsMutex);
        _bridgeRpcResults[request_id] = std::move(reply);
    }

    SendBridgeRpcCallMessage(frame, PID_RENDERER, kBridgeRpcCallJsMessageName, request_id, method, payload_json);
}

void Client::CompleteBridgeRpcCall(int32_t request_id, bool success, const std::optional<std::string>& result_json, const std::optional<std::string>& error)
{
    ipc::Reply controller_request_id;
    {
        std::lock_guard<std::mutex> lk(_bridgeRpcResultsMutex);
        auto it = _bridgeRpcResults.find(request_id);
        if (it == _bridgeRpcResults.end())
        {
            return;
        }

        controller_request_id = std::move(it->second);
        _bridgeRpcResults.erase(it);
    }

    QueueClientBridgeRpcResponse(std::move(controller_request_id), success, result_json.value_or("null"), error.value_or(""));
}

void Client::FailAllBridgeRpcCalls(const std::string& error)
{
    std::unordered_map<int32_t, ipc::Reply> pending_calls;
    {
        std::lock_guard<std::mutex> lk(_bridgeRpcResultsMutex);
        pending_calls.swap(_bridgeRpcResults);
    }

    for (auto& entry : pending_calls)
    {
        QueueClientBridgeRpcResponse(std::move(entry.second), false, "null", error);
    }
}

void Client::CancelPendingModifies()
{
    std::unordered_map<int, ipc::CallHandle> pending;
    {
        std::lock_guard<std::mutex> lock(_pendingModifiesMutex);
        pending.swap(_pendingModifies);
    }

    for (auto& entry : pending)
        entry.second.Cancel();
}

void Client::CancelHostCalls()
{
    auto calls = std::move(_hostCalls);
    _hostCalls.clear();
    for (auto& entry : calls)
        entry.second.Cancel();
}

bool Client::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line)
{
    if (settings.logConsole)
        LOG(INFO) << "ConsoleMessage:" << level << ":" << source.ToString().c_str() << ":" << line << ": " << message.ToString().c_str();
    return true;
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    const std::string message_name = message->GetName();
    if (message_name.rfind(kViewMessagePrefix, 0) == 0)
        return justcef_view::HandleHostProcessMessage(browser, frame, message);

    if (message_name == kOskMsg)
    {
        LOG(INFO) << "OnProcessMessageReceived (name = " << message_name << ", size = " << message->GetArgumentList()->GetSize() << ").";
        const int show = message->GetArgumentList()->GetInt(0);
        if (!show)
        {
            if (justcef_view::IsFocusInView(browser))
            {
                LOG(INFO) << "Steam dismiss OSK ignored because focus is in a view.";
                return true;
            }

            LOG(INFO) << "Steam dismiss OSK.";
            Steam::Instance().DismissOsk();
            return true;
        }

        LOG(INFO) << "Steam show OSK.";

        int x = message->GetArgumentList()->GetInt(1), y = message->GetArgumentList()->GetInt(2);
        justcef_view::TranslateViewOskRect(browser, x, y);
        const int w = message->GetArgumentList()->GetInt(3), h = message->GetArgumentList()->GetInt(4);
        const auto mode = static_cast<EFloatingGamepadTextInputMode>(message->GetArgumentList()->GetInt(5));
        if (Steam::Instance().ShouldShowOsk())
        {
            LOG(INFO) << "Steam show OSK (x = " << x << ", y = " << y << ", w = " << w << ", h = " << h << ", mode = " << mode << ").";
            Steam::Instance().ShowOsk(x, y, w, h, mode);
        }
        else
        {
            LOG(INFO) << "Steam show OSK failed because should show osk is false (x = " << x << ", y = " << y << ", w = " << w << ", h = " << h << ", mode = " << mode << ").";
        }

        return true;
    }

    if (message_name == kBridgeRpcCallHostMessageName)
    {
        int32_t request_id = 0;
        std::string method;
        std::string payload_json;
        if (!ParseBridgeRpcCallMessage(message, request_id, method, payload_json))
        {
            return true;
        }

        const int32_t browser_identifier = browser ? browser->GetIdentifier() : 0;

        CefRefPtr<Client> self(this);
        _hostCalls[request_id] = IPC::Singleton.WindowBridgeRpc(
            browser_identifier, method, payload_json,
            [self, request_id, browser_identifier](IPCBridgeRpcResult result)
            {
                CefPostTask(TID_UI, base::BindOnce(
                                        [](CefRefPtr<Client> self, int32_t browser_identifier, int32_t request_id, IPCBridgeRpcResult result)
                                        {
                                            if (self->_hostCalls.erase(request_id) == 0)
                                            {
                                                return;
                                            }

                                            CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(browser_identifier);
                                            if (!browser)
                                            {
                                                return;
                                            }

                                            CefRefPtr<CefFrame> frame = browser->GetMainFrame();
                                            if (!frame)
                                            {
                                                return;
                                            }

                                            SendBridgeRpcResultMessage(frame, PID_RENDERER, kBridgeRpcCallHostResultMessageName, request_id, result.success,
                                                                       result.success ? result.result_json.value_or("null") : result.error.value_or("Bridge RPC failed."));
                                        },
                                        self, browser_identifier, request_id, result));
            });

        return true;
    }

    if (message_name == kBridgeRpcCallJsResultMessageName)
    {
        int32_t request_id = 0;
        bool success = false;
        std::string payload;
        if (!ParseBridgeRpcResultMessage(message, request_id, success, payload))
        {
            return true;
        }

        CompleteBridgeRpcCall(request_id, success, success ? std::optional<std::string>(payload) : std::optional<std::string>("null"),
                              success ? std::optional<std::string>("") : std::optional<std::string>(payload));
        return true;
    }

    if (message_name == kBridgeRpcContextReleasedMessageName)
    {
        FailAllBridgeRpcCalls("Bridge RPC failed because the JavaScript context was released.");
        CancelHostCalls();
        return true;
    }

    return false;
}

void Client::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string)
{
    LOG(ERROR) << "Render process terminated (identifier = " << browser->GetIdentifier() << ", status = " << status << ", error_code = " << error_code << ").";

    if (IsPrimaryBrowser(browser))
    {
        FailAllBridgeRpcCalls("Bridge RPC failed because the render process terminated.");
        CancelHostCalls();
    }

    justcef_view::OnHostBrowserGone(browser);
}

bool Client::OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url, JSDialogType dialog_type, const CefString& message_text,
                        const CefString& default_prompt_text, CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message)
{
    justcef_view::OnHostDialogStateChanged(browser, true);
    return false;
}

bool Client::OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser, const CefString& message_text, bool is_reload, CefRefPtr<CefJSDialogCallback> callback)
{
    justcef_view::OnHostDialogStateChanged(browser, true);
    return false;
}

void Client::OnResetDialogState(CefRefPtr<CefBrowser> browser)
{
    justcef_view::OnHostDialogStateChanged(browser, false);
}

void Client::OnDialogClosed(CefRefPtr<CefBrowser> browser)
{
    justcef_view::OnHostDialogStateChanged(browser, false);
}

void Client::OnFrameDetached(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    if (frame && frame->IsMain())
        justcef_view::OnHostFrameGone(browser, frame);
}

void Client::OnMainFrameChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> old_frame, CefRefPtr<CefFrame> new_frame)
{
    if (old_frame)
        justcef_view::OnHostFrameGone(browser, old_frame);
}

// TODO: Implement Minimized, Maximized, Restored, KeyboardEvent, Resized, Moved

CefRefPtr<CefRenderHandler> Client::GetRenderHandler()
{
    auto cl = CefCommandLine::GetGlobalCommandLine();
    if (cl->HasSwitch("headless"))
        return this;
    return nullptr;
}
