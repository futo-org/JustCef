#ifndef CEF_CLIENT_H_
#define CEF_CLIENT_H_

#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/wrapper/cef_resource_manager.h"
#include "ipc.h"

#include <future>
#include <unordered_set>

class Client : public CefClient,
    public CefDisplayHandler,
    public CefLifeSpanHandler,
    public CefLoadHandler,
    public CefFocusHandler,
    public CefContextMenuHandler,
    public CefKeyboardHandler,
    public CefRequestHandler,
    public CefResourceRequestHandler,
    public CefDevToolsMessageObserver {
 public:
    Client(const IPCWindowCreate& settings);
    // CefClient methods:
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool is_navigation, bool is_download, const CefString& request_initiator, bool& disable_default_handling) override { return this; }
    // CefDisplayHandler methods:
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line) override;
    // CefLifeSpanHandler methods:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    // CefLoadHandler methods:
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override;
    // CefFocusHandler methods:
    void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override;
    void OnGotFocus(CefRefPtr<CefBrowser> browser) override;
    // CefContextMenuHandler methods:
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override;
    // CefKeyboardHandler methods:
    bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event) override;
    // CefResourceRequestHandler methods:
    CefRefPtr<CefResourceHandler> GetResourceHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request) override;
    cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback) override;
    void OnResourceLoadComplete(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response, URLRequestStatus status, int64_t received_content_length) override;
    // CefDevToolsMessageObserver methods:
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id, bool success, const void* result, size_t result_size) override;
    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void* params, size_t params_size) override;

    int GetIdentifier() { return _identifier; }
    std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> ExecuteDevToolsMethod(CefRefPtr<CefBrowser> browser, std::string& method, CefRefPtr<CefDictionaryValue> params = nullptr);
    std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> ExecuteDevToolsMethod(CefRefPtr<CefBrowser> browser, std::string& method, std::string& json);
    void OverrideTitle(CefRefPtr<CefBrowser> browser, const std::string& title);
    void OverrideIcon(CefRefPtr<CefBrowser> browser, const std::string& iconPath);
    void AddUrlToProxy(const std::string& url);
    void RemoveUrlToProxy(const std::string& url);
    void AddUrlToModify(const std::string& url);
    void RemoveUrlToModify(const std::string& url);
    void AddDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method);
    void RemoveDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method);

    IPCWindowCreate settings;
 private:
    void SetTitle(CefRefPtr<CefBrowser> browser, const std::string& title);
    bool EnsureDevToolsRegistration(CefRefPtr<CefBrowser> browser);

    std::map<int32_t, std::shared_ptr<std::promise<std::optional<IPCDevToolsMethodResult>>>> _devToolsMethodResults; 
    CefRefPtr<CefRegistration> _devToolsRegistration = nullptr;
    int _identifier = 0;
    int _messageIdGenerator = 0;
    std::unordered_set<int> _modifiedRequests;
    std::mutex _modifiedRequestsMutex;
    std::string _titleOverride;
    std::mutex _proxyRequestsSetMutex;
    std::unordered_set<std::string> _proxyRequestsSet;
    std::mutex _modifyRequestsSetMutex;
    std::unordered_set<std::string> _modifyRequestsSet;
    std::mutex _devToolsEventMethodsSetMutex;
    std::unordered_set<std::string> _devToolsEventMethodsSet;
    IMPLEMENT_REFCOUNTING(Client);
    DISALLOW_COPY_AND_ASSIGN(Client);
};

#endif // CEF_CLIENT_H_
