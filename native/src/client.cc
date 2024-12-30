#include "client.h"

#include "include/cef_command_line.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include "client_manager.h"
#include "client_util.h"
#include "ipc.h"
#include "devtoolsclient.h"
#include "stb_image.h"

#ifdef _WIN32
typedef HRESULT(WINAPI* DwmSetWindowAttributeProc)(HWND, DWORD, LPCVOID, DWORD);

typedef struct _MARGINS {
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
} MARGINS, *PMARGINS;
typedef HRESULT(WINAPI* DwmExtendFrameIntoClientAreaProc)(HWND, const MARGINS*);

const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;
const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

bool IsWindows10OrGreater(int build = -1) {
    OSVERSIONINFOEXW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = 10;
    osvi.dwBuildNumber = build;

    DWORDLONG const dwlConditionMask = VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_BUILDNUMBER, build == -1 ? VER_EQUAL : VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | (build == -1 ? 0 : VER_BUILDNUMBER), dwlConditionMask);
}

bool UseImmersiveDarkMode(HWND hwnd, bool enabled) {
    static HMODULE hDwmapi = LoadLibraryW(L"dwmapi.dll");
    static DwmSetWindowAttributeProc DwmSetWindowAttribute = nullptr;
    static DwmExtendFrameIntoClientAreaProc DwmExtendFrameIntoClientArea = nullptr;

    if (IsWindows10OrGreater(17763)) {
        if (hDwmapi) {
            if (!DwmSetWindowAttribute) {
                DwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeProc>(GetProcAddress(hDwmapi, "DwmSetWindowAttribute"));
            }

            if (!DwmExtendFrameIntoClientArea) {
                DwmExtendFrameIntoClientArea = reinterpret_cast<DwmExtendFrameIntoClientAreaProc>(GetProcAddress(hDwmapi, "DwmExtendFrameIntoClientArea"));
            }

            if (DwmSetWindowAttribute) {
                DWORD attribute = DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;
                if (IsWindows10OrGreater(18985)) {
                    attribute = DWMWA_USE_IMMERSIVE_DARK_MODE;
                }

                int useImmersiveDarkMode = enabled ? 1 : 0;
                HRESULT result = DwmSetWindowAttribute(hwnd, attribute, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode));
                LOG(INFO) << "DwmSetWindowAttribute result: " << (int)result;

                if (SUCCEEDED(result)) {
                    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    if (DwmExtendFrameIntoClientArea) {
                        MARGINS margins = {-1};
                        result = DwmExtendFrameIntoClientArea(hwnd, &margins);
                        LOG(INFO) << "DwmExtendFrameIntoClientArea result: " << (int)result;
                    } else {
                        LOG(WARNING) << "Windows failed to find DwmExtendFrameIntoClientArea in 'dwmapi.dll'.";
                    }
                }

                return SUCCEEDED(result);
            } else {
                LOG(WARNING) << "Windows failed to find DwmSetWindowAttribute in 'dwmapi.dll'.";
            }
        } else {
            LOG(WARNING) << "Windows failed to load 'dwmapi.dll'.";
        }
    } else {
        LOG(WARNING) << "Windows build not high enough for immersive dark mode feature.";
    }

    return false;
}

struct WindowData {
    int identifier;
    WNDPROC originalWndProc;
};

std::map<HWND, WindowData> hwndMap;

