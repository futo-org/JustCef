#include "justcef_view_host.h"

#include "client.h"
#include "client_manager.h"
#include "ipc.h"
#include "justcef_view_client.h"
#include "justcef_view_common.h"

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_parser.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_overlay_controller.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_panel_delegate.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace justcef_view
{
namespace
{

constexpr int64_t kSnapshotAfterLoadMs = 500;
constexpr int64_t kSnapshotIntervalMs = 2000;
constexpr int64_t kZoomRecheckMs = 200;
constexpr int64_t kHostZoomSyncMs = 100;

double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class ViewBackend
{
    Holder,
    Overlay
};

ViewBackend GetViewBackend()
{
    static const ViewBackend backend = []()
    {
        const char* value = std::getenv("JUSTCEF_VIEW_BACKEND");
        if (value && std::string(value) == "overlay")
            return ViewBackend::Overlay;
        return ViewBackend::Holder;
    }();
    return backend;
}

int ToNativeWheelDelta(double pixels, bool horizontal)
{
#if defined(OS_WIN)
    UINT amount = horizontal ? 1 : 3;
    SystemParametersInfo(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &amount, 0);
    if (amount == 0 || amount == WHEEL_PAGESCROLL)
        amount = horizontal ? 1 : 3;
    return static_cast<int>(std::lround(pixels * WHEEL_DELTA / (amount * 100.0 / 3.0)));
#else
    return static_cast<int>(std::lround(pixels));
#endif
}

bool FreezeOnWindowResize()
{
#if defined(OS_MAC)
    return GetViewBackend() == ViewBackend::Overlay;
#else
    return false;
#endif
}

struct ViewFrame
{
    CefRect full;
    CefRect clip;
    CefPoint contentOrigin;
    ViewAnchor anchorH = ViewAnchor::Start;
    ViewAnchor anchorV = ViewAnchor::Start;
    CefSize container;
};

bool SameFrame(const ViewFrame& a, const ViewFrame& b)
{
    return a.full == b.full && a.clip == b.clip && a.anchorH == b.anchorH && a.anchorV == b.anchorV && a.container.width == b.container.width &&
           a.container.height == b.container.height;
}

ViewAnchor ToAnchor(int32_t value)
{
    switch (value)
    {
    case static_cast<int32_t>(ViewAnchor::End):
        return ViewAnchor::End;
    case static_cast<int32_t>(ViewAnchor::Stretch):
        return ViewAnchor::Stretch;
    case static_cast<int32_t>(ViewAnchor::Center):
        return ViewAnchor::Center;
    default:
        return ViewAnchor::Start;
    }
}

class TransparentPanelDelegate : public CefPanelDelegate
{
public:
    TransparentPanelDelegate() = default;

    void OnThemeChanged(CefRefPtr<CefView> view) override { view->SetBackgroundColor(0); }

    CefSize GetPreferredSize(CefRefPtr<CefView> view) override { return preferred_; }

    void SetPreferredSize(const CefSize& size) { preferred_ = size; }

private:
    CefSize preferred_;

    IMPLEMENT_REFCOUNTING(TransparentPanelDelegate);
    DISALLOW_COPY_AND_ASSIGN(TransparentPanelDelegate);
};

void ApplyClipLayout(CefRefPtr<CefPanel> clip, const ViewFrame& frame)
{
    const CefRect& c = frame.clip;
    const CefRect& f = frame.full;

    const CefInsets insets(f.y - c.y, f.x - c.x, (c.y + c.height) - (f.y + f.height), (c.x + c.width) - (f.x + f.width));
    if (clip->GetInsets() != insets)
        clip->SetInsets(insets);
}

class ViewSurface
{
public:
    virtual ~ViewSurface() = default;
    virtual void Attach() = 0;
    virtual void Apply(const ViewFrame& frame) = 0;
    virtual void SetVisible(bool visible) = 0;
    virtual void SetOrder(int index) {}
    virtual void OnContainerLayout(const CefSize& container) {}
    virtual void Detach() = 0;
};

class HolderSurface : public ViewSurface
{
public:
    HolderSurface(CefRefPtr<CefWindow> window, CefRefPtr<CefBrowserView> browserView) : window_(window), browserView_(browserView) {}

    void Attach() override
    {
        holderDelegate_ = new TransparentPanelDelegate();
        clipDelegate_ = new TransparentPanelDelegate();
        holder_ = CefPanel::CreatePanel(holderDelegate_);
        clip_ = CefPanel::CreatePanel(clipDelegate_);
        holder_->SetBackgroundColor(0);
        clip_->SetBackgroundColor(0);
        clip_->AddChildView(browserView_);
        holder_->AddChildView(clip_);
        holder_->SetVisible(false);
        window_->AddChildView(holder_);
    }

    void Apply(const ViewFrame& frame) override
    {
        if (!holder_ || !clip_ || (hasFrame_ && SameFrame(frame, frame_)))
            return;

        const bool configureLayout = !hasFrame_ || frame.anchorH != frame_.anchorH || frame.anchorV != frame_.anchorV;
        frame_ = frame;
        hasFrame_ = true;

        const CefRect& c = frame.clip;
        const int width = frame.container.width;
        const int height = frame.container.height;

        CefBoxLayoutSettings settings;
        settings.horizontal = true;
        int top = 0, left = 0, bottom = 0, right = 0, flex = 0;

        switch (frame.anchorH)
        {
        case ViewAnchor::End:
            settings.main_axis_alignment = CEF_AXIS_ALIGNMENT_END;
            right = width - (c.x + c.width);
            break;
        case ViewAnchor::Stretch:
            settings.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
            left = c.x;
            right = width - (c.x + c.width);
            flex = 1;
            break;
        case ViewAnchor::Center:
        {
            settings.main_axis_alignment = CEF_AXIS_ALIGNMENT_CENTER;
            const int delta = 2 * c.x + c.width - width;
            left = std::max(0, delta);
            right = std::max(0, -delta);
            break;
        }
        default:
            settings.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
            left = c.x;
            break;
        }

        switch (frame.anchorV)
        {
        case ViewAnchor::End:
            settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_END;
            bottom = height - (c.y + c.height);
            break;
        case ViewAnchor::Stretch:
            settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
            top = c.y;
            bottom = height - (c.y + c.height);
            break;
        case ViewAnchor::Center:
        {
            settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_CENTER;
            const int delta = 2 * c.y + c.height - height;
            top = std::max(0, delta);
            bottom = std::max(0, -delta);
            break;
        }
        default:
            settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_START;
            top = c.y;
            break;
        }

        const CefInsets insets(std::max(0, top), std::max(0, left), std::max(0, bottom), std::max(0, right));
        if (holder_->GetInsets() != insets)
            holder_->SetInsets(insets);
        clipDelegate_->SetPreferredSize(CefSize(c.width, c.height));
        if (configureLayout)
        {
            CefRefPtr<CefBoxLayout> layout = holder_->SetToBoxLayout(settings);
            if (layout)
                layout->SetFlexForView(clip_, flex);
        }

        ApplyClipLayout(clip_, frame);
        holder_->InvalidateLayout();
        holder_->Layout();
        clip_->Layout();
    }

    void SetVisible(bool visible) override
    {
        if (holder_)
            holder_->SetVisible(visible);
    }

    void SetOrder(int index) override
    {
        if (!window_ || !holder_ || !holder_->IsAttached())
            return;

        const int count = static_cast<int>(window_->GetChildViewCount());
        window_->ReorderChildView(holder_, std::max(1, std::min(index, count - 1)));
    }

    void Detach() override
    {
        CefRefPtr<CefWindow> window = std::move(window_);
        CefRefPtr<CefPanel> holder = std::move(holder_);
        CefRefPtr<CefPanel> clip = std::move(clip_);
        CefRefPtr<CefBrowserView> browserView = std::move(browserView_);
        holderDelegate_ = nullptr;
        clipDelegate_ = nullptr;

        if (window && holder && holder->IsValid() && holder->IsAttached())
            window->RemoveChildView(holder);
    }

private:
    CefRefPtr<CefWindow> window_;
    CefRefPtr<CefBrowserView> browserView_;
    CefRefPtr<CefPanel> holder_;
    CefRefPtr<CefPanel> clip_;
    CefRefPtr<TransparentPanelDelegate> holderDelegate_;
    CefRefPtr<TransparentPanelDelegate> clipDelegate_;
    ViewFrame frame_;
    bool hasFrame_ = false;
};

class OverlaySurface : public ViewSurface
{
public:
    OverlaySurface(CefRefPtr<CefWindow> window, CefRefPtr<CefBrowserView> browserView) : window_(window), browserView_(browserView) {}

    void Attach() override
    {
        clipDelegate_ = new TransparentPanelDelegate();
        clip_ = CefPanel::CreatePanel(clipDelegate_);
        clip_->SetBackgroundColor(0);
        clip_->AddChildView(browserView_);
        controller_ = window_->AddOverlayView(clip_, CEF_DOCKING_MODE_CUSTOM, true);
        if (controller_)
            controller_->SetVisible(false);
    }

    void Apply(const ViewFrame& frame) override
    {
        frame_ = frame;
        hasFrame_ = true;
        ApplyInternal(frame);
    }

    void OnContainerLayout(const CefSize& container) override
    {
        if (!hasFrame_ || (container.width == frame_.container.width && container.height == frame_.container.height))
            return;

        const int dx = container.width - frame_.container.width;
        const int dy = container.height - frame_.container.height;
        ViewFrame predicted = frame_;
        Shift(predicted.full, predicted.anchorH, predicted.anchorV, dx, dy);
        Shift(predicted.clip, predicted.anchorH, predicted.anchorV, dx, dy);
        predicted.container = container;
        frame_ = predicted;
        ApplyInternal(predicted);
    }

    void SetVisible(bool visible) override
    {
        if (controller_ && controller_->IsValid())
            controller_->SetVisible(visible);
    }

    void Detach() override
    {
        CefRefPtr<CefOverlayController> controller = std::move(controller_);
        CefRefPtr<CefPanel> clip = std::move(clip_);
        CefRefPtr<CefBrowserView> browserView = std::move(browserView_);
        CefRefPtr<CefWindow> window = std::move(window_);
        clipDelegate_ = nullptr;

        if (controller && controller->IsValid())
            controller->Destroy();
    }

private:
    static void Shift(CefRect& rect, ViewAnchor anchorH, ViewAnchor anchorV, int dx, int dy)
    {
        switch (anchorH)
        {
        case ViewAnchor::End:
            rect.x += dx;
            break;
        case ViewAnchor::Stretch:
            rect.width = std::max(0, rect.width + dx);
            break;
        case ViewAnchor::Center:
            rect.x += dx / 2;
            break;
        default:
            break;
        }

        switch (anchorV)
        {
        case ViewAnchor::End:
            rect.y += dy;
            break;
        case ViewAnchor::Stretch:
            rect.height = std::max(0, rect.height + dy);
            break;
        case ViewAnchor::Center:
            rect.y += dy / 2;
            break;
        default:
            break;
        }
    }

    void ApplyInternal(const ViewFrame& frame)
    {
        if (!controller_ || !controller_->IsValid() || frame.clip.width <= 0 || frame.clip.height <= 0)
            return;

        ApplyClipLayout(clip_, frame);
        controller_->SetBounds(frame.clip);
        clip_->Layout();
    }

    CefRefPtr<CefWindow> window_;
    CefRefPtr<CefBrowserView> browserView_;
    CefRefPtr<CefPanel> clip_;
    CefRefPtr<TransparentPanelDelegate> clipDelegate_;
    CefRefPtr<CefOverlayController> controller_;
    ViewFrame frame_;
    bool hasFrame_ = false;
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
    bool loadStarted = false;
    bool destroyRequested = false;
    bool detached = false;
    bool hasUpdate = false;
    bool refreshPending = false;
    ViewUpdate update;
    int32_t derivedZoomSeq = -1;
    bool zoomRecheckPending = false;
    bool hasFrame = false;
    ViewFrame lastFrame;
    int appliedOrder = -1;
    bool visibleApplied = false;
    bool fullscreen = false;
    bool restoreWindowFullscreen = false;
    bool captureInFlight = false;
    bool snapshotScheduled = false;
    bool focused = false;
    double wheelRemainderX = 0;
    double wheelRemainderY = 0;
    int32_t wheelToken = 0;
    std::deque<std::array<double, 3>> wheelSamples;
    CefPoint wheelPoint;
    bool momentumActive = false;
    double momentumVelocityX = 0;
    double momentumVelocityY = 0;
    double momentumLastTime = 0;
    double momentumStartTime = 0;
    double lastInjectTime = 0;
};

std::vector<std::shared_ptr<ViewEntry>> g_views;
std::unordered_set<int> g_hostDialogsOpen;
std::unordered_set<int> g_hostZoomSyncPending;
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

void Refresh(const std::shared_ptr<ViewEntry>& entry);
void Capture(const std::shared_ptr<ViewEntry>& entry);
void OnHostActivity(CefRefPtr<CefBrowser> host, const std::string& kind);

void SyncHostZoom(int parentId)
{
    g_hostZoomSyncPending.erase(parentId);
    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(parentId);
    if (host)
        PropagateHostZoom(host, host->GetHost()->GetZoomLevel());
}

void RefreshLatest(int32_t key)
{
    auto entry = FindByKey(key);
    if (!entry)
        return;
    entry->refreshPending = false;
    Refresh(entry);
}

void DetachEntry(int32_t key)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry || entry->detached)
        return;

    entry->detached = true;
    entry->visibleApplied = false;
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
    if (entry->surface)
        entry->surface->SetVisible(false);
    entry->visibleApplied = false;
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
    detail->SetString("resizeMode", FreezeOnWindowResize() ? "freeze" : "anchor");
    SendEvent(entry, "viewcreated", detail);

    Refresh(entry);
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

void OnZoomRecheck(int32_t key, int32_t seq)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry)
        return;

    entry->zoomRecheckPending = false;
    if (entry->update.seq != seq)
        return;

    entry->derivedZoomSeq = seq;
    Refresh(entry);
}

