#ifndef CEF_DOTCEF_CLIENT_MANAGER_H_
#define CEF_DOTCEF_CLIENT_MANAGER_H_

#include <list>
#include <mutex>

#include "include/base/cef_thread_checker.h"
#include "include/cef_browser.h"

namespace shared {
  // Manages multiple CefBrowser instances. All methods must be called on the
  // main application thread (browser process UI thread).
  class ClientManager {
  public:
    ClientManager();
    ~ClientManager();

    // Returns the singleton instance of this object.
    static ClientManager* GetInstance();

    // Called from CefLifeSpanHandler methods:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser);
    void DoClose(CefRefPtr<CefBrowser> browser);
    void OnBeforeClose(CefRefPtr<CefBrowser> browser);
    size_t GetBrowserCount();

    // Request that all existing browser windows close.
    void CloseAllBrowsers(bool force_close);
    CefRefPtr<CefBrowser> AcquirePointer(int identifier);

    // Returns true if the last browser instance is closing.
    bool IsClosing() const;

  private:
    base::ThreadChecker thread_checker_;

    bool is_closing_;

    typedef std::list<CefRefPtr<CefBrowser>> BrowserList;
    BrowserList browser_list_;
    std::recursive_mutex _browserListMutex;
  };
}

#endif  // CEF_DOTCEF_CLIENT_MANAGER_H_