static LRESULT CALLBACK WindowProcHook(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto it = hwndMap.find(hwnd);
    if (it != hwndMap.end()) {
        if (uMsg == WM_CLOSE || uMsg == WM_DESTROY) {
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)it->second.originalWndProc);
            hwndMap.erase(it);
            LOG(INFO) << "Unhooked window procedure for identifier: " << it->second.identifier;
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        CefRefPtr<CefBrowser> browser = shared::ClientManager::GetInstance()->AcquirePointer(it->second.identifier);
        if (!browser) {
            LOG(ERROR) << "WindowProcHook called while CefBrowser is already closed. Ignored.";
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
        Client* pClient = (Client*)client.get();
        if (!pClient) {
            LOG(ERROR) << "WindowProcHook client is null. Ignored.";
            return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
        }

        if (uMsg == WM_GETMINMAXINFO) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = pClient->settings.minimumWidth;
            mmi->ptMinTrackSize.y = pClient->settings.minimumHeight;
        } else if (uMsg == WM_SETTINGCHANGE) {
            if (wParam == SPI_SETCLIENTAREAANIMATION) {
                UseImmersiveDarkMode(hwnd, true);
            }
        }

        return CallWindowProc(it->second.originalWndProc, hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}


#endif

Client::Client(const IPCWindowCreate& settings) : settings(settings) {}

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
    if (!shared::IsViewsEnabled())
        shared::PlatformSetFullscreen(browser, fullscreen);

    IPC::Singleton.QueueWork([browser, fullscreen] () {
        IPC::Singleton.NotifyWindowFullscreenChanged(browser, fullscreen);
    });
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();

    _identifier = browser->GetIdentifier();

    // Add to the list of existing browsers.
    shared::ClientManager::GetInstance()->OnAfterCreated(browser);
    LOG(INFO) << "Browser opened " << browser->GetIdentifier();

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();

        window->SetFullscreen(settings.fullscreen);
        if (settings.centered && settings.shown) {
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
    else
    {
        if (settings.shown)
        {
            shared::PlatformShow(browser);
            shared::PlatformSetFullscreen(browser, settings.fullscreen);
            shared::PlatformSetFrameless(browser, settings.frameless);
            shared::PlatformSetResizable(browser, settings.resizable);
            if (settings.centered) {
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
        //SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#endif
    }

    if (settings.title)
        OverrideTitle(browser, *settings.title);

    if (settings.iconPath)
        OverrideIcon(browser, *settings.iconPath);

    IPC::Singleton.QueueWork([browser] () {
        IPC::Singleton.NotifyWindowOpened(browser);
    });
}

bool Client::DoClose(CefRefPtr<CefBrowser> browser) {
    LOG(INFO) << "DoClose called " << browser->GetIdentifier();

    CEF_REQUIRE_UI_THREAD();

    // Closing the main window requires special handling. See the DoClose()
    // documentation in the CEF header for a detailed destription of this
    // process.
    shared::ClientManager::GetInstance()->DoClose(browser);

    // Allow the close. For windowed browsers this will result in the OS close
    // event being sent.
    LOG(INFO) << "DoClose finished " << browser->GetIdentifier();
    return false;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    LOG(INFO) << "OnBeforeClose called " << browser->GetIdentifier();

    CEF_REQUIRE_UI_THREAD();

#if _WIN32
    HWND hwnd = browser->GetHost()->GetWindowHandle();
    auto it = hwndMap.find(hwnd);
    if (it != hwndMap.end()) {
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)it->second.originalWndProc);
        hwndMap.erase(it);
        LOG(INFO) << "Unhooked window procedure for identifier: " << it->second.identifier;
    }
#endif

    for (auto& itr : _devToolsMethodResults)
        itr.second->set_value(std::nullopt);
    _devToolsMethodResults.clear();

    _identifier = 0;

    // Remove from the list of existing browsers.
    shared::ClientManager::GetInstance()->OnBeforeClose(browser);

    LOG(INFO) << "Browser closed " << browser->GetIdentifier();

    IPC::Singleton.QueueWork([browser] () {
        IPC::Singleton.NotifyWindowClosed(browser);
    });

    LOG(INFO) << "OnBeforeClose finished " << browser->GetIdentifier();
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) 
{
    IPC::Singleton.QueueWork([browser, frame]() {
        IPC::Singleton.NotifyWindowLoadEnd(browser, frame->GetURL());
    });
}

void Client::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) 
{
    IPC::Singleton.QueueWork([browser, frame]() {
        IPC::Singleton.NotifyWindowLoadStart(browser, frame->GetURL());
    });
}

void Client::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) 
{
    LOG(ERROR) << "Failed to load URL (" << errorCode << ") '" << failedUrl << "': " << errorText;

    IPC::Singleton.QueueWork([browser, errorCode, errorText, failedUrl]() {
        IPC::Singleton.NotifyWindowLoadError(browser, errorCode, errorText, failedUrl);
    });
}

void Client::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    LOG(INFO) << "Browser unfocused " << browser->GetIdentifier();

    IPC::Singleton.QueueWork([browser]() {
        IPC::Singleton.NotifyWindowUnfocused(browser);
    });
}

