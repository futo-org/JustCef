#include "browser_features.h"
#include "screenshot_capture.h"
#include "client.h"
#include "client_manager.h"
#include "include/cef_parser.h"
#include "include/cef_request_context.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "justcef_view_host.h"

namespace
{
bool CanRememberOrigin(const std::string& origin)
{
    CefURLParts parts;
    if (!CefParseURL(origin, parts))
        return false;
    const auto scheme = CefString(&parts.scheme).ToString();
    return scheme == "https" || scheme == "http";
}
void SetMediaSetting(CefRefPtr<CefBrowser> browser, const std::string& origin, uint32_t permissions, cef_content_setting_values_t value, const std::string& topLevel = {})
{
    if (!CanRememberOrigin(origin) || (!topLevel.empty() && !CanRememberOrigin(topLevel)))
        return;
    auto context = browser->GetHost()->GetRequestContext();
    if (permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE)
        context->SetContentSetting(origin, topLevel.empty() ? origin : topLevel, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC, value);
    if (permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE)
        context->SetContentSetting(origin, topLevel.empty() ? origin : topLevel, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA, value);
}
} // namespace

void BrowserFeatures::Ask(CefRefPtr<CefBrowser> browser, const std::string& kind, CefRefPtr<CefDictionaryValue> detail, Decision callback)
{
    CEF_REQUIRE_UI_THREAD();
    const int id = ++nextId_;
    const int browserId = browser->GetIdentifier();
    detail->SetInt("requestId", id);
    pending_.emplace(id, Pending{browserId, kind == "download", detail->GetString("frameId").ToString(), std::move(callback), {}});
    CefRefPtr<BrowserFeatures> self(this);
    auto call = IPC::Singleton.BrowserDecision(browserId, kind, detail,
                                               [self, browser, browserId, id, kind, detail](CefRefPtr<CefDictionaryValue> answer)
                                               {
                                                   if (self->pending_.find(id) == self->pending_.end())
                                                       return;
                                                   if (answer->GetSize() > 0 || !justcef_view::OnViewBrowserRequest(browser, kind, detail))
                                                       self->Answer(browserId, id, answer);
                                               });
    auto it = pending_.find(id);
    if (it != pending_.end())
        it->second.call = call;
    CefPostDelayedTask(TID_UI,
                       base::BindOnce(
                           [](CefRefPtr<BrowserFeatures> state, int browser, int request)
                           {
                               state->Answer(browser, request, CefDictionaryValue::Create());
                           },
                           self, browserId, id),
                       60000);
}

bool BrowserFeatures::Answer(int browserId, int id, CefRefPtr<CefDictionaryValue> answer)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = pending_.find(id);
    if (it == pending_.end() || it->second.browserId != browserId)
        return false;
    auto pending = std::move(it->second);
    pending_.erase(it);
    pending.call.Cancel();
    pending.callback(answer);
    return true;
}

void BrowserFeatures::Cancel(int browserId, bool closing, const std::string& frameId)
{
    std::vector<int> ids;
    for (auto& [id, pending] : pending_)
        if (pending.browserId == browserId && (closing || (!pending.survivesNavigation && (frameId.empty() || pending.frameId == frameId))))
            ids.push_back(id);
    for (int id : ids)
        Answer(browserId, id, CefDictionaryValue::Create());
    for (auto it = prompts_.begin(); it != prompts_.end();)
    {
        if (it->first.first == browserId && (frameId.empty() || std::find(ids.begin(), ids.end(), it->second) != ids.end()))
            it = prompts_.erase(it);
        else
            ++it;
    }
    if (closing)
    {
        std::vector<CefRefPtr<CefDownloadItemCallback>> callbacks;
        for (auto it = downloads_.begin(); it != downloads_.end();)
        {
            if (it->first.first == browserId)
            {
                callbacks.push_back(it->second);
                it = downloads_.erase(it);
            }
            else
                ++it;
        }
        for (auto& callback : callbacks)
            callback->Cancel();
    }
}

