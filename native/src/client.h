#ifndef CEF_CLIENT_H_
#define CEF_CLIENT_H_

#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/wrapper/cef_resource_manager.h"
#include "ipc.h"

#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#define DEFAULT_DEDEUPE_INPUT_MS 120

class Client : public CefClient,
               public CefDisplayHandler,
               public CefLifeSpanHandler,
               public CefLoadHandler,
               public CefFocusHandler,
               public CefContextMenuHandler,
               public CefKeyboardHandler,
               public CefRequestHandler,
               public CefResourceRequestHandler,
               public CefRenderHandler,
               public CefDevToolsMessageObserver,
               public CefJSDialogHandler,
               public CefFrameHandler
{
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
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    CefRefPtr<CefFrameHandler> GetFrameHandler() override { return this; }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool is_navigation,
                                                                   bool is_download, const CefString& request_initiator, bool& disable_default_handling) override
    {
        return this;
    }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;
    // CefDisplayHandler methods:
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line) override;
    // CefLifeSpanHandler methods:
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id, const CefString& target_url, const CefString& target_frame_name,
                       CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture, const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings, CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access) override;
    void OnBeforeDevToolsPopup(CefRefPtr<CefBrowser> browser, CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings,
                               CefRefPtr<CefDictionaryValue>& extra_info, bool* use_default_window) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    // CefLoadHandler methods:
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward) override;
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
    void OnResourceLoadComplete(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response, URLRequestStatus status,
                                int64_t received_content_length) override;
    // CefRequestHandler methods:
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string) override;
    // CefJSDialogHandler methods:
    bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url, JSDialogType dialog_type, const CefString& message_text, const CefString& default_prompt_text,
                    CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message) override;
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser, const CefString& message_text, bool is_reload, CefRefPtr<CefJSDialogCallback> callback) override;
    void OnResetDialogState(CefRefPtr<CefBrowser> browser) override;
    void OnDialogClosed(CefRefPtr<CefBrowser> browser) override;
    // CefFrameHandler methods:
    void OnFrameDetached(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) override;
    void OnMainFrameChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> old_frame, CefRefPtr<CefFrame> new_frame) override;
    // CefDevToolsMessageObserver methods:
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id, bool success, const void* result, size_t result_size) override;
    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void* params, size_t params_size) override;
    // CefRenderHandler methods:
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override { rect = CefRect(0, 0, settings.preferredWidth, settings.preferredHeight); }
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) override {}

    int GetIdentifier() { return _identifier; }
    bool IsPrimaryBrowser(CefRefPtr<CefBrowser> browser) const { return browser && _primaryIdentifier != 0 && browser->GetIdentifier() == _primaryIdentifier; }
    bool ExecuteDevToolsMethodAsync(CefRefPtr<CefBrowser> browser, const std::string& method, CefRefPtr<CefDictionaryValue> params,
                                    std::function<void(bool success, std::string result)> callback);
    void OverrideTitle(CefRefPtr<CefBrowser> browser, const std::string& title);
    void OverrideIcon(CefRefPtr<CefBrowser> browser, const std::string& iconPath);
    void AddUrlToProxy(const std::string& url);
    void RemoveUrlToProxy(const std::string& url);
    void AddDomainToProxy(const std::string& domain);
    void RemoveDomainToProxy(const std::string& domain);
    void AddUrlToModify(const std::string& url);
    void RemoveUrlToModify(const std::string& url);
    void AddDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method);
    void RemoveDevToolsEventMethod(CefRefPtr<CefBrowser> browser, const std::string& method);
    void StartBridgeRpcCall(CefRefPtr<CefBrowser> browser, const std::string& method, const std::string& payload_json, ipc::Reply reply);
    void SetProxyRequests(bool proxyRequests);
    void SetModifyRequests(bool modifyRequests, bool modifyRequestBody);

    IPCWindowCreate settings;

protected:
    void TrackBrowser(CefRefPtr<CefBrowser> browser);

private:
    void SetTitle(CefRefPtr<CefBrowser> browser, const std::string& title);
    bool EnsureDevToolsRegistration(CefRefPtr<CefBrowser> browser);
    void CompleteBridgeRpcCall(int32_t request_id, bool success, const std::optional<std::string>& result_json, const std::optional<std::string>& error);
    void FailAllBridgeRpcCalls(const std::string& error);
    void CancelHostCalls();
    void CancelPendingModifies();

    std::map<int32_t, std::function<void(bool, std::string)>> _devToolsMethodCallbacks;
    int _primaryIdentifier = 0;
    std::unordered_map<int32_t, ipc::Reply> _bridgeRpcResults;
    std::unordered_map<int32_t, ipc::CallHandle> _hostCalls;
    CefRefPtr<CefRegistration> _devToolsRegistration = nullptr;
    int _identifier = 0;
    int _messageIdGenerator = 0;
    int _bridgeRpcRequestIdGenerator = 0;
    std::unordered_set<int> _modifiedRequests;
    std::mutex _modifiedRequestsMutex;
    std::unordered_map<int, ipc::CallHandle> _pendingModifies;
    std::mutex _pendingModifiesMutex;
    std::atomic<bool> _proxyRequests{false};
    std::atomic<bool> _modifyRequests{false};
    std::atomic<bool> _modifyRequestBody{false};
    std::string _titleOverride;
    std::mutex _proxyRequestsSetMutex;
    std::unordered_set<std::string> _proxyRequestsSet;
    std::mutex _proxyDomainsMutex;
    std::unordered_set<std::string> _exactProxyDomains;
    std::unordered_set<std::string> _leadingDotProxyDomains;
    std::mutex _proxyCacheMutex;
    std::unordered_set<std::string> _proxyCache;
    std::unordered_set<std::string> _negativeProxyCache;
    std::mutex _modifyRequestsSetMutex;
    std::unordered_set<std::string> _modifyRequestsSet;
    std::mutex _devToolsEventMethodsSetMutex;
    std::unordered_set<std::string> _devToolsEventMethodsSet;
    std::mutex _bridgeRpcResultsMutex;

    IMPLEMENT_REFCOUNTING(Client);
    DISALLOW_COPY_AND_ASSIGN(Client);
};

#endif // CEF_CLIENT_H_