void Client::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
    LOG(INFO) << "Browser focused " << browser->GetIdentifier();

    IPC::Singleton.QueueWork([browser]() {
        IPC::Singleton.NotifyWindowFocused(browser);
    });
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
            case 0x74: //F5
                if (settings.developerToolsEnabled)
                {
                    browser->Reload();
                }
                return true;
            case 0x7B: //F12
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
            case 0x7A: //F11
                CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
                if (browser_view)
                {
                    CefRefPtr<CefWindow> window = browser_view->GetWindow();
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
        LOG(INFO) << "EnsureDevToolsRegistration new registration added (identifier = " << browser->GetIdentifier() << ", _devToolsRegistration = " << (size_t)_devToolsRegistration.get() << ")";
    }

    return true;
}

std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> Client::ExecuteDevToolsMethod(CefRefPtr<CefBrowser> browser, std::string& method, CefRefPtr<CefDictionaryValue> params)
{
    CEF_REQUIRE_UI_THREAD();

    if (!EnsureDevToolsRegistration(browser))
        return std::nullopt;

    int messageId = ++_messageIdGenerator;
    std::shared_ptr<std::promise<std::optional<IPCDevToolsMethodResult>>> promise = std::make_shared<std::promise<std::optional<IPCDevToolsMethodResult>>>();
    _devToolsMethodResults[messageId] = promise;
    LOG(INFO) << "ExecuteDevToolsMethod (identifier = " << browser->GetIdentifier() << ", method = " << method << ", messageId = " << messageId << ")";
    browser->GetHost()->ExecuteDevToolsMethod(messageId, method, params);
    return promise->get_future();
}

std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> Client::ExecuteDevToolsMethod(CefRefPtr<CefBrowser> browser, std::string& method, std::string& json)
{
    LOG(INFO) << "ExecuteDevToolsMethod (identifier = " << browser->GetIdentifier() << ", method = " << method << ")";

    CefRefPtr<CefValue> value = CefParseJSON(json, cef_json_parser_options_t::JSON_PARSER_RFC);
    if (value && value->GetType() == VTYPE_DICTIONARY)
        return ExecuteDevToolsMethod(browser, method, value->GetDictionary());

    LOG(ERROR) << "Failed to parse JSON or JSON is not a dictionary.";
    return std::nullopt;
}

void Client::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id, bool success, const void* result, size_t result_size)
{
    LOG(INFO) << "OnDevToolsMethodResult (identifier = " << browser->GetIdentifier() << ", message_id = " << message_id << ", success = " << success << ")";

    CEF_REQUIRE_UI_THREAD();

    auto itr = _devToolsMethodResults.find(message_id);
    auto pPromise = itr->second;
    if (itr == _devToolsMethodResults.end())
        return;

    _devToolsMethodResults.erase(itr);

    IPCDevToolsMethodResult r;
    r.messageId = message_id;
    r.success = success;
    r.result = std::make_shared<std::vector<uint8_t>>(result_size);
    memcpy(r.result->data(), result, result_size);
    pPromise->set_value(r);
}

void Client::OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void* params, size_t params_size)
{
    LOG(INFO) << "OnDevToolsEvent (identifier = " << browser->GetIdentifier() << ", method = " << method << ")";

    {
        std::lock_guard<std::mutex> lk(_devToolsEventMethodsSetMutex);
        if (_devToolsEventMethodsSet.find(method) == _devToolsEventMethodsSet.end()) {
            return;
        }
    }

    IPC::Singleton.QueueWork([
        p = std::vector<uint8_t>(static_cast<const uint8_t*>(params), static_cast<const uint8_t*>(params) + params_size), 
        m = CefString(method), 
        browser]() 
    {
        IPC::Singleton.NotifyWindowDevToolsEvent(browser, m, p.data(), p.size());
    });
}

class ProxyResourceHandler : public CefResourceHandler {
public:
    ProxyResourceHandler(int32_t identifier, CefRefPtr<CefRequest> request)
        : _identifier(identifier), _request(request), _offset(0) {}

