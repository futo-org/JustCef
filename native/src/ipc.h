#ifndef IPC_H
#define IPC_H

#include "endpoint.h"
#include "event_loop.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_response.h"
#include "include/views/cef_browser_view.h"
#include "packet_reader.h"
#include "packet_writer.h"
#include "stream_receiver.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stdint.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Client;

// Requests from controller
enum class OpcodeController : uint8_t
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowCreate = 3,
    // WindowCreatePositioned = 4,
    WindowSetDevelopmentToolsEnabled = 5,
    WindowLoadUrl = 6,
    // WindowLoadHtml = 7,
    // WindowExecuteJavascript = 8, //string js
    WindowSetZoom = 9, // double zoom
    // WindowSetResizable = 10, //bool value
    // WindowSetWindowless = 11, //bool value
    // WindowGetWindowSize = 12,
    // WindowSetWindowSize = 13, //Size size
    WindowGetPosition = 14,
    WindowSetPosition = 15, // Position value
    // WindowCenterWindow = 16,
    WindowMaximize = 17,
    WindowMinimize = 18,
    WindowRestore = 19,
    WindowShow = 20,
    WindowHide = 21,
    WindowClose = 22,
    // WindowSetRequestModificationEnabled = 23, //bool enabled
    // WindowModifyRequest = 24, //Request request -> Response
    WindowRequestFocus = 25,
    // WindowRegisterKeyboardListener = 26,
    // WindowSetTitle = 27,
    WindowActivate = 28,
    WindowBringToTop = 29,
    WindowSetAlwaysOnTop = 30,
    WindowSetFullscreen = 31,
    WindowCenterSelf = 32,
    WindowSetProxyRequests = 33,
    WindowSetModifyRequests = 34,
    PickFile = 39,
    PickDirectory = 40,
    SaveFile = 41,
    WindowExecuteDevToolsMethod = 42,
    WindowSetDevelopmentToolsVisible = 43,
    WindowSetTitle = 44,
    WindowSetIcon = 45,
    WindowAddUrlToProxy = 46,
    WindowRemoveUrlToProxy = 47,
    WindowAddUrlToModify = 48,
    WindowRemoveUrlToModify = 49,
    WindowGetSize = 50,
    WindowSetSize = 51,
    WindowAddDevToolsEventMethod = 52,
    WindowRemoveDevToolsEventMethod = 53,
    WindowAddDomainToProxy = 54,
    WindowRemoveDomainToProxy = 55,
    WindowGetZoom = 56,
    WindowBridgeRpc = 57,
    GetWidevineStatus = 59,
    Debug = 250
};

// Notifications from controller
enum class OpcodeControllerNotification : uint8_t
{
    Exit = 0,
    StreamData = 1,
    StreamEnd = 2,
    StreamError = 3
};

// Requests from client
enum class OpcodeClient : uint8_t
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowProxyRequest = 3,
    WindowModifyRequest = 4,
    WindowBridgeRpc = 9,
    WindowViewCreated = 11
};

// Notifications from client
enum class OpcodeClientNotification : uint8_t
{
    Ready = 0,
    Exit = 1,
    WindowOpened = 2,
    WindowClosed = 3,
    // WindowResized = 4,
    WindowFocused = 5,
    WindowUnfocused = 6,
    // WindowMinimized = 7,
    // WindowMaximized = 8,
    // WindowRestored = 9,
    // WindowMoved = 10,
    // WindowKeyboardEvent = 11,
    WindowFullscreenChanged = 12,
    WindowFrameLoadStart = 13,
    WindowFrameLoadEnd = 14,
    WindowFrameLoadError = 15,
    WindowDevToolsEvent = 16,
    WindowLoadingStateChanged = 17,
    StreamCredit = 18,
    StreamCancel = 19,
    Debug = 250
};

typedef struct _IPCProxyResponse
{
    int32_t status_code = 0;
    std::string status_text = "";
    std::optional<std::string> media_type = std::nullopt;
    std::multimap<std::string, std::string> headers = {};
    std::optional<std::vector<uint8_t>> body = std::nullopt;
    std::shared_ptr<ipc::IncomingStream> bodyStream = nullptr;
    int64_t bodyLength = -1;
} IPCProxyResponse;

