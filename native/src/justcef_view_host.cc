#include "justcef_view_host.h"

#include "client.h"
#include "client_manager.h"
#include "client_util.h"
#include "ipc.h"
#include "justcef_view_client.h"
#include "justcef_view_common.h"

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_parser.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_panel_delegate.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#if defined(OS_MAC)
#include "include/cef_path_util.h"
#endif
#if defined(OS_WIN)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace justcef_view
{
namespace
{

struct GuestApi
{
    using GetId = int (*)(int, int, char*, size_t);
    using IsAttached = int (*)(int);
    using Focus = void (*)(int);
    using Detach = void (*)(int);
    using TransformRect = int (*)(int, int*, int*, int*, int*);
    GetId getId = nullptr;
    IsAttached isAttached = nullptr;
    Focus focus = nullptr;
    Detach detach = nullptr;
    TransformRect transformRect = nullptr;
};
const GuestApi& GetGuestApi()
{
    static GuestApi api = []
    {
        GuestApi result;
#if defined(OS_MAC)
        CefString directory;
        if (CefGetPath(PK_DIR_EXE, directory))
        {
            const std::string path = directory.ToString() + "/../Frameworks/Chromium Embedded Framework.framework/Chromium Embedded Framework";
            void* library = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
#elif defined(OS_WIN)
        auto library = GetModuleHandleW(L"libcef.dll");
        auto dlsym = [](HMODULE module, const char* name) { return GetProcAddress(module, name); };
#else
        void* library = dlopen("libcef.so", RTLD_LAZY | RTLD_NOLOAD);
#endif
            if (library)
            {
                result.getId = reinterpret_cast<GuestApi::GetId>(dlsym(library, "justcef_guest_surface_id"));
                result.isAttached = reinterpret_cast<GuestApi::IsAttached>(dlsym(library, "justcef_guest_is_attached"));
                result.focus = reinterpret_cast<GuestApi::Focus>(dlsym(library, "justcef_guest_focus"));
                result.transformRect = reinterpret_cast<GuestApi::TransformRect>(dlsym(library, "justcef_guest_transform_rect"));
                result.detach = reinterpret_cast<GuestApi::Detach>(dlsym(library, "justcef_guest_detach"));
#if !defined(OS_WIN)
                dlclose(library);
#endif
            }
#if defined(OS_MAC)
        }
#endif
        return result;
    }();
    return api;
}

class TransparentPanelDelegate : public CefPanelDelegate
{
public:
    TransparentPanelDelegate() = default;

    void OnThemeChanged(CefRefPtr<CefView> view) override { view->SetBackgroundColor(0); }

private:
    IMPLEMENT_REFCOUNTING(TransparentPanelDelegate);
    DISALLOW_COPY_AND_ASSIGN(TransparentPanelDelegate);
};

class ViewSurface
{
public:
    ViewSurface(CefRefPtr<CefWindow> window, CefRefPtr<CefBrowserView> browserView)
        : window_(window), browserView_(browserView) {}
    void Attach()
    {
        holder_ = CefPanel::CreatePanel(new TransparentPanelDelegate());
        holder_->SetVisible(false);
        holder_->AddChildView(browserView_);
        window_->AddChildView(holder_);
    }
    void Detach()
    {
        auto browser = browserView_ ? browserView_->GetBrowser() : nullptr;
        if (browser && GetGuestApi().detach)
            GetGuestApi().detach(browser->GetIdentifier());
        if (window_ && holder_ && holder_->IsValid() && holder_->IsAttached())
            window_->RemoveChildView(holder_);
        holder_ = nullptr;
        browserView_ = nullptr;
        window_ = nullptr;
    }
private:
    CefRefPtr<CefWindow> window_;
    CefRefPtr<CefBrowserView> browserView_;
    CefRefPtr<CefPanel> holder_;
};

class ViewBrowserViewDelegate : public CefBrowserViewDelegate
{
public:
    explicit ViewBrowserViewDelegate(const IPCWindowCreate& settings) : settings_(settings) {}

    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

    bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools) override
    {
        CreateTopLevelPopupWindow(popup_browser_view, is_devtools, MakeViewPopupSettings(settings_));
        return true;
    }

private:
    const IPCWindowCreate settings_;

    IMPLEMENT_REFCOUNTING(ViewBrowserViewDelegate);
    DISALLOW_COPY_AND_ASSIGN(ViewBrowserViewDelegate);
};

struct ViewEntry
{
    int32_t key = 0;
    int parentId = 0;
    std::string frameId;
    CefRefPtr<CefFrame> frame;
    std::string docToken;
    int32_t elementId = 0;
    CefRefPtr<CefWindow> window;
    CefRefPtr<Client> client;
    CefRefPtr<CefBrowserView> browserView;
    CefRefPtr<CefBrowser> browser;
    std::unique_ptr<ViewSurface> surface;
    int viewId = 0;
    std::string pendingSrc;
    bool handshakeDone = false;
    bool allowed = false;
    bool destroyRequested = false;
    bool detached = false;
    bool fullscreen = false;
    bool focused = false;
};

std::vector<std::shared_ptr<ViewEntry>> g_views;
int32_t g_nextKey = 0;

Client* ClientFor(CefRefPtr<CefBrowser> browser)
{
    if (!browser)
        return nullptr;
    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    return static_cast<Client*>(client.get());
}

std::shared_ptr<ViewEntry> FindByKey(int32_t key)
{
    for (auto& entry : g_views)
    {
        if (entry->key == key)
            return entry;
    }
    return nullptr;
}

std::shared_ptr<ViewEntry> FindByElement(int parentId, const std::string& token, int32_t elementId)
{
    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId && entry->docToken == token && entry->elementId == elementId && !entry->destroyRequested)
            return entry;
    }
    return nullptr;
}

