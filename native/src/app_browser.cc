#include "client.h"
#include "app_factory.h"
#include "ipc.h"
#include "simple_handler.h"

#include "include/base/cef_logging.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

/*#if defined(OS_LINUX)
#include <gtk/gtk.h>
#endif*/

class SimpleWindowDelegate : public CefWindowDelegate {
 public:
  SimpleWindowDelegate(CefRefPtr<CefBrowserView> browser_view, cef_runtime_style_t runtime_style, cef_show_state_t initial_show_state)
      : browser_view_(browser_view),
        runtime_style_(runtime_style),
        initial_show_state_(initial_show_state) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    window->AddChildView(browser_view_);
    if (initial_show_state_ != CEF_SHOW_STATE_HIDDEN) {
      window->Show();
    }
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    browser_view_ = nullptr;
  }

  bool CanClose(CefRefPtr<CefWindow> window) override {
    CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
    if (browser) {
      return browser->GetHost()->TryCloseBrowser();
    }
    return true;
  }

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return CefSize(800, 600);
  }

  cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow> window) override {
    return initial_show_state_;
  }

  cef_runtime_style_t GetWindowRuntimeStyle() override {
    return runtime_style_;
  }

 private:
  CefRefPtr<CefBrowserView> browser_view_;
  const cef_runtime_style_t runtime_style_;
  const cef_show_state_t initial_show_state_;

  IMPLEMENT_REFCOUNTING(SimpleWindowDelegate);
  DISALLOW_COPY_AND_ASSIGN(SimpleWindowDelegate);
};

class SimpleBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit SimpleBrowserViewDelegate(cef_runtime_style_t runtime_style)
      : runtime_style_(runtime_style) {}

  bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools) override {
    CefWindow::CreateTopLevelWindow(new SimpleWindowDelegate(popup_browser_view, runtime_style_, CEF_SHOW_STATE_NORMAL));
    return true;
  }

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return runtime_style_;
  }

 private:
  const cef_runtime_style_t runtime_style_;

  IMPLEMENT_REFCOUNTING(SimpleBrowserViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(SimpleBrowserViewDelegate);
};

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
    CEF_REQUIRE_UI_THREAD();

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
    if (command_line->HasSwitch("simple-url")) {
      std::string url = command_line->GetSwitchValue("simple-url");
      LOG(INFO) << "Launching initial window with (url = " << url << ")";

      cef_runtime_style_t runtime_style = CEF_RUNTIME_STYLE_DEFAULT;
      bool use_alloy_style = command_line->HasSwitch("use-alloy-style");
      if (use_alloy_style) {
          runtime_style = CEF_RUNTIME_STYLE_ALLOY;
      }
      bool use_chrome_style = command_line->HasSwitch("use-chrome-style");
      if (use_chrome_style) {
          runtime_style = CEF_RUNTIME_STYLE_CHROME;
      }

      CefRefPtr<SimpleHandler> handler(new SimpleHandler(use_alloy_style));
      CefBrowserSettings settings;

      cef_show_state_t initial_show_state = CEF_SHOW_STATE_NORMAL;
      const std::string& show_state_value =
          command_line->GetSwitchValue("initial-show-state");
      if (show_state_value == "minimized") {
        initial_show_state = CEF_SHOW_STATE_MINIMIZED;
      } else if (show_state_value == "maximized") {
        initial_show_state = CEF_SHOW_STATE_MAXIMIZED;
      }

      CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(handler, url, settings, nullptr, nullptr, new SimpleBrowserViewDelegate(runtime_style));
      CefWindow::CreateTopLevelWindow(new SimpleWindowDelegate(browser_view, runtime_style, initial_show_state));
    } else if (command_line->HasSwitch("url")) {
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
      CreateBrowserWindow(windowCreate);
    } else {
      LOG(INFO) << "No URL specified, skipping launching URL.";
    }
  }

  CefRefPtr<CefClient> GetDefaultClient() override {
    CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
    std::optional<std::string> appId = command_line->HasSwitch("appId") ? command_line->GetSwitchValue("appId").ToString() : (std::optional<std::string>)std::nullopt;
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
    windowCreate.title = std::nullopt;
    windowCreate.url = "about:blank";
    return new Client(windowCreate);
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