bool BrowserFeatures::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& origin, uint32_t permissions,
                                                     CefRefPtr<CefMediaAccessCallback> callback)
{
    if (permissions & (CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE))
        return false; // Screen selection remains with Chromium's default handling.
    auto context = browser->GetHost()->GetRequestContext();
    const std::string topLevel = browser->GetMainFrame()->GetURL();
    bool remembered = true;
    for (const auto& pair : {std::make_pair(static_cast<uint32_t>(CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE), CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC),
                             std::make_pair(static_cast<uint32_t>(CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE), CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA)})
    {
        if (!(permissions & pair.first))
            continue;
        const auto setting = context->GetContentSetting(origin, topLevel, pair.second);
        if (setting == CEF_CONTENT_SETTING_VALUE_BLOCK)
        {
            callback->Cancel();
            return true;
        }
        remembered = remembered && setting == CEF_CONTENT_SETTING_VALUE_ALLOW;
    }
    const auto deviceMask = CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
    if (remembered && permissions != 0 && !(permissions & ~deviceMask))
    {
        callback->Continue(permissions);
        return true;
    }
    auto detail = CefDictionaryValue::Create();
    detail->SetString("origin", origin);
    detail->SetString("frameId", frame ? frame->GetIdentifier() : CefString());
    detail->SetString("topLevelOrigin", topLevel);
    detail->SetString("type", "media");
    detail->SetBool("canRemember", CanRememberOrigin(origin) && CanRememberOrigin(topLevel) && !(permissions & ~deviceMask));
    detail->SetInt("permissions", permissions);
    Ask(browser, "permission", detail,
        [browser, origin = origin.ToString(), topLevel, permissions, callback](CefRefPtr<CefDictionaryValue> answer)
        {
            const bool allow = answer->GetBool("allow");
            if (answer->GetBool("remember"))
                SetMediaSetting(browser, origin, permissions, allow ? CEF_CONTENT_SETTING_VALUE_ALLOW : CEF_CONTENT_SETTING_VALUE_BLOCK, topLevel);
            if (allow)
                callback->Continue(permissions);
            else
                callback->Cancel();
        });
    return true;
}

bool BrowserFeatures::OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t promptId, const CefString& origin, uint32_t permissions,
                                             CefRefPtr<CefPermissionPromptCallback> callback)
{
    auto detail = CefDictionaryValue::Create();
    detail->SetString("origin", origin);
    detail->SetString("type", "prompt");
    detail->SetInt("permissions", permissions);
    const int id = nextId_ + 1;
    prompts_[{browser->GetIdentifier(), promptId}] = id;
    Ask(browser, "permission", detail,
        [callback](CefRefPtr<CefDictionaryValue> answer)
        {
            callback->Continue(answer->GetBool("allow") ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
        });
    return true;
}

void BrowserFeatures::OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t promptId, cef_permission_request_result_t)
{
    auto it = prompts_.find({browser->GetIdentifier(), promptId});
    if (it == prompts_.end())
        return;
    const int id = it->second;
    prompts_.erase(it);
    auto pending = pending_.find(id);
    if (pending != pending_.end())
    {
        pending->second.call.Cancel();
        pending_.erase(pending);
    }
}

bool BrowserFeatures::OnBeforeDownload(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item, const CefString& name, CefRefPtr<CefBeforeDownloadCallback> callback)
{
    auto detail = CefDictionaryValue::Create();
    detail->SetInt("id", item->GetId());
    detail->SetString("url", item->GetURL());
    detail->SetString("suggestedName", name);
    detail->SetString("mimeType", item->GetMimeType());
    const auto downloadId = item->GetId();
    CefRefPtr<BrowserFeatures> self(this);
    Ask(browser, "download", detail,
        [self, browser, downloadId, callback](CefRefPtr<CefDictionaryValue> answer)
        {
            if (answer->GetBool("allow"))
                callback->Continue(answer->GetString("path"), answer->GetBool("showDialog"));
            else
                self->DownloadCommand(browser->GetIdentifier(), downloadId, "cancel");
        });
    return true;
}

void BrowserFeatures::OnDownloadUpdated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback> callback)
{
    const auto key = std::make_pair(browser->GetIdentifier(), item->GetId());
    if (item->IsInProgress() || item->IsInterrupted())
        downloads_[key] = callback;
    else
        downloads_.erase(key);
    auto detail = CefDictionaryValue::Create();
    detail->SetInt("id", item->GetId());
    detail->SetString("url", item->GetURL());
    detail->SetString("path", item->GetFullPath());
    detail->SetDouble("receivedBytes", static_cast<double>(item->GetReceivedBytes()));
    detail->SetDouble("totalBytes", static_cast<double>(item->GetTotalBytes()));
    detail->SetInt("percent", item->GetPercentComplete());
    detail->SetBool("complete", item->IsComplete());
    detail->SetBool("canceled", item->IsCanceled());
    detail->SetBool("inProgress", item->IsInProgress());
    detail->SetBool("interrupted", item->IsInterrupted());
    detail->SetInt("interruptReason", item->GetInterruptReason());
    detail->SetBool("paused", item->IsPaused());
    IPC::Singleton.NotifyDownloadUpdated(browser->GetIdentifier(), detail);
}

bool BrowserFeatures::DownloadCommand(int browserId, uint32_t id, const std::string& action)
{
    auto it = downloads_.find({browserId, id});
    if (it == downloads_.end())
        return false;
    auto callback = it->second;
    if (action == "cancel")
    {
        downloads_.erase(it);
        callback->Cancel();
    }
    else if (action == "pause")
        callback->Pause();
    else if (action == "resume")
        callback->Resume();
    else
        return false;
    return true;
}