std::shared_ptr<ViewEntry> FindByBrowser(CefRefPtr<CefBrowser> browser)
{
    if (!browser)
        return nullptr;

    Client* client = ClientFor(browser);
    for (auto& entry : g_views)
    {
        if ((entry->browser && entry->browser->IsSame(browser)) || (client && entry->client.get() == client))
            return entry;
    }
    return nullptr;
}

void RemoveEntry(const std::shared_ptr<ViewEntry>& entry)
{
    g_views.erase(std::remove(g_views.begin(), g_views.end(), entry), g_views.end());
}

CefRefPtr<CefFrame> TargetFrame(const std::shared_ptr<ViewEntry>& entry)
{
    if (entry->frame && entry->frame->IsValid())
        return entry->frame;

    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
    return host ? host->GetMainFrame() : nullptr;
}

std::string ToJson(CefRefPtr<CefDictionaryValue> dictionary)
{
    if (!dictionary)
        return "null";

    CefRefPtr<CefValue> value = CefValue::Create();
    value->SetDictionary(dictionary);
    return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

void SendEventToFrame(CefRefPtr<CefFrame> frame, const std::string& token, int32_t elementId, const std::string& type, CefRefPtr<CefDictionaryValue> detail)
{
    if (!frame || !frame->IsValid())
    {
        VLOG(1) << "Dropped view event " << type << " for element " << elementId << " (no valid frame).";
        return;
    }

    VLOG(1) << "View event " << type << " for element " << elementId << " (token = " << token << ", frame = " << frame->GetIdentifier().ToString() << ").";
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewEventMessageName);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetString(0, token);
    args->SetInt(1, elementId);
    args->SetString(2, type);
    args->SetString(3, ToJson(detail));
    frame->SendProcessMessage(PID_RENDERER, message);
}

void SendEvent(const std::shared_ptr<ViewEntry>& entry, const std::string& type, CefRefPtr<CefDictionaryValue> detail = nullptr)
{
    SendEventToFrame(TargetFrame(entry), entry->docToken, entry->elementId, type, detail);
}

CefRefPtr<CefDictionaryValue> ErrorDetail(const std::string& reason)
{
    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("reason", reason);
    return detail;
}