int RoundedSize(double size, double zoom)
{
    return std::max(0, static_cast<int>(std::lround(size * zoom)));
}

int CenteredStart(double start, double size, double zoom, int roundedSize)
{
    return static_cast<int>(std::lround((start + size / 2) * zoom - roundedSize / 2.0));
}

int ClipEdge(double clipEdge, double fullEdge, int roundedFullEdge, double zoom)
{
    if (std::fabs(clipEdge - fullEdge) < 0.01)
        return roundedFullEdge;
    return static_cast<int>(std::lround(clipEdge * zoom));
}

std::optional<ViewFrame> ComputeFrame(const std::shared_ptr<ViewEntry>& entry, CefRefPtr<CefBrowser> host, const CefRect& hostBounds, const CefSize& container)
{
    const ViewUpdate& u = entry->update;
    double zoom = std::pow(1.2, host->GetHost()->GetZoomLevel());

    if (u.innerWidth > 0 && zoom > 0)
    {
        const double expected = hostBounds.width / zoom;
        if (std::fabs(expected - u.innerWidth) > 1.5)
        {
            if (entry->derivedZoomSeq != u.seq)
            {
                if (!entry->zoomRecheckPending)
                {
                    entry->zoomRecheckPending = true;
                    CefPostDelayedTask(TID_UI, base::BindOnce(&OnZoomRecheck, entry->key, u.seq), kZoomRecheckMs);
                }
                return std::nullopt;
            }

            zoom = hostBounds.width / u.innerWidth;
        }
    }

    int offsetY = 0;
    if (u.innerHeight > 0)
    {
        const double delta = hostBounds.height - u.innerHeight * zoom;
        if (delta > 2.0)
            offsetY = static_cast<int>(std::lround(delta));
    }

    const int fullWidth = RoundedSize(u.width, zoom);
    const int fullHeight = RoundedSize(u.height, zoom);
    const int fullLeft = CenteredStart(u.x, u.width, zoom, fullWidth);
    const int fullTop = CenteredStart(u.y, u.height, zoom, fullHeight);
    const int fullRight = fullLeft + fullWidth;
    const int fullBottom = fullTop + fullHeight;

    const int clipLeft = ClipEdge(u.clipX, u.x, fullLeft, zoom);
    const int clipTop = ClipEdge(u.clipY, u.y, fullTop, zoom);
    const int clipRight = ClipEdge(u.clipX + u.clipWidth, u.x + u.width, fullRight, zoom);
    const int clipBottom = ClipEdge(u.clipY + u.clipHeight, u.y + u.height, fullBottom, zoom);

    const int originX = hostBounds.x;
    const int originY = hostBounds.y + offsetY;

    ViewFrame frame;
    frame.full = CefRect(originX + fullLeft, originY + fullTop, fullWidth, fullHeight);
    frame.contentOrigin = CefPoint(fullLeft, fullTop);

    const int left = std::max({originX + clipLeft, frame.full.x, 0});
    const int top = std::max({originY + clipTop, frame.full.y, 0});
    const int right = std::min({originX + clipRight, frame.full.x + frame.full.width, container.width});
    const int bottom = std::min({originY + clipBottom, frame.full.y + frame.full.height, container.height});
    frame.clip = CefRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
    frame.anchorH = ToAnchor(u.anchorH);
    frame.anchorV = ToAnchor(u.anchorV);
    frame.container = container;
    return frame;
}

