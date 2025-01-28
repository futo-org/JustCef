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
    LOG(INFO) << "Initialized GTK.";
#endif

    LOG(INFO) << "OnContextInitialized";

    if (IPC::Singleton.HasValidHandles()) {
      IPC::Singleton.Start();

      IPC::Singleton.QueueWork([] () {
        LOG(INFO) << "NotifyReady before";
        IPC::Singleton.NotifyReady();
        LOG(INFO) << "NotifyReady after";
      });
    } else {
      LOG(INFO) << "No handles specified, skipping IPC.";
    }

    CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
    if (command_line->HasSwitch("url")) {
      std::string url = command_line->GetSwitchValue("url");
      std::optional<std::string> title = command_line->HasSwitch("title") ? command_line->GetSwitchValue("title").ToString() : (std::optional<std::string>)std::nullopt;
      std::optional<std::string> appId = command_line->HasSwitch("appId") ? command_line->GetSwitchValue("appId").ToString() : (std::optional<std::string>)std::nullopt;
      LOG(INFO) << "Launching initial window with (url = " << url << ", title = " << (title ? *title : "Not specified") << ", appId = " << (appId ? *appId : "Not specified") << ")";

      IPCWindowCreate windowCreate;
      windowCreate.appId = appId;
      windowCreate.centered = true;
      windowCreate.contextMenuEnable = true;
      windowCreate.developerToolsEnabled = true;
      windowCreate.frameless = false;
      windowCreate.fullscreen = false;
      windowCreate.logConsole = false;
      windowCreate.minimumWidth = 0;
      windowCreate.minimumHeight = 0;
      windowCreate.modifyRequestBody = false;
      windowCreate.modifyRequests = false;
      windowCreate.preferredWidth = 800;
      windowCreate.preferredHeight = 800;
      windowCreate.proxyRequests = false;
      windowCreate.resizable = true;
      windowCreate.shown = true;
      windowCreate.title = title;
      windowCreate.url = url;
      CreateWindow(windowCreate);
    } else {
      LOG(INFO) << "No URL specified, skipping launching URL.";
    }
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