void DetachEntry(int32_t key)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry || entry->detached)
        return;

    entry->detached = true;
    std::unique_ptr<ViewSurface> surface = std::move(entry->surface);
    CefRefPtr<CefBrowserView> browserView = std::move(entry->browserView);
    entry->window = nullptr;
    if (surface)
        surface->Detach();
    surface.reset();
    browserView = nullptr;

    if (!entry->browser)
        RemoveEntry(entry);
}

void DetachNow(const std::shared_ptr<ViewEntry>& entry)
{
    entry->destroyRequested = true;
    DetachEntry(entry->key);
}

void Destroy(const std::shared_ptr<ViewEntry>& entry)
{
    if (entry->detached)
        return;

    entry->destroyRequested = true;
    CefPostTask(TID_UI, base::BindOnce(&DetachEntry, entry->key));
}

void DestroyWhere(const std::function<bool(const ViewEntry&)>& predicate, bool immediate)
{
    std::vector<std::shared_ptr<ViewEntry>> matches;
    for (auto& entry : g_views)
    {
        if (predicate(*entry))
            matches.push_back(entry);
    }

    for (auto& entry : matches)
    {
        if (immediate)
            DetachNow(entry);
        else
            Destroy(entry);
    }
}

void CheckGuestAttachment(int32_t key, int remaining)
{
    auto entry = FindByKey(key);
    if (!entry || entry->detached || entry->destroyRequested || !entry->browser)
        return;
    if (GetGuestApi().isAttached(entry->viewId))
    {
        SendEvent(entry, "guestattached");
        return;
    }
    if (remaining > 0)
        CefPostDelayedTask(TID_UI, base::BindOnce(&CheckGuestAttachment, key, remaining - 1), 16);
    else
    {
        SendEvent(entry, "error", ErrorDetail("surface-attach-timeout"));
        Destroy(entry);
    }
}

void OnHandshakeResult(int32_t key, bool allow)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry || entry->handshakeDone)
        return;

    entry->handshakeDone = true;
    if (entry->destroyRequested || entry->detached || !entry->browser)
    {
        Destroy(entry);
        return;
    }

    if (!allow)
    {
        LOG(WARNING) << "View " << entry->viewId << " was not allowed by the controller.";
        SendEvent(entry, "error", ErrorDetail("create-failed"));
        Destroy(entry);
        return;
    }

    entry->allowed = true;

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetInt("viewId", entry->viewId);
    detail->SetString("backend", "guest");
    char contentId[128] = {};
    if (!GetGuestApi().getId(entry->viewId, entry->parentId, contentId, sizeof(contentId)))
    {
        SendEvent(entry, "error", ErrorDetail("surface-registration-failed"));
        Destroy(entry);
        return;
    }
    detail->SetString("guestContentId", contentId);
    CefPostDelayedTask(TID_UI, base::BindOnce(&CheckGuestAttachment, entry->key, 300), 16);
    SendEvent(entry, "viewcreated", detail);

    entry->browser->GetMainFrame()->LoadURL(entry->pendingSrc);
}

void StartHandshake(const std::shared_ptr<ViewEntry>& entry)
{
    const int32_t key = entry->key;
    if (!IPC::Singleton.IsAvailable())
    {
        CefPostTask(TID_UI, base::BindOnce(&OnHandshakeResult, key, true));
        return;
    }

    IPC::Singleton.WindowViewCreated(entry->parentId, entry->viewId, entry->pendingSrc,
                                     [key](bool allow)
                                     {
                                         CefPostTask(TID_UI, base::BindOnce(&OnHandshakeResult, key, allow));
                                     });
}

