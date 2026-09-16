#ifndef JUSTCEF_VIEW_CLIENT_H_
#define JUSTCEF_VIEW_CLIENT_H_

#include "client.h"

class ViewClient : public Client
{
public:
    explicit ViewClient(const IPCWindowCreate& settings);

    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return _permissionHandler; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return nullptr; }
    CefRefPtr<CefFrameHandler> GetFrameHandler() override { return nullptr; }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id, const CefString& target_url, const CefString& target_frame_name,
                       CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture, const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings, CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override;
    void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override;
    void OnGotFocus(CefRefPtr<CefBrowser> browser) override;
    bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string) override;

private:
    CefRefPtr<CefPermissionHandler> _permissionHandler;
};

IPCWindowCreate MakeViewPopupSettings(const IPCWindowCreate& settings);

#endif // JUSTCEF_VIEW_CLIENT_H_
