#include "screenshot_capture.h"
#include "client.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include <cmath>
#include <memory>
#include <deque>
#include <unordered_map>
#include <algorithm>

namespace
{
CefRefPtr<CefDictionaryValue> Object(const std::string& text)
{
    auto value = CefParseJSON(text, JSON_PARSER_RFC);
    return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
}

class Capture : public std::enable_shared_from_this<Capture>
{
public:
    using Callback = std::function<void(bool, std::string)>;
    Capture(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> options, Callback callback)
        : browser_(browser), options_(options), callback_(std::move(callback))
    {
    }

    void Enqueue()
    {
        if (!browser_) return Finish(false, "Cannot capture a closed browser.");
        auto& queue = Queues()[browser_->GetIdentifier()];
        queue.push_back(shared_from_this());
        if (queue.size() == 1) Start();
    }

    void Cancel(const std::string& reason)
    {
        if (!callback_) return;
        auto callback = std::move(callback_);
        if (!inFlight_) Release();
        callback(false, reason);
    }

    void Start()
    {
        if (!callback_) return;
        std::weak_ptr<Capture> weak = shared_from_this();
        CefPostDelayedTask(TID_UI, base::BindOnce([](std::weak_ptr<Capture> weak) {
            if (auto capture = weak.lock()) capture->Cancel("Screenshot timed out.");
        }, weak), 60000);
        if (!browser_ || !browser_->IsValid())
            return Finish(false, "Cannot capture a closed browser.");
        auto params = options_->Copy(false);
        const bool fullPage = params->GetBool("fullPage");
        params->Remove("fullPage");
        if (!params->HasKey("format")) params->SetString("format", "png");
        params->SetBool("captureBeyondViewport", fullPage);
        if (!fullPage) return Image(params);
        Call("Page.getLayoutMetrics", CefDictionaryValue::Create(),
             [self = shared_from_this(), params](auto metrics) {
                 auto size = metrics->GetDictionary("cssContentSize");
                 if (!size) return self->Finish(false, "Cannot measure the document.");
                 auto clip = size->Copy(false);
                 const double zoom = std::pow(1.2, self->browser_->GetHost()->GetZoomLevel());
                 for (const auto* key : {"x", "y", "width", "height"}) {
                     const double value = clip->GetType(key) == VTYPE_INT ? clip->GetInt(key) : clip->GetDouble(key);
                     clip->SetDouble(key, value * zoom);
                 }
                 clip->SetDouble("scale", 1);
                 params->SetDictionary("clip", clip);
                 self->Image(params);
             });
    }

    void Finish(bool ok, std::string result)
    {
        if (!callback_)
            return;
        auto callback = std::move(callback_);
        Release();
        callback(ok, std::move(result));
    }

private:
    using Queue = std::deque<std::shared_ptr<Capture>>;
    static std::unordered_map<int, Queue>& Queues()
    {
        static std::unordered_map<int, Queue> queues;
        return queues;
    }

    void Release()
    {
        if (!browser_) return;
        auto& queues = Queues();
        auto found = queues.find(browser_->GetIdentifier());
        if (found == queues.end()) return;
        auto& queue = found->second;
        auto current = std::find(queue.begin(), queue.end(), shared_from_this());
        if (current == queue.end()) return;
        const bool wasFront = current == queue.begin();
        queue.erase(current);
        if (queue.empty()) queues.erase(found);
        else if (wasFront) {
            auto next = queue.front();
            CefPostTask(TID_UI, base::BindOnce([](std::shared_ptr<Capture> next) { next->Start(); }, next));
        }
    }

    void Call(const std::string& method, CefRefPtr<CefDictionaryValue> params, std::function<void(CefRefPtr<CefDictionaryValue>)> next)
    {
        if (!callback_)
            return;
        if (!browser_->IsValid())
            return Finish(false, "Browser closed during screenshot.");
        auto client = static_cast<Client*>(browser_->GetHost()->GetClient().get());
        auto self = shared_from_this();
        inFlight_ = true;
        if (!client->ExecuteDevToolsMethodAsync(browser_, method, params,
                                                [self, next](bool ok, std::string text)
                                                {
                                                    self->inFlight_ = false;
                                                    if (!self->callback_) { self->Release(); return; }
                                                    auto result = Object(text);
                                                    if (!ok || !result || result->HasKey("exceptionDetails"))
                                                        return self->Finish(false, text.empty() ? "Screenshot operation failed." : text);
                                                    next(result);
                                                }))
        {
            inFlight_ = false;
            Finish(false, "Cannot start screenshot operation.");
        }
    }

    void Image(CefRefPtr<CefDictionaryValue> params)
    {
        Call("Page.captureScreenshot", params,
             [self = shared_from_this()](auto result) {
                 if (!result->HasKey("data")) return self->Finish(false, "Invalid screenshot response.");
                 auto value = CefValue::Create();
                 value->SetDictionary(result);
                 self->Finish(true, CefWriteJSON(value, JSON_WRITER_DEFAULT));
             });
    }

    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefDictionaryValue> options_;
    Callback callback_;
    bool inFlight_ = false;
};
}

std::function<void()> CaptureBrowserScreenshot(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> options, std::function<void(bool, std::string)> callback)
{
    CEF_REQUIRE_UI_THREAD();
    auto capture = std::make_shared<Capture>(browser, options, std::move(callback));
    capture->Enqueue();
    std::weak_ptr<Capture> weak = capture;
    return [weak]()
    {
        if (auto operation = weak.lock())
            operation->Cancel("Screenshot canceled.");
    };
}