cef_color_t ToOpaqueColor(int32_t argb)
{
    const uint32_t value = static_cast<uint32_t>(argb);
    if ((value >> 24) == 0)
        return CefColorSetARGB(255, 255, 255, 255);
    return CefColorSetARGB(255, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

void CreateView(CefRefPtr<CefBrowser> host, CefRefPtr<CefFrame> frame, const std::string& token, int32_t elementId, const std::string& src, int32_t background)
{
    const int parentId = host->GetIdentifier();
    Client* hostClient = ClientFor(host);

    if (std::shared_ptr<ViewEntry> existing = FindByElement(parentId, token, elementId))
    {
        if (existing->pendingSrc != src && IsAllowedViewSource(src))
        {
            existing->pendingSrc = src;
            if (existing->allowed && existing->browser)
                existing->browser->GetMainFrame()->LoadURL(src);
        }
        return;
    }

    if (!IsAllowedViewSource(src))
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("denied"));
        return;
    }

    int count = 0;
    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId && entry->docToken == token && !entry->destroyRequested)
            ++count;
    }
    if (count >= kViewMaxPerDocument)
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("limit"));
        return;
    }

    CefRefPtr<CefBrowserView> hostView = CefBrowserView::GetForBrowser(host);
    CefRefPtr<CefWindow> window = hostView ? hostView->GetWindow() : nullptr;
    if (!GetGuestApi().getId || !GetGuestApi().detach || !GetGuestApi().isAttached || !GetGuestApi().focus || !GetGuestApi().transformRect)
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("guest-extension-unavailable"));
        return;
    }
    if (!window || !hostClient)
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("unsupported"));
        return;
    }

    IPCWindowCreate settings = hostClient->settings;
    settings.bridgeEnabled = false;
    settings.viewsEnabled = false;
    settings.proxyRequests = false;
    settings.modifyRequests = false;
    settings.modifyRequestBody = false;
    settings.fullscreen = false;
    settings.shown = true;
    settings.title = std::nullopt;
    settings.iconPath = std::nullopt;
    settings.url = src;

    auto entry = std::make_shared<ViewEntry>();
    entry->key = ++g_nextKey;
    entry->parentId = parentId;
    entry->frame = frame;
    entry->frameId = frame->GetIdentifier();
    entry->docToken = token;
    entry->elementId = elementId;
    entry->window = window;
    entry->pendingSrc = src;
    entry->client = new ViewClient(settings);

    CefBrowserSettings browserSettings;
    browserSettings.background_color = ToOpaqueColor(background);
    CefRefPtr<CefDictionaryValue> extraInfo = CefDictionaryValue::Create();
    entry->browserView = CefBrowserView::CreateBrowserView(entry->client, "", browserSettings, extraInfo, nullptr, new ViewBrowserViewDelegate(settings));
    if (!entry->browserView)
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("create-failed"));
        return;
    }

    entry->surface = std::make_unique<ViewSurface>(window, entry->browserView);

    g_views.push_back(entry);
    entry->surface->Attach();
}

bool IsHostAuthorized(CefRefPtr<CefBrowser> browser)
{
    Client* client = ClientFor(browser);
    return client && client->settings.viewsEnabled && client->IsPrimaryBrowser(browser);
}

}

void ConfigureHostWindowLayout(CefRefPtr<CefWindow> window, CefRefPtr<CefBrowserView> browserView)
{
    if (!window || !browserView)
        return;

    CefBoxLayoutSettings settings;
    settings.horizontal = false;
    settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
    CefRefPtr<CefBoxLayout> layout = window->SetToBoxLayout(settings);
    layout->SetFlexForView(browserView, 1);
}