void ScheduleSnapshot(const std::shared_ptr<ViewEntry>& entry, int64_t delayMs);

void Refresh(const std::shared_ptr<ViewEntry>& entry)
{
    if (!entry->surface || entry->detached)
        return;

    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
    CefRefPtr<CefBrowserView> hostView = host ? CefBrowserView::GetForBrowser(host) : nullptr;
    if (!hostView)
        return;

    const CefRect hostBounds = hostView->GetBounds();
    const CefSize container(hostBounds.x + hostBounds.width, hostBounds.y + hostBounds.height);
    const bool canShow = entry->allowed && entry->loadStarted && g_hostDialogsOpen.find(entry->parentId) == g_hostDialogsOpen.end();
    bool visible = canShow;

    if (entry->fullscreen)
    {
        ViewFrame frame;
        frame.full = CefRect(0, 0, container.width, container.height);
        frame.clip = frame.full;
        frame.anchorH = ViewAnchor::Stretch;
        frame.anchorV = ViewAnchor::Stretch;
        frame.container = container;
        entry->surface->Apply(frame);
        entry->lastFrame = frame;
        entry->hasFrame = true;
    }
    else if (entry->hasUpdate && entry->update.frozen)
    {
        visible = false;
    }
    else if (entry->hasUpdate)
    {
        visible = visible && entry->update.visible;
        if (visible)
        {
            std::optional<ViewFrame> frame = ComputeFrame(entry, host, hostBounds, container);
            if (frame && (frame->clip.width <= 0 || frame->clip.height <= 0))
            {
                visible = false;
            }
            else if (frame)
            {
                VLOG(1) << "View " << entry->viewId << " frame full = (" << frame->full.x << ", " << frame->full.y << ", " << frame->full.width << ", "
                        << frame->full.height << ") clip = (" << frame->clip.x << ", " << frame->clip.y << ", " << frame->clip.width << ", " << frame->clip.height
                        << ") anchors = " << static_cast<int>(frame->anchorH) << "/" << static_cast<int>(frame->anchorV);
                entry->surface->Apply(*frame);
                entry->lastFrame = *frame;
                entry->hasFrame = true;
            }
        }

        if (!entry->hasFrame)
            visible = false;

        if (entry->update.order != entry->appliedOrder)
        {
            entry->appliedOrder = entry->update.order;
            entry->surface->SetOrder(1 + entry->update.order);
        }
    }
    else
    {
        visible = false;
    }

    if (visible != entry->visibleApplied)
    {
        VLOG(1) << "View " << entry->viewId << " visible = " << visible;
        entry->visibleApplied = visible;
        entry->surface->SetVisible(visible);
        if (visible)
            ScheduleSnapshot(entry, kSnapshotIntervalMs);
    }
}

