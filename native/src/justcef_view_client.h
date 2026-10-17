#ifndef JUSTCEF_VIEW_CLIENT_H_
#define JUSTCEF_VIEW_CLIENT_H_

#include "client.h"

class ViewClient : public Client
{
public:
    explicit ViewClient(const IPCWindowCreate& settings);

    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override;
    void OnGotFocus(CefRefPtr<CefBrowser> browser) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string) override;

};

IPCWindowCreate MakeViewPopupSettings(const IPCWindowCreate& settings);

#endif // JUSTCEF_VIEW_CLIENT_H_
