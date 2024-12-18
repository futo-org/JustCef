#ifndef CEF_DOTCEF_APP_FACTORY_H_
#define CEF_DOTCEF_APP_FACTORY_H_

#include "include/cef_app.h"

namespace shared {
    CefRefPtr<CefApp> CreateBrowserProcessApp();
    CefRefPtr<CefApp> CreateRendererProcessApp();
    CefRefPtr<CefApp> CreateOtherProcessApp();
}

#endif  // CEF_DOTCEF_APP_FACTORY_H_
