#include "app_factory.h"
#include "steam.h"

class RenderApp : public CefApp, public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnFocusedNodeChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefDOMNode> node) override {
        const bool editable = node && node->IsEditable();
        if (!editable) {
            auto msg = CefProcessMessage::Create(kOskMsg);
            msg->GetArgumentList()->SetInt(0, 0);
            frame->SendProcessMessage(PID_BROWSER, msg);
            return;
        }

        int mode = k_EFloatingGamepadTextInputModeModeSingleLine;
        if (node->IsElement()) {
            auto tag = node->GetElementTagName().ToString();
            for (auto& c : tag) c = (char)tolower(c);
            if (tag == "textarea") mode = k_EFloatingGamepadTextInputModeModeMultipleLines;
            else if (tag == "input") {
                auto typ = node->GetElementAttribute("type").ToString();
                for (auto& c : typ) c = (char)tolower(c);
                if (typ == "email") mode = k_EFloatingGamepadTextInputModeModeEmail;
                else if (typ == "number" || typ == "tel") mode = k_EFloatingGamepadTextInputModeModeNumeric;
            }
        }

        const CefRect r = node->GetElementBounds();
        auto msg = CefProcessMessage::Create(kOskMsg);
        auto args = msg->GetArgumentList();
        args->SetInt(0, 1);
        args->SetInt(1, r.x); args->SetInt(2, r.y);
        args->SetInt(3, r.width); args->SetInt(4, r.height);
        args->SetInt(5, mode);
        frame->SendProcessMessage(PID_BROWSER, msg);
    }

private:
    IMPLEMENT_REFCOUNTING(RenderApp);
};

namespace shared {
    CefRefPtr<CefApp> CreateRendererProcessApp() {
        return new RenderApp();
    }
}