void OnSnapshotResult(int32_t key, bool success, std::string result)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry)
        return;

    entry->captureInFlight = false;
    if (!success || entry->detached)
        return;

    CefRefPtr<CefValue> value = CefParseJSON(result, JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY)
        return;

    CefRefPtr<CefDictionaryValue> dictionary = value->GetDictionary();
    if (!dictionary->HasKey("data"))
        return;

    CefRefPtr<CefFrame> frame = TargetFrame(entry);
    if (!frame)
        return;

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewSnapshotMessageName);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetString(0, entry->docToken);
    args->SetInt(1, entry->elementId);
    args->SetString(2, dictionary->GetString("data"));
    frame->SendProcessMessage(PID_RENDERER, message);
}

void Capture(const std::shared_ptr<ViewEntry>& entry)
{
    if (!entry->browser || !entry->client || !entry->visibleApplied || entry->captureInFlight || entry->detached)
        return;

    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("format", "jpeg");
    params->SetInt("quality", 80);
    params->SetBool("optimizeForSpeed", true);
    params->SetBool("fromSurface", true);

    entry->captureInFlight = true;
    const int32_t key = entry->key;
    if (!entry->client->ExecuteDevToolsMethodAsync(entry->browser, "Page.captureScreenshot", params,
                                                   [key](bool success, std::string result)
                                                   {
                                                       OnSnapshotResult(key, success, std::move(result));
                                                   }))
    {
        entry->captureInFlight = false;
    }
}

