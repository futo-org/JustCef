#ifndef JUSTCEF_BROWSER_FEATURES_H_
#define JUSTCEF_BROWSER_FEATURES_H_
#include "include/cef_download_handler.h"
#include "include/cef_permission_handler.h"
#include "ipc.h"
#include <functional>
#include <map>

// UI-thread-only state. Each continuation is removed before being invoked.
class BrowserFeatures : public CefPermissionHandler, public CefDownloadHandler
{
public:
    using Decision = std::function<void(CefRefPtr<CefDictionaryValue>)>;
    void Ask(CefRefPtr<CefBrowser> browser, const std::string& kind, CefRefPtr<CefDictionaryValue> detail, Decision callback);
    bool Answer(int browserId, int requestId, CefRefPtr<CefDictionaryValue> answer);
    void Cancel(int browserId, bool closing = false, const std::string& frameId = {});
    bool DownloadCommand(int browserId, uint32_t downloadId, const std::string& action);
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&, uint32_t, CefRefPtr<CefMediaAccessCallback>) override;
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, const CefString&, uint32_t, CefRefPtr<CefPermissionPromptCallback>) override;
    void OnDismissPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, cef_permission_request_result_t) override;
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString&, CefRefPtr<CefBeforeDownloadCallback>) override;
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, CefRefPtr<CefDownloadItemCallback>) override;

private:
    struct Pending
    {
        int browserId;
        bool survivesNavigation;
        std::string frameId;
        Decision callback;
        ipc::CallHandle call;
    };
    int nextId_ = 0;
    std::map<int, Pending> pending_;
    std::map<std::pair<int, uint64_t>, int> prompts_;
    std::map<std::pair<int, uint32_t>, CefRefPtr<CefDownloadItemCallback>> downloads_;
    IMPLEMENT_REFCOUNTING(BrowserFeatures);
};
void HandleBrowserCommand(PacketReader& reader, ipc::Reply reply);
#endif
