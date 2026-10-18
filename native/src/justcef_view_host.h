#ifndef JUSTCEF_VIEW_HOST_H_
#define JUSTCEF_VIEW_HOST_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_process_message.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

#include <functional>
#include <string>
#include <vector>

class Client;

namespace justcef_view
{


void ConfigureHostWindowLayout(CefRefPtr<CefWindow> window, CefRefPtr<CefBrowserView> browserView);
bool HandleHostProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message);
void OnHostWindowClosing(CefRefPtr<CefWindow> window);
void OnHostFrameGone(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
void OnHostBrowserGone(CefRefPtr<CefBrowser> browser);
void OnHostGotFocus(CefRefPtr<CefBrowser> browser);
bool IsFocusInView(CefRefPtr<CefBrowser> hostBrowser);
bool FocusView(CefRefPtr<CefBrowser> browser, std::function<void(bool)> completion = {});
bool IsEmbeddedBrowser(CefRefPtr<CefBrowser> browser);
void PropagateHostZoom(CefRefPtr<CefBrowser> browser, double zoomLevel);

void OnViewAfterCreated(Client* client, CefRefPtr<CefBrowser> browser);
bool OnViewDoClose(CefRefPtr<CefBrowser> browser);
void OnViewBeforeClose(CefRefPtr<CefBrowser> browser);
void OnViewLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode);
void OnViewLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int errorCode, const std::string& errorText, const std::string& failedUrl);
void OnViewTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title);
void OnViewFocusChanged(CefRefPtr<CefBrowser> browser, bool focused);
void OnViewFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen);
bool OnViewBrowserRequest(CefRefPtr<CefBrowser> browser, const std::string& kind, CefRefPtr<CefDictionaryValue> detail);
void OnViewRenderProcessTerminated(CefRefPtr<CefBrowser> browser);

bool TranslateViewOskRect(CefRefPtr<CefBrowser> browser, int& x, int& y, int& width, int& height);

}

#endif // JUSTCEF_VIEW_HOST_H_
