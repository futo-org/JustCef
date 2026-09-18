#ifndef JUSTCEF_VIEW_HOST_H_
#define JUSTCEF_VIEW_HOST_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_process_message.h"
#include "include/views/cef_window.h"

#include <string>

class Client;

namespace justcef_view
{

bool HandleHostProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message);
void OnHostWindowLayoutChanged(CefRefPtr<CefWindow> window);
void OnHostWindowClosing(CefRefPtr<CefWindow> window);
void OnHostFrameGone(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
void OnHostBrowserGone(CefRefPtr<CefBrowser> browser);
void OnHostDialogStateChanged(CefRefPtr<CefBrowser> browser, bool open);
void OnHostGotFocus(CefRefPtr<CefBrowser> browser);
bool IsFocusInView(CefRefPtr<CefBrowser> hostBrowser);
void PropagateHostZoom(CefRefPtr<CefBrowser> browser, double zoomLevel);

void OnViewAfterCreated(Client* client, CefRefPtr<CefBrowser> browser);
bool OnViewDoClose(CefRefPtr<CefBrowser> browser);
void OnViewBeforeClose(CefRefPtr<CefBrowser> browser);
void OnViewLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
void OnViewLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode);
void OnViewLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int errorCode, const std::string& errorText, const std::string& failedUrl);
void OnViewTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title);
void OnViewFocusChanged(CefRefPtr<CefBrowser> browser, bool focused);
void OnViewTakeFocus(CefRefPtr<CefBrowser> browser, bool next);
bool OnViewKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event);
void OnViewFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen);
void OnViewPermissionRequest(CefRefPtr<CefBrowser> browser, const std::string& origin, const std::string& permissions);
void OnViewRenderProcessTerminated(CefRefPtr<CefBrowser> browser);
void OnViewWheel(CefRefPtr<CefBrowser> browser, double deltaX, double deltaY, double clientX, double clientY, int phase);
void OnViewKeyScroll(CefRefPtr<CefBrowser> browser, const std::string& kind);
bool TranslateViewOskRect(CefRefPtr<CefBrowser> browser, int& x, int& y);

} // namespace justcef_view

#endif // JUSTCEF_VIEW_HOST_H_