bool HandleHostProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message)
{
    CEF_REQUIRE_UI_THREAD();

    const std::string name = message->GetName();
    if (name.rfind(kViewMessagePrefix, 0) != 0)
        return false;

    if (!browser || !frame || !frame->IsMain())
        return true;

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!args || args->GetSize() < 1 || args->GetType(0) != VTYPE_STRING)
        return true;

    const int parentId = browser->GetIdentifier();
    const std::string token = args->GetString(0);

    if (name == kViewContextReleasedMessageName)
    {
        DestroyWhere(
            [&](const ViewEntry& entry)
            {
                return entry.parentId == parentId && entry.docToken == token;
            },
            false);
        return true;
    }

    if (args->GetSize() < 2 || args->GetType(1) != VTYPE_INT)
        return true;

    const int32_t elementId = args->GetInt(1);

    if (name == kViewCreateMessageName)
    {
        if (!IsHostAuthorized(browser))
        {
            LOG(WARNING) << "Rejected view creation from browser " << parentId << ".";
            SendEventToFrame(frame, token, elementId, "error", ErrorDetail("denied"));
            return true;
        }

        if (args->GetSize() < 4)
            return true;

        CreateView(browser, frame, token, elementId, args->GetString(2), args->GetInt(3));
        return true;
    }

    std::shared_ptr<ViewEntry> entry = FindByElement(parentId, token, elementId);
    if (!entry)
        return true;

    if (name == kViewNavigateMessageName)
    {
        const std::string src = args->GetSize() > 2 ? args->GetString(2).ToString() : "";
        if (!IsAllowedViewSource(src))
        {
            SendEvent(entry, "error", ErrorDetail("denied"));
            return true;
        }

        entry->pendingSrc = src;
        if (entry->allowed && entry->browser)
            entry->browser->GetMainFrame()->LoadURL(src);
        return true;
    }

    if (name == kViewCommandMessageName)
    {
        const std::string command = args->GetSize() > 2 ? args->GetString(2).ToString() : "";
        if (command == "focus")
        {
            FocusView(entry->browser);
            return true;
        }

        if (!entry->allowed || !entry->browser)
            return true;

        if (command == "respond" && args->GetSize() > 3 && args->GetType(3) == VTYPE_DICTIONARY)
        {
            auto answer = args->GetDictionary(3);
            entry->client->Features()->Answer(entry->browser->GetIdentifier(), answer->GetInt("requestId"), answer);
        }
        else if (command == "exitFullscreen")
            entry->browser->GetHost()->ExitFullscreen(true);
        else if (command == "reload")
            entry->browser->Reload();
        else if (command == "goBack")
            entry->browser->GoBack();
        else if (command == "goForward")
            entry->browser->GoForward();
        return true;
    }

    if (name == kViewDestroyMessageName)
    {
        Destroy(entry);
        return true;
    }

    return true;
}

void OnHostWindowClosing(CefRefPtr<CefWindow> window)
{
    CEF_REQUIRE_UI_THREAD();

    DestroyWhere(
        [&](const ViewEntry& entry)
        {
            return entry.window && entry.window->IsSame(window);
        },
        true);
}

void OnHostFrameGone(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    CEF_REQUIRE_UI_THREAD();

    if (!browser || !frame)
        return;

    const int parentId = browser->GetIdentifier();
    const std::string frameId = frame->GetIdentifier();
    DestroyWhere(
        [&](const ViewEntry& entry)
        {
            return entry.parentId == parentId && entry.frameId == frameId;
        },
        false);
}

void OnHostBrowserGone(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    if (!browser)
        return;

    const int parentId = browser->GetIdentifier();
    DestroyWhere(
        [&](const ViewEntry& entry)
        {
            return entry.parentId == parentId;
        },
        true);
}

void OnHostGotFocus(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    if (!browser)
        return;

    const int parentId = browser->GetIdentifier();
    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId && entry->focused)
        {
            entry->focused = false;
            SendEvent(entry, "viewblur");
        }
    }
}

bool IsFocusInView(CefRefPtr<CefBrowser> hostBrowser)
{
    if (!hostBrowser)
        return false;

    const int parentId = hostBrowser->GetIdentifier();
    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId && entry->focused && !entry->detached)
            return true;
    }
    return false;
}

bool IsEmbeddedBrowser(CefRefPtr<CefBrowser> browser)
{
    return FindByBrowser(browser) != nullptr;
}

bool FocusView(CefRefPtr<CefBrowser> browser)
{
    auto entry = FindByBrowser(browser);
    if (!entry) return false;
    if (entry->allowed && !entry->detached && !entry->destroyRequested)
    {
        if (entry->window && entry->window->IsValid()) entry->window->RequestFocus();
        GetGuestApi().focus(entry->viewId);
    }
    return true;
}

