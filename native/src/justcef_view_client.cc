#include "justcef_view_client.h"

#include "client_manager.h"
#include "client_util.h"
#include "ipc.h"
#include "justcef_view_common.h"
#include "justcef_view_host.h"

#include "include/wrapper/cef_helpers.h"

IPCWindowCreate MakeViewPopupSettings(const IPCWindowCreate& settings)
{
    IPCWindowCreate popupSettings = settings;
    popupSettings.bridgeEnabled = false;
    popupSettings.viewsEnabled = false;
    popupSettings.proxyRequests = settings.proxyRequests;
    popupSettings.modifyRequests = false;
    popupSettings.modifyRequestBody = false;
    popupSettings.fullscreen = false;
    popupSettings.shown = true;
    popupSettings.centered = true;
    popupSettings.frameless = false;
    popupSettings.resizable = true;
    popupSettings.minimumWidth = 0;
    popupSettings.minimumHeight = 0;
    popupSettings.preferredWidth = 1024;
    popupSettings.preferredHeight = 768;
    popupSettings.title = std::nullopt;
    popupSettings.iconPath = std::nullopt;
    return popupSettings;
}

ViewClient::ViewClient(const IPCWindowCreate& settings) : Client(settings, false)
{
}

void ViewClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title)
{
    CEF_REQUIRE_UI_THREAD();
    justcef_view::OnViewTitleChange(browser, title.ToString());
}

void ViewClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
    justcef_view::OnViewFullscreenModeChange(browser, fullscreen);
    Client::OnFullscreenModeChange(browser, fullscreen);
}


void ViewClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    TrackBrowser(browser);
    justcef_view::OnViewAfterCreated(this, browser);
    IPC::Singleton.NotifyWindowOpened(browser);
}

bool ViewClient::DoClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    shared::CancelPendingFileDialogs(browser->GetIdentifier());
    return justcef_view::OnViewDoClose(browser);
}

void ViewClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    justcef_view::OnViewBeforeClose(browser);
    Client::OnBeforeClose(browser);
}

void ViewClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
    justcef_view::OnViewLoadEnd(browser, frame, httpStatusCode);
    Client::OnLoadEnd(browser, frame, httpStatusCode);
}

void ViewClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl)
{
    justcef_view::OnViewLoadError(browser, frame, errorCode, errorText.ToString(), failedUrl.ToString());
    Client::OnLoadError(browser, frame, errorCode, errorText, failedUrl);
}

void ViewClient::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
    justcef_view::OnViewFocusChanged(browser, true);
    Client::OnGotFocus(browser);
}

void ViewClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string)
{
    LOG(ERROR) << "View render process terminated (identifier = " << browser->GetIdentifier() << ", status = " << status << ", error_code = " << error_code << ").";
    Features()->Cancel(browser->GetIdentifier(), true);
    Client::OnRenderProcessTerminated(browser, status, error_code, error_string);
    justcef_view::OnViewRenderProcessTerminated(browser);
}
