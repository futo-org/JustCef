#include "client.h"
#include "app_factory.h"
#include "ipc.h"

#include "include/base/cef_logging.h"

#if defined(OS_LINUX)
#include <gtk/gtk.h>
#endif

class BrowserApp : public CefApp, public CefBrowserProcessHandler {
 public:
  BrowserApp() {}

  // CefApp methods:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
    // Command-line flags can be modified in this callback.
    // |process_type| is empty for the browser process.
    if (process_type.empty()) {
#if defined(OS_MACOSX)
      // Disable the macOS keychain prompt. Cookies will not be encrypted.
      command_line->AppendSwitch("use-mock-keychain");
#endif
    }
  }

  // CefBrowserProcessHandler methods:
  void OnContextInitialized() override {
#if defined(OS_LINUX)
    if (!gtk_init_check(nullptr, nullptr)) 
    {
        LOG(ERROR) << "Failed to initialized GTK.";
        return;
    }
#endif
    
    LOG(INFO) << "Initialized GTK.";

    LOG(INFO) << "OnContextInitialized";
    IPC::Singleton.Start();

    IPC::Singleton.QueueWork([] () {
      LOG(INFO) << "NotifyReady before";
      IPC::Singleton.NotifyReady();
      LOG(INFO) << "NotifyReady after";
    });
  }

 private:
  IMPLEMENT_REFCOUNTING(BrowserApp);
  DISALLOW_COPY_AND_ASSIGN(BrowserApp);
};

namespace shared {
  CefRefPtr<CefApp> CreateBrowserProcessApp() {
    return new BrowserApp();
  }
}