void OnViewAfterCreated(Client* client, CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    std::shared_ptr<ViewEntry> entry;
    for (auto& candidate : g_views)
    {
        if (candidate->client.get() == client)
        {
            entry = candidate;
            break;
        }
    }

    if (!entry)
    {
        LOG(ERROR) << "View browser " << browser->GetIdentifier() << " has no host entry. Closing.";
        browser->GetHost()->CloseBrowser(true);
        return;
    }

    entry->browser = browser;
    entry->viewId = browser->GetIdentifier();

    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
    if (host)
        browser->GetHost()->SetZoomLevel(host->GetHost()->GetZoomLevel());

    if (entry->destroyRequested)
    {
        Destroy(entry);
        return;
    }

    StartHandshake(entry);
}

bool OnViewDoClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || entry->detached)
        return false;

    entry->destroyRequested = true;
    CefPostTask(TID_UI, base::BindOnce(&DetachEntry, entry->key));
    return true;
}

void OnViewBeforeClose(CefRefPtr<CefBrowser> browser)
{
    CEF_REQUIRE_UI_THREAD();

    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return;

    if (entry->allowed)
    {
        CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
        detail->SetInt("viewId", entry->viewId);
        SendEvent(entry, "close", detail);
    }

    entry->detached = true;
    entry->browser = nullptr;
    std::unique_ptr<ViewSurface> surface = std::move(entry->surface);
    CefRefPtr<CefBrowserView> browserView = std::move(entry->browserView);
    entry->window = nullptr;
    RemoveEntry(entry);
    if (surface)
        surface->Detach();
}

void OnViewLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !frame || !frame->IsMain() || !entry->allowed)
        return;

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("url", frame->GetURL());
    detail->SetInt("httpStatusCode", httpStatusCode);
    SendEvent(entry, "load", detail);
}

void OnViewLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int errorCode, const std::string& errorText, const std::string& failedUrl)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !frame || !frame->IsMain() || !entry->allowed || errorCode == ERR_ABORTED)
        return;

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("url", failedUrl);
    detail->SetInt("errorCode", errorCode);
    detail->SetString("errorText", errorText);
    SendEvent(entry, "loaderror", detail);
}

void OnViewTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !entry->allowed)
        return;

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("title", title);
    SendEvent(entry, "titlechange", detail);
}

void OnViewFocusChanged(CefRefPtr<CefBrowser> browser, bool focused)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || entry->focused == focused)
        return;

    if (focused)
    {
        for (auto& other : g_views)
        {
            if (other != entry && other->parentId == entry->parentId && other->focused)
            {
                other->focused = false;
                SendEvent(other, "viewblur");
            }
        }
    }

    entry->focused = focused;
    SendEvent(entry, focused ? "viewfocus" : "viewblur");
}

void OnViewFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || entry->fullscreen == fullscreen || !entry->window)
        return;

    entry->fullscreen = fullscreen;
    auto detail = CefDictionaryValue::Create();
    detail->SetBool("fullscreen", fullscreen);
    SendEvent(entry, "fullscreenchange", detail);

}

bool OnViewBrowserRequest(CefRefPtr<CefBrowser> browser, const std::string& kind, CefRefPtr<CefDictionaryValue> detail)
{
    auto entry = FindByBrowser(browser);
    if (!entry)
        return false;
    SendEvent(entry, kind == "permission" ? "permissionrequest" : kind == "newWindow" ? "newwindowrequest" : kind + "request", detail);
    return true;
}

void OnViewRenderProcessTerminated(CefRefPtr<CefBrowser> browser)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return;

    SendEvent(entry, "crash");
}

void PropagateHostZoom(CefRefPtr<CefBrowser> browser, double zoomLevel)
{
    CEF_REQUIRE_UI_THREAD();
    if (!browser)
        return;

    const int parentId = browser->GetIdentifier();
    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId && entry->browser && !entry->detached && !entry->destroyRequested &&
            std::fabs(entry->browser->GetHost()->GetZoomLevel() - zoomLevel) > 0.0001)
            entry->browser->GetHost()->SetZoomLevel(zoomLevel);
    }
}

bool TranslateViewOskRect(CefRefPtr<CefBrowser> browser, int& x, int& y, int& width, int& height)
{
    return browser && GetGuestApi().transformRect &&
           GetGuestApi().transformRect(browser->GetIdentifier(), &x, &y, &width, &height);
}

}