void HandleBrowserCommand(PacketReader& reader, ipc::Reply reply)
{
    auto identifier = reader.read<int32_t>();
    auto name = reader.readSizePrefixedString();
    auto json = reader.readSizePrefixedString();
    auto value = json ? CefParseJSON(*json, JSON_PARSER_RFC) : nullptr;
    if (!identifier || !name || !value || value->GetType() != VTYPE_DICTIONARY || reader.remainingSize() != 0)
    {
        reply.Fail(ipc::StatusCode::InvalidRequest, "Expected browser identifier, command, and JSON object.");
        return;
    }
    auto browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        reply.Fail(ipc::StatusCode::NotFound, "Browser is closed.");
        return;
    }
    auto client = static_cast<Client*>(browser->GetHost()->GetClient().get());
    auto options = value->GetDictionary();
    auto result = CefDictionaryValue::Create();
    if (*name == "captureScreenshot")
    {
        const std::string format = options->GetString("format");
        if ((!format.empty() && format != "png" && format != "jpeg" && format != "webp") ||
            (options->HasKey("quality") && (options->GetInt("quality") < 0 || options->GetInt("quality") > 100)))
        {
            reply.Fail(ipc::StatusCode::InvalidRequest, "Invalid screenshot format or quality.");
            return;
        }
        auto response = std::make_shared<ipc::Reply>(std::move(reply));
        auto cancel = CaptureBrowserScreenshot(browser, options, [response](bool ok, std::string image)
        {
            if (!ok) return response->Fail(ipc::StatusCode::Error, image);
            PacketWriter writer;
            if (!writer.writeSizePrefixedString(image))
                return response->Fail(ipc::StatusCode::TooLarge, "Screenshot is too large.");
            response->Ok(writer.data(), writer.size());
        });
        response->OnCanceled([cancel]() { CefPostTask(TID_UI, base::BindOnce([](std::function<void()> cancel) { cancel(); }, cancel)); });
        return;
    }
    if (*name == "windowOpenPolicy")
    {
        client->allowNewWindows = options->GetBool("allow");
        client->interceptNewWindows = options->GetBool("intercept");
    }
    else if (*name == "print")
        browser->GetHost()->Print();
    else if (*name == "download")
    {
        const auto url = options->GetString("url");
        if (url.empty())
        {
            reply.Fail(ipc::StatusCode::InvalidRequest, "Download URL is required.");
            return;
        }
        browser->GetHost()->StartDownload(url);
    }
    else if (*name == "downloadControl")
    {
        if (!client->Features()->DownloadCommand(*identifier, options->GetInt("id"), options->GetString("action")))
        {
            reply.Fail(ipc::StatusCode::NotFound, "Active download or action not found.");
            return;
        }
    }
    else if (*name == "permission")
    {
        const auto origin = options->GetString("origin");
        if (origin.empty())
        {
            reply.Fail(ipc::StatusCode::InvalidRequest, "Permission origin is required.");
            return;
        }
        auto context = browser->GetHost()->GetRequestContext();
        const std::string topLevel = options->HasKey("topLevelOrigin") ? options->GetString("topLevelOrigin").ToString() : origin.ToString();
        if (options->HasKey("value"))
        {
            if (!CanRememberOrigin(origin) || !CanRememberOrigin(topLevel))
            {
                reply.Fail(ipc::StatusCode::Unsupported, "Chromium cannot persist media content settings for this scheme; use an HTTP(S) origin.");
                return;
            }
            const int setting = options->GetInt("value");
            if (setting != CEF_CONTENT_SETTING_VALUE_DEFAULT && setting != CEF_CONTENT_SETTING_VALUE_ALLOW && setting != CEF_CONTENT_SETTING_VALUE_BLOCK &&
                setting != CEF_CONTENT_SETTING_VALUE_ASK)
            {
                reply.Fail(ipc::StatusCode::InvalidRequest, "Unsupported permission value.");
                return;
            }
            const int selected = options->GetInt("permissions");
            if (selected < 1 || selected > 3)
            {
                reply.Fail(ipc::StatusCode::InvalidRequest, "Expected microphone/camera flags.");
                return;
            }
            SetMediaSetting(browser, origin, ((selected & 1) ? CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE : 0) | ((selected & 2) ? CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE : 0),
                            static_cast<cef_content_setting_values_t>(setting), topLevel);
        }
        result->SetInt("microphone", context->GetContentSetting(origin, topLevel, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC));
        result->SetInt("camera", context->GetContentSetting(origin, topLevel, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA));
    }
    else
    {
        reply.Fail(ipc::StatusCode::Unsupported, "Unknown browser command.");
        return;
    }
    auto output = CefValue::Create();
    output->SetDictionary(result);
    PacketWriter writer;
    writer.writeSizePrefixedString(CefWriteJSON(output, JSON_WRITER_DEFAULT));
    reply.Ok(writer.data(), writer.size());
}
