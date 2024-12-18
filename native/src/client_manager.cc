#include "client_manager.h"
#include "ipc.h"

#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

namespace shared {
  ClientManager* g_manager = nullptr;

  ClientManager::ClientManager() : is_closing_(false) {
    g_manager = this;
  }

  ClientManager::~ClientManager() {
    DCHECK(thread_checker_.CalledOnValidThread());

    std::lock_guard<std::recursive_mutex> lk(_browserListMutex);

    DCHECK(browser_list_.empty());
    g_manager = nullptr;
  }

  // static
  ClientManager* ClientManager::GetInstance() {
    CEF_REQUIRE_UI_THREAD();
    DCHECK(g_manager);
    return g_manager;
  }

  size_t ClientManager::GetBrowserCount() {
      return browser_list_.size();
  }

  void ClientManager::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    DCHECK(thread_checker_.CalledOnValidThread());

    std::lock_guard<std::recursive_mutex> lk(_browserListMutex);

    // Add to the list of existing browsers.
    browser_list_.push_back(browser);
  }

  void ClientManager::DoClose(CefRefPtr<CefBrowser> browser) {
    LOG(INFO) << "ClientManager::DoClose called identifier = " << browser->GetIdentifier() << ".";

    DCHECK(thread_checker_.CalledOnValidThread());

    {
      std::lock_guard<std::recursive_mutex> lk(_browserListMutex);

      if (browser_list_.size() == 1U) {
        // The last browser window is closing.
        is_closing_ = true;
      }
    }

    LOG(INFO) << "ClientManager::DoClose is_closing_ = " << is_closing_ << ".";

    if (is_closing_)
    {
      //LOG(INFO) << "Notify exit.";
      //IPC::Singleton.NotifyExit();
      IPC::Singleton.Stop();
    }

    LOG(INFO) << "ClientManager::DoClose finished identifier = " << browser->GetIdentifier() << ".";
  }

  void ClientManager::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    LOG(INFO) << "ClientManager::OnBeforeClose called identifier = " << browser->GetIdentifier() << ".";
    DCHECK(thread_checker_.CalledOnValidThread());

    std::lock_guard<std::recursive_mutex> lk(_browserListMutex);
    LOG(INFO) << "ClientManager::OnBeforeClose called identifier = " << browser->GetIdentifier() << ", acquired mutex.";

    // Remove from the list of existing browsers.
    BrowserList::iterator bit = browser_list_.begin();
    for (; bit != browser_list_.end(); ++bit) {
      if ((*bit)->IsSame(browser)) {
        browser_list_.erase(bit);
        break;
      }
    }

    if (browser_list_.empty()) {
      // All browser windows have closed. Quit the application message loop.
      CefQuitMessageLoop();
    }

    LOG(INFO) << "ClientManager::OnBeforeClose called identifier = " << browser->GetIdentifier() << ".";
  }

  void ClientManager::CloseAllBrowsers(bool force_close) {
    LOG(INFO) << "Close all browser called force_close = " << force_close << ".";

    DCHECK(thread_checker_.CalledOnValidThread());

    std::lock_guard<std::recursive_mutex> lk(_browserListMutex);
    LOG(INFO) << "Close all browser called force_close = " << force_close << ", acquired mutex.";

    if (browser_list_.empty())
      return;

    BrowserList::const_iterator it = browser_list_.begin();
    for (; it != browser_list_.end(); ++it)
    {
      LOG(INFO) << "Call (*it)->GetHost()->CloseBrowser(), identifier = " << (*it)->GetIdentifier() << ", force_close = " << force_close << " started.";
      (*it)->GetHost()->CloseBrowser(force_close);
      LOG(INFO) << "Call (*it)->GetHost()->CloseBrowser(), identifier = " << (*it)->GetIdentifier() << ", force_close = " << force_close << " stopped.";
    }
  }

  CefRefPtr<CefBrowser> ClientManager::AcquirePointer(int identifier) {
    std::lock_guard<std::recursive_mutex> lk(_browserListMutex);

    BrowserList::const_iterator it = browser_list_.begin();
    for (; it != browser_list_.end(); ++it)
    {
      auto refPtr = *it;
      if (refPtr->GetIdentifier() == identifier)
        return refPtr;
    }

    return nullptr;
  }

  bool ClientManager::IsClosing() const {
    DCHECK(thread_checker_.CalledOnValidThread());
    return is_closing_;
  }
}
