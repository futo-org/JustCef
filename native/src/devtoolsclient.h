#ifndef DEVTOOLS_CLIENT_H
#define DEVTOOLS_CLIENT_H

#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"

#include <future>

class DevToolsClient : public CefClient,
               public CefKeyboardHandler {
 public:
    DevToolsClient();
    // CefClient methods:
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    // CefKeyboardHandler methods:
    bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event) override;
 private:
    IMPLEMENT_REFCOUNTING(DevToolsClient);
    DISALLOW_COPY_AND_ASSIGN(DevToolsClient);
};

#endif // DEVTOOLS_CLIENT_H
