#ifndef CEF_DOTCEF_CLIENT_BASE_H_
#define CEF_DOTCEF_CLIENT_BASE_H_

#include "include/cef_client.h"

#include <future>

namespace shared {
    // Returns the contents of |request| as a string.
    std::string DumpRequestContents(CefRefPtr<CefRequest> request);

    // Platform-specific implementations.
    void PlatformTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title);
    void PlatformIconChange(CefRefPtr<CefBrowser> browser, const std::string& iconPath);
    bool PlatformGetFullscreen(CefRefPtr<CefBrowser> browser);
    void PlatformSetFullscreen(CefRefPtr<CefBrowser> browser, bool fullscreen);
    void PlatformSetResizable(CefRefPtr<CefBrowser> browser, bool resizable);
    void PlatformSetFrameless(CefRefPtr<CefBrowser> browser, bool frameless);
    void PlatformSetMinimumWindowSize(CefRefPtr<CefBrowser> browser, int minWidth, int minHeight);
    void PlatformMaximize(CefRefPtr<CefBrowser> browser);
    void PlatformMinimize(CefRefPtr<CefBrowser> browser);
    void PlatformRestore(CefRefPtr<CefBrowser> browser);
    void PlatformShow(CefRefPtr<CefBrowser> browser);
    void PlatformHide(CefRefPtr<CefBrowser> browser);
    void PlatformActivate(CefRefPtr<CefBrowser> browser);
    void PlatformBringToTop(CefRefPtr<CefBrowser> browser);
    void PlatformSetAlwaysOnTop(CefRefPtr<CefBrowser> browser, bool alwaysOnTop);
    CefSize PlatformGetWindowSize(CefRefPtr<CefBrowser> browser);
    void PlatformCenterWindow(CefRefPtr<CefBrowser> browser, const CefSize& size);
    void PlatformSetWindowSize(CefRefPtr<CefBrowser> browser, const CefSize& size);
    CefPoint PlatformGetWindowPosition(CefRefPtr<CefBrowser> browser);
    void PlatformSetWindowPosition(CefRefPtr<CefBrowser> browser, const CefPoint& position);
    void PlatformWindowRequestFocus(CefRefPtr<CefBrowser> browser);
    std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string /* name [Text Files (*.txt)] */, std::string /* *.txt */>>& filters);
    std::future<std::string> PlatformPickDirectory();
    std::future<std::string> PlatformSaveFile(const std::string& default_name, const std::vector<std::pair<std::string, std::string>>& filters);
}
#endif  // CEF_DOTCEF_CLIENT_BASE_H_