typedef struct _IPCBridgeRpcResult
{
    bool success = false;
    std::optional<std::string> result_json = std::nullopt;
    std::optional<std::string> error = std::nullopt;
} IPCBridgeRpcResult;

enum class ModifyTimeoutPolicy : uint8_t
{
    Continue = 0,
    Cancel = 1
};

typedef struct _IPCWindowCreate
{
    bool resizable = true;
    bool frameless = false;
    bool fullscreen = false;
    bool centered = true;
    bool shown = true;
    bool contextMenuEnable = true;
    bool developerToolsEnabled = false;
    bool modifyRequests = false;
    bool modifyRequestBody = false;
    bool proxyRequests = false;
    bool logConsole = false;
    bool bridgeEnabled = false;
    int minimumWidth = 800;
    int minimumHeight = 600;
    int preferredWidth = 1024;
    int preferredHeight = 768;
    std::string url = "";
    std::optional<std::string> title = std::nullopt;
    std::optional<std::string> iconPath = std::nullopt;
    std::optional<std::string> appId = std::nullopt;
    bool viewsEnabled = false;
    uint32_t modifyTimeoutMs = 0;
    ModifyTimeoutPolicy modifyTimeoutPolicy = ModifyTimeoutPolicy::Continue;
    uint32_t proxyOpenTimeoutMs = 0;
} IPCWindowCreate;

class IPC
{
public:
    static IPC Singleton;

    IPC();
    ~IPC();

#ifdef _WIN32
    void SetHandles(HANDLE readHandle, HANDLE writeHandle);
#else
    void SetHandles(int readFd, int writeFd);
#endif

    bool HasValidHandles();
    bool IsAvailable();
    bool IsDebugEnabled() const { return _debugEnabled; }

    void Start();
    void Stop();

    ipc::CallHandle Echo(std::vector<uint8_t> data, std::chrono::milliseconds timeout, std::function<void(ipc::Response)> callback);
    ipc::CallHandle WindowModifyRequest(int32_t identifier, CefRefPtr<CefRequest> request, bool modifyRequestBody, std::chrono::milliseconds timeout,
                                        std::function<void(ipc::StatusCode)> callback);
    ipc::CallHandle WindowProxyRequest(int32_t identifier, CefRefPtr<CefRequest> request, std::chrono::milliseconds timeout,
                                       std::function<void(ipc::StatusCode, std::unique_ptr<IPCProxyResponse>)> callback);
    ipc::CallHandle WindowBridgeRpc(int32_t identifier, const std::string& method, const std::string& payload_json, std::function<void(IPCBridgeRpcResult)> callback);
    void WindowViewCreated(int32_t parentIdentifier, int32_t viewIdentifier, const std::string& src, std::function<void(bool allow)> callback);

    void NotifyExit() { Notify(OpcodeClientNotification::Exit); }
    void NotifyReady()
    {
        const uint32_t version = ipc::kProtocolVersion;
        Notify(OpcodeClientNotification::Ready, reinterpret_cast<const uint8_t*>(&version), sizeof(version));
    }

    void NotifyWindowOpened(CefRefPtr<CefBrowser> browser);
    void NotifyWindowClosed(CefRefPtr<CefBrowser> browser);
    void NotifyWindowFocused(CefRefPtr<CefBrowser> browser);
    void NotifyWindowUnfocused(CefRefPtr<CefBrowser> browser);
    // void NotifyWindowMinimized(CefRefPtr<CefBrowser> browser);
    // void NotifyWindowMaximized(CefRefPtr<CefBrowser> browser);
    // void NotifyWindowRestored(CefRefPtr<CefBrowser> browser);
    // void NotifyWindowKeyboardEvent(CefRefPtr<CefBrowser> browser, const cef_key_event_t& event);
    // void NotifyWindowResized(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height);
    // void NotifyWindowMoved(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height);
    void NotifyWindowFullscreenChanged(CefRefPtr<CefBrowser> browser, bool fullscreen);
    void NotifyWindowFrameLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);
    void NotifyWindowFrameLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode);
    void NotifyWindowFrameLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, cef_errorcode_t errorCode, const CefString& errorText, const CefString& url);
    void NotifyWindowLoadingStateChanged(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward);
    void NotifyWindowDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const uint8_t* result, size_t result_size);
    void NotifyDebug(uint32_t sequence);

    void BeginAnnounceHold();
    void EndAnnounceHold();

