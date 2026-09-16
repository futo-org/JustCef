#include "justcef_view_client.h"

#include "client_manager.h"
#include "client_util.h"
#include "ipc.h"
#include "justcef_view_common.h"
#include "justcef_view_host.h"

#include "include/wrapper/cef_helpers.h"

namespace
{

class ViewPermissionHandler : public CefPermissionHandler
{
public:
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& requesting_origin, uint32_t requested_permissions,
                                        CefRefPtr<CefMediaAccessCallback> callback) override
    {
        justcef_view::OnViewPermissionRequest(browser, requesting_origin.ToString(), "media:" + std::to_string(requested_permissions));
        return false;
    }

    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t prompt_id, const CefString& requesting_origin, uint32_t requested_permissions,
                                CefRefPtr<CefPermissionPromptCallback> callback) override
    {
        justcef_view::OnViewPermissionRequest(browser, requesting_origin.ToString(), "prompt:" + std::to_string(requested_permissions));
        return false;
    }

private:
    IMPLEMENT_REFCOUNTING(ViewPermissionHandler);
};

} // namespace

IPCWindowCreate MakeViewPopupSettings(const IPCWindowCreate& settings)
{
    IPCWindowCreate popupSettings = settings;
    popupSettings.bridgeEnabled = false;
    popupSettings.viewsEnabled = false;
    popupSettings.proxyRequests = false;
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

ViewClient::ViewClient(const IPCWindowCreate& settings) : Client(settings), _permissionHandler(new ViewPermissionHandler())
{
}

bool ViewClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    const std::string message_name = message->GetName();
    if (message_name.rfind(kViewMessagePrefix, 0) != 0)
        return Client::OnProcessMessageReceived(browser, frame, source_process, message);

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!frame || !frame->IsMain() || !args)
        return true;

    if (message_name == kViewWheelMessageName && args->GetSize() >= 5)
        justcef_view::OnViewWheel(browser, args->GetDouble(0), args->GetDouble(1), args->GetDouble(2), args->GetDouble(3), static_cast<int>(args->GetDouble(4)));
    else if (message_name == kViewKeyScrollMessageName && args->GetSize() >= 1)
        justcef_view::OnViewKeyScroll(browser, args->GetString(0).ToString());
    return true;
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

bool ViewClient::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id, const CefString& target_url, const CefString& target_frame_name,
                               CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture, const CefPopupFeatures& popupFeatures,
                               CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client, CefBrowserSettings& browserSettings, CefRefPtr<CefDictionaryValue>& extra_info,
                               bool* no_javascript_access)
{
    Client::OnBeforePopup(browser, frame, popup_id, target_url, target_frame_name, target_disposition, user_gesture, popupFeatures, windowInfo, client, browserSettings,
                          extra_info, no_javascript_access);
    client = new Client(MakeViewPopupSettings(settings));
    return false;
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

void ViewClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
    justcef_view::OnViewLoadStart(browser, frame);
    Client::OnLoadStart(browser, frame, transition_type);
}

void ViewClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl)
{
    justcef_view::OnViewLoadError(browser, frame, errorCode, errorText.ToString(), failedUrl.ToString());
    Client::OnLoadError(browser, frame, errorCode, errorText, failedUrl);
}

void ViewClient::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    justcef_view::OnViewTakeFocus(browser, next);
    Client::OnTakeFocus(browser, next);
}

void ViewClient::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
    justcef_view::OnViewFocusChanged(browser, true);
    Client::OnGotFocus(browser);
}

bool ViewClient::OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event)
{
    if (event.type == KEYEVENT_RAWKEYDOWN && event.windows_key_code == 0x7A)
        return justcef_view::OnViewKeyEvent(browser, event);

    return Client::OnKeyEvent(browser, event, os_event) || justcef_view::OnViewKeyEvent(browser, event);
}

void ViewClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string)
{
    LOG(ERROR) << "View render process terminated (identifier = " << browser->GetIdentifier() << ", status = " << status << ", error_code = " << error_code << ").";
    justcef_view::OnViewRenderProcessTerminated(browser);
}