void RunScheduledSnapshot(int32_t key)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry)
        return;

    entry->snapshotScheduled = false;
    if (entry->detached || !entry->visibleApplied)
        return;

    Capture(entry);
    ScheduleSnapshot(entry, kSnapshotIntervalMs);
}

void ScheduleSnapshot(const std::shared_ptr<ViewEntry>& entry, int64_t delayMs)
{
    if (entry->snapshotScheduled || entry->detached)
        return;

    entry->snapshotScheduled = true;
    CefPostDelayedTask(TID_UI, base::BindOnce(&RunScheduledSnapshot, entry->key), delayMs);
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
    extraInfo->SetBool(kViewContentExtraInfoKey, true);
    entry->browserView = CefBrowserView::CreateBrowserView(entry->client, "", browserSettings, extraInfo, nullptr, new ViewBrowserViewDelegate(settings));
    if (!entry->browserView)
    {
        SendEventToFrame(frame, token, elementId, "error", ErrorDetail("create-failed"));
        return;
    }

    if (GetViewBackend() == ViewBackend::Overlay)
        entry->surface = std::make_unique<OverlaySurface>(window, entry->browserView);
    else
        entry->surface = std::make_unique<HolderSurface>(window, entry->browserView);

    g_views.push_back(entry);
    entry->surface->Attach();
}

bool IsHostAuthorized(CefRefPtr<CefBrowser> browser)
{
    Client* client = ClientFor(browser);
    return client && client->settings.viewsEnabled && client->IsPrimaryBrowser(browser);
}

std::string KeyName(const CefKeyEvent& event)
{
    const int code = event.windows_key_code;
    const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    if (code >= 0x41 && code <= 0x5A)
        return std::string(1, static_cast<char>(shift ? code : code + 32));
    if (code >= 0x30 && code <= 0x39)
        return std::string(1, static_cast<char>(code));
    if (code >= 0x70 && code <= 0x87)
        return "F" + std::to_string(code - 0x6F);

    switch (code)
    {
    case 0x08:
        return "Backspace";
    case 0x09:
        return "Tab";
    case 0x0D:
        return "Enter";
    case 0x1B:
        return "Escape";
    case 0x20:
        return " ";
    case 0x21:
        return "PageUp";
    case 0x22:
        return "PageDown";
    case 0x23:
        return "End";
    case 0x24:
        return "Home";
    case 0x25:
        return "ArrowLeft";
    case 0x26:
        return "ArrowUp";
    case 0x27:
        return "ArrowRight";
    case 0x28:
        return "ArrowDown";
    case 0x2D:
        return "Insert";
    case 0x2E:
        return "Delete";
    default:
        return "Unidentified";
    }
}

std::string KeyCode(const CefKeyEvent& event)
{
    const int code = event.windows_key_code;
    if (code >= 0x41 && code <= 0x5A)
        return std::string("Key") + static_cast<char>(code);
    if (code >= 0x30 && code <= 0x39)
        return std::string("Digit") + static_cast<char>(code);
    if (code >= 0x70 && code <= 0x87)
        return "F" + std::to_string(code - 0x6F);

    const std::string name = KeyName(event);
    return name == " " ? "Space" : name;
}

} // namespace