private:
    void OnTransportClosed();
    void OnRequest(ipc::IncomingRequest request, ipc::Reply reply);
    void RunRequest(ipc::IncomingRequest request, ipc::Reply reply);
    ipc::CallHandle CallAsync(OpcodeClient opcode, std::vector<uint8_t> body, std::chrono::milliseconds timeout, std::function<void(ipc::Response)> callback);
    void Notify(OpcodeClientNotification opcode, const uint8_t* body = nullptr, size_t size = 0);
    void Notify(OpcodeClientNotification opcode, const PacketWriter& writer);
    bool HandleRequest(ipc::Reply& reply, OpcodeController opcode, PacketReader& reader, PacketWriter& writer, ipc::StatusCode& status);
    void HandleNotification(OpcodeControllerNotification opcode, PacketReader& reader);
    bool SerializePostData(PacketWriter& writer, CefRefPtr<CefPostData> postData);
    std::unique_ptr<IPCProxyResponse> ParseProxyResponse(const std::vector<uint8_t>& response);
    void HandleWindowBridgeRpcRequest(ipc::Reply reply, PacketReader& reader);
    void HandleWindowExecuteDevToolsMethodRequest(ipc::Reply reply, PacketReader& reader);
    void HandleDebug(ipc::Reply reply, PacketReader& reader);

    std::atomic<bool> _stopped = true;
    std::atomic<bool> _startCalled = false;
    std::atomic<bool> _stopCalled = false;
    bool _debugEnabled = false;
    std::mutex _holdMutex;
    int _holdDepth = 0;
    std::vector<std::pair<OpcodeClientNotification, std::vector<uint8_t>>> _held;
    ipc::Transport _transport;
    ipc::EventLoop _loop;
    std::shared_ptr<ipc::Endpoint> _endpoint;
    std::shared_ptr<ipc::StreamReceiver> _streams;
    // Exit fullscreen
};

void CloseEverything();
void HandleWindowCreate(PacketReader& reader, ipc::Reply reply);
ipc::StatusCode HandleWindowMaximize(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowMinimize(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowRestore(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowShow(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowHide(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowActivate(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowBringToTop(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetAlwaysOnTop(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetFullscreen(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowCenterSelf(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetProxyRequests(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowGetPosition(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetPosition(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetDevelopmentToolsEnabled(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetDevelopmentToolsVisible(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowClose(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowLoadUrl(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowRequestFocus(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetModifyRequests(PacketReader& reader, PacketWriter& writer);
void HandleWindowOpenFilePicker(PacketReader& reader, ipc::Reply reply);
void HandleWindowOpenDirectoryPicker(PacketReader& reader, ipc::Reply reply);
void HandleWindowSaveFilePicker(PacketReader& reader, ipc::Reply reply);
ipc::StatusCode HandleWindowSetTitle(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetIcon(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleAddUrlToProxy(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleRemoveUrlToProxy(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleAddDomainToProxy(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleRemoveDomainToProxy(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleAddUrlToModify(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleRemoveUrlToModify(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowGetSize(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetSize(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleAddDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleRemoveDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowSetZoom(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleWindowGetZoom(PacketReader& reader, PacketWriter& writer);
ipc::StatusCode HandleGetWidevineStatus(PacketReader& reader, PacketWriter& writer);
bool HandleWindowBridgeRpc(uint32_t requestId, PacketReader& reader, PacketWriter& writer);
CefRefPtr<Client> CreateBrowserWindow(const IPCWindowCreate& windowCreate);
void CreateTopLevelPopupWindow(CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools, const IPCWindowCreate& settings);

#endif // IPC_H