    bool Open(CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) override 
    {
        std::unique_ptr<IPCProxyResponse> response = IPC::Singleton.WindowProxyRequest(_identifier, request);
        if (!response)
        {
            // If there's no response, indicate that we're not handling the request
            //TODO: The not handled flow doesn't seem to work yet
            handle_request = false;
            return true;
        }
        
        handle_request = true;
        _response = std::move(response);
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length, CefString& redirectUrl) override 
    {
        if (!_response)
            return;

        response->SetStatus(_response->status_code);
        response->SetStatusText(_response->status_text);
        if (_response->media_type)
            response->SetMimeType(*_response->media_type);

        CefResponse::HeaderMap headerMap;
        for (auto& header : _response->headers) {
            headerMap.insert(std::make_pair(header.first, header.second));
        }

        response->SetHeaderMap(headerMap);
        response_length = _response->body ? (*_response->body).size() : -1;
    }

    bool Read(void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback) override 
    {
        bytes_read = 0;

        if (!_response) 
            return false;

        if (_response->body)
        {
            if (_offset < (*_response->body).size()) 
            {
                size_t bytes_to_copy = std::min(static_cast<size_t>(bytes_to_read), (*_response->body).size() - _offset);
                memcpy(data_out, (*_response->body).data() + _offset, bytes_to_copy);
                _offset += bytes_to_copy;
                bytes_read = (int)bytes_to_copy;
                return true;
            }
        }
        else if (_response->bodyStream)
        {
            size_t bytesRead = _response->bodyStream->Read((uint8_t*)data_out, static_cast<size_t>(bytes_to_read));

            if (bytesRead > 0)
            {
                bytes_read = (int)bytesRead;
                return true;
            }
        }

        return false;
    }

    void Cancel() override 
    {
        if (_response && _response->bodyStream)
        {
            LOG(INFO) << "Closing stream " << _response->bodyStream->GetIdentifier() << ".";
            _response->bodyStream->Close();
            IPC::Singleton.CloseStream(_response->bodyStream->GetIdentifier());
        }
    }
private:
    int32_t _identifier;
    CefRefPtr<CefRequest> _request;
    std::unique_ptr<IPCProxyResponse> _response;
    size_t _offset;

    IMPLEMENT_REFCOUNTING(ProxyResourceHandler);
};

CefRefPtr<CefResourceHandler> Client::GetResourceHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request)
{
    if (settings.proxyRequests)
        return new ProxyResourceHandler(browser->GetIdentifier(), request);

    {
        std::lock_guard<std::mutex> lk(_proxyRequestsSetMutex);
        if (_proxyRequestsSet.find(request->GetURL()) != _proxyRequestsSet.end())
            return new ProxyResourceHandler(browser->GetIdentifier(), request);
    }

    return nullptr;
}

cef_return_value_t Client::OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback) {
    int requestIdentifier = (int)request->GetIdentifier();
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
            IPC::Singleton.WindowModifyRequest(browser->GetIdentifier(), request, settings.modifyRequestBody);
    };

    if (settings.modifyRequests) {
        modifyRequestIfNeeded(request->GetURL());
    }

    {
        std::lock_guard<std::mutex> lk(_modifyRequestsSetMutex);
        if (_modifyRequestsSet.find(request->GetURL()) != _modifyRequestsSet.end()) {
            modifyRequestIfNeeded(request->GetURL());
        }
    }

    return RV_CONTINUE;
}

void Client::OnResourceLoadComplete(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response, URLRequestStatus status, int64_t received_content_length) {
    std::lock_guard<std::mutex> lock(_modifiedRequestsMutex);
    int requestIdentifier = (int)request->GetIdentifier();
    _modifiedRequests.erase(requestIdentifier);
}

void Client::OverrideTitle(CefRefPtr<CefBrowser> browser, const std::string& title)
{
    LOG(INFO) << "Override title: " << *settings.title;
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
    LOG(INFO) << "Override icon: " << *settings.iconPath;
    CefRefPtr<CefBrowserView> browserView = CefBrowserView::GetForBrowser(browser);
    if (browserView)
    {
        CefRefPtr<CefWindow> window = browserView->GetWindow();
        int width, height, channels;
        unsigned char* image = stbi_load(iconPath.c_str(), &width, &height, &channels, 4);
        if (!image) {
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

bool Client::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line)
{
    if (settings.logConsole)
        LOG(INFO) << "ConsoleMessage:" << level << ":" << source.ToString().c_str() << ":" << line << ": " << message.ToString().c_str();
    return true;
}

//TODO: Implement Minimized, Maximized, Restored, KeyboardEvent, Resized, Moved