bool HandleHostProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message)
{
    CEF_REQUIRE_UI_THREAD();

    const std::string name = message->GetName();
    if (name.rfind(kViewMessagePrefix, 0) != 0)
        return false;

    if (!browser || !frame || !frame->IsMain())
        return true;

    if (name == kViewPongMessageName)
    {
        VLOG(1) << "Host " << browser->GetIdentifier() << " answered the dialog ping.";
        OnHostDialogStateChanged(browser, false);
        return true;
    }

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!args || args->GetSize() < 1 || args->GetType(0) != VTYPE_STRING)
        return true;

    const int parentId = browser->GetIdentifier();
    const std::string token = args->GetString(0);

    if (name == kViewHostActivityMessageName)
    {
        if (args->GetSize() >= 2 && args->GetType(1) == VTYPE_STRING)
            OnHostActivity(browser, args->GetString(1).ToString());
        return true;
    }

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

    if (name == kViewUpdateMessageName)
    {
        ViewUpdate update;
        if (!ReadViewUpdate(args, 2, update))
            return true;
        const uint32_t distance = static_cast<uint32_t>(update.seq) - static_cast<uint32_t>(entry->update.seq);
        if (entry->hasUpdate && (distance == 0 || distance >= 0x80000000u))
            return true;

        entry->update = update;
        entry->hasUpdate = true;
        if (!entry->refreshPending)
        {
            entry->refreshPending = true;
            CefPostTask(TID_UI, base::BindOnce(&RefreshLatest, entry->key));
        }
        return true;
    }

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
            if (entry->browserView)
                entry->browserView->RequestFocus();
            return true;
        }

        if (command == "snapshot")
        {
            Capture(entry);
            return true;
        }

        if (!entry->allowed || !entry->browser)
            return true;

        if (command == "reload")
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

void OnHostWindowLayoutChanged(CefRefPtr<CefWindow> window)
{
    CEF_REQUIRE_UI_THREAD();

    for (auto& entry : g_views)
    {
        if (!entry->surface || entry->detached || !entry->window || !entry->window->IsSame(window))
            continue;

        CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
        CefRefPtr<CefBrowserView> hostView = host ? CefBrowserView::GetForBrowser(host) : nullptr;
        if (!hostView)
            continue;

        const CefRect hostBounds = hostView->GetBounds();
        entry->surface->OnContainerLayout(CefSize(hostBounds.x + hostBounds.width, hostBounds.y + hostBounds.height));
        if (entry->fullscreen)
            Refresh(entry);
    }
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
    g_hostDialogsOpen.erase(parentId);
    DestroyWhere(
        [&](const ViewEntry& entry)
        {
            return entry.parentId == parentId;
        },
        true);
}

void OnHostDialogStateChanged(CefRefPtr<CefBrowser> browser, bool open)
{
    CEF_REQUIRE_UI_THREAD();

    if (!browser)
        return;

    const int parentId = browser->GetIdentifier();
    const bool wasOpen = g_hostDialogsOpen.find(parentId) != g_hostDialogsOpen.end();
    if (wasOpen == open)
        return;

    if (open)
    {
        g_hostDialogsOpen.insert(parentId);
        VLOG(1) << "Host " << parentId << " opened a dialog.";
        if (CefRefPtr<CefFrame> frame = browser->GetMainFrame())
            frame->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create(kViewPingMessageName));
    }
    else
    {
        g_hostDialogsOpen.erase(parentId);
    }

    for (auto& entry : g_views)
    {
        if (entry->parentId == parentId)
            Refresh(entry);
    }
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
    if (entry->surface)
        entry->surface->SetVisible(false);
    entry->visibleApplied = false;
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

void OnViewLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !frame || !frame->IsMain() || !entry->allowed)
        return;

    if (!entry->loadStarted)
    {
        entry->loadStarted = true;
        Refresh(entry);
    }
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
    ScheduleSnapshot(entry, kSnapshotAfterLoadMs);
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

void OnViewTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return;

    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
    CefRefPtr<CefBrowserView> hostView = host ? CefBrowserView::GetForBrowser(host) : nullptr;
    if (hostView)
        hostView->RequestFocus();

    if (entry->focused)
    {
        entry->focused = false;
        SendEvent(entry, "viewblur");
    }

    CefRefPtr<CefFrame> frame = TargetFrame(entry);
    if (!frame)
        return;

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewTakeFocusMessageName);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetString(0, entry->docToken);
    args->SetInt(1, entry->elementId);
    args->SetBool(2, next);
    frame->SendProcessMessage(PID_RENDERER, message);
}

bool OnViewKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event)
{
    if (event.type != KEYEVENT_RAWKEYDOWN)
        return false;

    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return false;

    const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
    const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
    const bool meta = (event.modifiers & EVENTFLAG_COMMAND_DOWN) != 0;
    const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    const bool functionKey = event.windows_key_code >= 0x70 && event.windows_key_code <= 0x87;
    const bool escape = event.windows_key_code == 0x1B;

    if (escape && entry->fullscreen && entry->browser)
    {
        entry->browser->GetHost()->ExitFullscreen(true);
        return true;
    }

    if (!ctrl && !alt && !meta && !functionKey && !escape)
        return false;

    CefRefPtr<CefDictionaryValue> modifiers = CefDictionaryValue::Create();
    modifiers->SetBool("ctrl", ctrl);
    modifiers->SetBool("alt", alt);
    modifiers->SetBool("meta", meta);
    modifiers->SetBool("shift", shift);

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("key", KeyName(event));
    detail->SetString("code", KeyCode(event));
    detail->SetInt("keyCode", event.windows_key_code);
    detail->SetDictionary("modifiers", modifiers);
    SendEvent(entry, "viewkeydown", detail);
    return true;
}

void OnViewFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || entry->fullscreen == fullscreen || !entry->window)
        return;

    entry->fullscreen = fullscreen;
    if (fullscreen)
    {
        entry->restoreWindowFullscreen = !entry->window->IsFullscreen();
        if (entry->restoreWindowFullscreen)
            entry->window->SetFullscreen(true);
    }
    else if (entry->restoreWindowFullscreen)
    {
        entry->restoreWindowFullscreen = false;
        entry->window->SetFullscreen(false);
    }

    Refresh(entry);
}

void OnViewPermissionRequest(CefRefPtr<CefBrowser> browser, const std::string& origin, const std::string& permissions)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return;

    CefRefPtr<CefDictionaryValue> detail = CefDictionaryValue::Create();
    detail->SetString("origin", origin);
    detail->SetString("permissions", permissions);
    SendEvent(entry, "permissionrequest", detail);
}

void OnViewRenderProcessTerminated(CefRefPtr<CefBrowser> browser)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry)
        return;

    SendEvent(entry, "crash");
}

namespace
{

constexpr int64_t kWheelIdleMs = 45;
constexpr int64_t kMomentumFrameMs = 16;
constexpr double kMomentumSampleWindow = 0.1;
constexpr double kMomentumTimeConstant = 0.35;
constexpr double kMomentumMinimumStartVelocity = 250;
constexpr double kMomentumStopVelocity = 30;
constexpr double kMomentumMaximumDuration = 3.0;
constexpr int kLatchMs = 250;
constexpr double kOwnInjectionWindow = 0.1;

void InjectWheel(const std::shared_ptr<ViewEntry>& entry, double deltaX, double deltaY, bool precise)
{
    CefRefPtr<CefBrowser> host = ClientManager::GetInstance()->AcquirePointer(entry->parentId);
    if (!host)
        return;

    const double totalX = entry->wheelRemainderX + std::clamp(deltaX, -10000.0, 10000.0);
    const double totalY = entry->wheelRemainderY + std::clamp(deltaY, -10000.0, 10000.0);
    const double pixelsX = std::trunc(totalX);
    const double pixelsY = std::trunc(totalY);
    entry->wheelRemainderX = totalX - pixelsX;
    entry->wheelRemainderY = totalY - pixelsY;
    if (pixelsX == 0 && pixelsY == 0)
        return;

    CefMouseEvent event;
    event.x = entry->wheelPoint.x;
    event.y = entry->wheelPoint.y;
    event.modifiers = precise ? EVENTFLAG_PRECISION_SCROLLING_DELTA : 0;
    entry->lastInjectTime = NowSeconds();
    host->GetHost()->SendMouseWheelEvent(event, ToNativeWheelDelta(-pixelsX, true), ToNativeWheelDelta(-pixelsY, false));
}

void StopMomentum(const std::shared_ptr<ViewEntry>& entry)
{
    if (entry->momentumActive)
        VLOG(1) << "View " << entry->viewId << " momentum cancelled.";
    ++entry->wheelToken;
    entry->momentumActive = false;
    entry->wheelSamples.clear();
}

void MomentumStep(int32_t key, int32_t token)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry || !entry->momentumActive || entry->wheelToken != token || entry->detached)
        return;

    const double now = NowSeconds();
    const double dt = std::clamp(now - entry->momentumLastTime, 0.001, 0.1);
    entry->momentumLastTime = now;

    const double decay = std::exp(-dt / kMomentumTimeConstant);
    const double distanceX = entry->momentumVelocityX * kMomentumTimeConstant * (1 - decay);
    const double distanceY = entry->momentumVelocityY * kMomentumTimeConstant * (1 - decay);
    entry->momentumVelocityX *= decay;
    entry->momentumVelocityY *= decay;
    InjectWheel(entry, distanceX, distanceY, true);

    const double speed = std::hypot(entry->momentumVelocityX, entry->momentumVelocityY);
    if (speed < kMomentumStopVelocity || now - entry->momentumStartTime > kMomentumMaximumDuration)
    {
        entry->momentumActive = false;
        return;
    }

    CefPostDelayedTask(TID_UI, base::BindOnce(&MomentumStep, key, token), kMomentumFrameMs);
}

