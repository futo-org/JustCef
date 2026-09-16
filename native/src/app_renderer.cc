#include "app_factory.h"
#include "bridge.h"
#include "justcef_view_renderer.h"
#include "steam.h"

#include <unordered_map>
#include <unordered_set>

class RenderApp : public CefApp, public CefRenderProcessHandler
{
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra_info) override
    {
        if (!browser)
        {
            return;
        }

        ++browser_references_[browser->GetIdentifier()];

        if (IsBridgeEnabled(extra_info))
        {
            bridge_enabled_browsers_.insert(browser->GetIdentifier());
        }
        else
        {
            bridge_enabled_browsers_.erase(browser->GetIdentifier());
        }

        if (IsViewsEnabled(extra_info))
        {
            views_enabled_browsers_.insert(browser->GetIdentifier());
        }
        else
        {
            views_enabled_browsers_.erase(browser->GetIdentifier());
        }

        if (IsViewContent(extra_info))
        {
            view_content_browsers_.insert(browser->GetIdentifier());
        }
        else
        {
            view_content_browsers_.erase(browser->GetIdentifier());
        }
    }

    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override
    {
        if (!browser)
        {
            return;
        }

        auto references = browser_references_.find(browser->GetIdentifier());
        if (references != browser_references_.end() && --references->second > 0)
        {
            return;
        }
        browser_references_.erase(browser->GetIdentifier());

        bridge_enabled_browsers_.erase(browser->GetIdentifier());
        views_enabled_browsers_.erase(browser->GetIdentifier());
        view_content_browsers_.erase(browser->GetIdentifier());
        ClearBridgeState(browser);
        ClearViewState(browser);
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
    {
        if (!browser || !frame || !frame->IsMain())
        {
            return;
        }

        if (views_enabled_browsers_.find(browser->GetIdentifier()) != views_enabled_browsers_.end())
        {
            InstallViewElement(browser, frame, context);
        }

        if (view_content_browsers_.find(browser->GetIdentifier()) != view_content_browsers_.end())
        {
            InstallViewContentScript(browser, frame, context);
        }

        if (bridge_enabled_browsers_.find(browser->GetIdentifier()) == bridge_enabled_browsers_.end())
        {
            return;
        }

        InstallBridge(context);
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
    {
        if (!browser || !frame || !frame->IsMain())
        {
            return;
        }

        if (views_enabled_browsers_.find(browser->GetIdentifier()) != views_enabled_browsers_.end())
        {
            ReleaseViewContext(browser, frame, context);
        }

        if (view_content_browsers_.find(browser->GetIdentifier()) != view_content_browsers_.end())
        {
            ReleaseViewContentContext(browser, context);
        }

        if (bridge_enabled_browsers_.find(browser->GetIdentifier()) == bridge_enabled_browsers_.end())
        {
            return;
        }

        ReleaseBridgeContext(browser, frame, context);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override
    {
        if (HandleBridgeProcessMessage(browser, frame, message))
        {
            return true;
        }

        if (HandleViewProcessMessage(browser, frame, message))
        {
            return true;
        }

        return false;
    }

    void OnFocusedNodeChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefDOMNode> node) override
    {
        const bool editable = node && node->IsEditable();
        if (!editable)
        {
            auto msg = CefProcessMessage::Create(kOskMsg);
            msg->GetArgumentList()->SetInt(0, 0);
            frame->SendProcessMessage(PID_BROWSER, msg);
            return;
        }

        int mode = k_EFloatingGamepadTextInputModeModeSingleLine;
        if (node->IsElement())
        {
            auto tag = node->GetElementTagName().ToString();
            for (auto& c : tag)
                c = (char)tolower(c);
            if (tag == "textarea")
                mode = k_EFloatingGamepadTextInputModeModeMultipleLines;
            else if (tag == "input")
            {
                auto typ = node->GetElementAttribute("type").ToString();
                for (auto& c : typ)
                    c = (char)tolower(c);
                if (typ == "email")
                    mode = k_EFloatingGamepadTextInputModeModeEmail;
                else if (typ == "number" || typ == "tel")
                    mode = k_EFloatingGamepadTextInputModeModeNumeric;
            }
        }

        const CefRect r = node->GetElementBounds();
        auto msg = CefProcessMessage::Create(kOskMsg);
        auto args = msg->GetArgumentList();
        args->SetInt(0, 1);
        args->SetInt(1, r.x);
        args->SetInt(2, r.y);
        args->SetInt(3, r.width);
        args->SetInt(4, r.height);
        args->SetInt(5, mode);
        frame->SendProcessMessage(PID_BROWSER, msg);
    }

private:
    std::unordered_set<int> bridge_enabled_browsers_;
    std::unordered_set<int> views_enabled_browsers_;
    std::unordered_set<int> view_content_browsers_;
    std::unordered_map<int, int> browser_references_;

    IMPLEMENT_REFCOUNTING(RenderApp);
};

namespace shared
{
CefRefPtr<CefApp> CreateRendererProcessApp()
{
    return new RenderApp();
}
} // namespace shared