void StartMomentum(const std::shared_ptr<ViewEntry>& entry)
{
    if (entry->wheelSamples.size() < 2)
    {
        entry->wheelSamples.clear();
        return;
    }

    const double last = entry->wheelSamples.back()[0];
    if (NowSeconds() - last > 0.12)
    {
        entry->wheelSamples.clear();
        return;
    }

    const double first = entry->wheelSamples.front()[0];
    double sumX = 0;
    double sumY = 0;
    for (size_t i = 1; i < entry->wheelSamples.size(); ++i)
    {
        sumX += entry->wheelSamples[i][1];
        sumY += entry->wheelSamples[i][2];
    }
    entry->wheelSamples.clear();

    const double span = last - first;
    if (span < 0.008)
        return;

    const double velocityX = sumX / span;
    const double velocityY = sumY / span;
    if (std::hypot(velocityX, velocityY) < kMomentumMinimumStartVelocity)
        return;

    VLOG(1) << "View " << entry->viewId << " momentum started at " << velocityX << ", " << velocityY << " px/s.";
    entry->momentumActive = true;
    entry->momentumVelocityX = velocityX;
    entry->momentumVelocityY = velocityY;
    entry->momentumStartTime = entry->momentumLastTime = NowSeconds();
    CefPostDelayedTask(TID_UI, base::BindOnce(&MomentumStep, entry->key, entry->wheelToken), kMomentumFrameMs);
}

void OnWheelIdle(int32_t key, int32_t token)
{
    std::shared_ptr<ViewEntry> entry = FindByKey(key);
    if (!entry || entry->wheelToken != token || entry->momentumActive)
        return;

    StartMomentum(entry);
}

void OnHostActivity(CefRefPtr<CefBrowser> host, const std::string& kind)
{
    const int parentId = host->GetIdentifier();
    if (g_hostZoomSyncPending.insert(parentId).second)
        CefPostDelayedTask(TID_UI, base::BindOnce(&SyncHostZoom, parentId), kHostZoomSyncMs);
    const double now = NowSeconds();
    for (auto& entry : g_views)
    {
        if (entry->parentId != parentId || entry->detached)
            continue;

        if (kind == "wheel" && now - entry->lastInjectTime < kOwnInjectionWindow)
            continue;

        StopMomentum(entry);
        if (kind != "wheel" || !entry->browser || !entry->visibleApplied)
            continue;

        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewLatchMessageName);
        message->GetArgumentList()->SetInt(0, kLatchMs);
        entry->browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
    }
}

} // namespace

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

void OnViewWheel(CefRefPtr<CefBrowser> browser, double deltaX, double deltaY, double clientX, double clientY, int phase)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !entry->allowed || !entry->visibleApplied || entry->fullscreen || !entry->hasFrame)
        return;
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY) || !std::isfinite(clientX) || !std::isfinite(clientY))
        return;

    const ViewScrollPhase scrollPhase = static_cast<ViewScrollPhase>(phase);
    if (scrollPhase == ViewScrollPhase::Cancel)
    {
        StopMomentum(entry);
        return;
    }

    if (scrollPhase == ViewScrollPhase::TouchEnd)
    {
        ++entry->wheelToken;
        entry->momentumActive = false;
        StartMomentum(entry);
        return;
    }

    ++entry->wheelToken;
    entry->momentumActive = false;

    const double viewZoom = std::pow(1.2, browser->GetHost()->GetZoomLevel());
    const CefRect& full = entry->lastFrame.full;
    const double localX = std::clamp(clientX * viewZoom, 0.0, static_cast<double>(std::max(0, full.width - 1)));
    const double localY = std::clamp(clientY * viewZoom, 0.0, static_cast<double>(std::max(0, full.height - 1)));
    entry->wheelPoint = CefPoint(entry->lastFrame.contentOrigin.x + static_cast<int>(std::lround(localX)),
                                 entry->lastFrame.contentOrigin.y + static_cast<int>(std::lround(localY)));

    const bool precise = scrollPhase != ViewScrollPhase::Wheel;
    InjectWheel(entry, deltaX, deltaY, precise);

    if (!precise)
    {
        entry->wheelSamples.clear();
        return;
    }

    const double now = NowSeconds();
    entry->wheelSamples.push_back({now, deltaX, deltaY});
    while (!entry->wheelSamples.empty() && now - entry->wheelSamples.front()[0] > kMomentumSampleWindow)
        entry->wheelSamples.pop_front();

    if (scrollPhase == ViewScrollPhase::PreciseWheel)
        CefPostDelayedTask(TID_UI, base::BindOnce(&OnWheelIdle, entry->key, entry->wheelToken), kWheelIdleMs);
}

void OnViewKeyScroll(CefRefPtr<CefBrowser> browser, const std::string& kind)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !entry->allowed || entry->fullscreen)
        return;

    static const std::unordered_set<std::string> kinds = {"up", "down", "left", "right", "pageup", "pagedown", "home", "end"};
    if (kinds.find(kind) == kinds.end())
        return;

    CefRefPtr<CefFrame> frame = TargetFrame(entry);
    if (!frame)
        return;

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewKeyScrollMessageName);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetString(0, entry->docToken);
    args->SetInt(1, entry->elementId);
    args->SetString(2, kind);
    frame->SendProcessMessage(PID_RENDERER, message);
}

bool TranslateViewOskRect(CefRefPtr<CefBrowser> browser, int& x, int& y)
{
    std::shared_ptr<ViewEntry> entry = FindByBrowser(browser);
    if (!entry || !entry->hasFrame)
        return false;

    x += entry->lastFrame.full.x;
    y += entry->lastFrame.full.y;
    return true;
}

} // namespace justcef_view
