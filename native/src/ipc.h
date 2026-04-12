#ifndef IPC_H
#define IPC_H

#include "pipe.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_response.h"
#include "work_queue.h"
#include "datastream.h"
#include "thread_pool.h"
#include "bufferpool.h"
#include "packet_reader.h"
#include "packet_writer.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include <optional>
#include <queue>
#include <stdint.h>
#include <utility>

class Client;

#define MAXIMUM_IPC_SIZE 10 * 1024 * 1024

enum class PacketType : uint8_t {
    Request = 0,
    Response = 1,
    Notification = 2
};

//Requests from controller
enum class OpcodeController : uint8_t {
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowCreate = 3,
    //WindowCreatePositioned = 4,
    WindowSetDevelopmentToolsEnabled = 5,
    WindowLoadUrl = 6,
    //WindowLoadHtml = 7,
    //WindowExecuteJavascript = 8, //string js
    WindowSetZoom = 9, //double zoom
    //WindowSetResizable = 10, //bool value
    //WindowSetWindowless = 11, //bool value
    //WindowGetWindowSize = 12,
    //WindowSetWindowSize = 13, //Size size
    WindowGetPosition = 14,
    WindowSetPosition = 15, //Position value
    //WindowCenterWindow = 16,
    WindowMaximize = 17,
    WindowMinimize = 18,
    WindowRestore = 19,
    WindowShow = 20,
    WindowHide = 21,
    WindowClose = 22,
    //WindowSetRequestModificationEnabled = 23, //bool enabled
    //WindowModifyRequest = 24, //Request request -> Response
    WindowRequestFocus = 25,
    //WindowRegisterKeyboardListener = 26,
    //WindowSetTitle = 27,
    WindowActivate = 28,
    WindowBringToTop = 29,
    WindowSetAlwaysOnTop = 30,
    WindowSetFullscreen = 31,
    WindowCenterSelf = 32,
    WindowSetProxyRequests = 33,
    WindowSetModifyRequests = 34,
    StreamOpen = 35,
    StreamClose = 36,
    StreamData = 37,
    StreamCancel = 38,
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
    WindowBridgeRpc = 57
};

//Notifications from controller
enum class OpcodeControllerNotification : uint8_t {
    Exit = 0
};

//Requests from client
enum class OpcodeClient : uint8_t {
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowProxyRequest = 3,
    WindowModifyRequest = 4,
    StreamOpen = 5,
    StreamData = 6,
    StreamClose = 7,
    StreamCancel = 8,
    WindowBridgeRpc = 9
};

//Notifications from client
enum class OpcodeClientNotification : uint8_t {
    Ready = 0,
    Exit = 1,
    WindowOpened = 2,
    WindowClosed = 3,
    //WindowResized = 4,
    WindowFocused = 5,
    WindowUnfocused = 6,
    //WindowMinimized = 7,
    //WindowMaximized = 8,
    //WindowRestored = 9,
    //WindowMoved = 10,
    //WindowKeyboardEvent = 11,
    WindowFullscreenChanged = 12,
    WindowLoadStart = 13,
    WindowLoadEnd = 14,
    WindowLoadError = 15,
    WindowDevToolsEvent = 16
};

typedef struct _IPCPendingRequest {
    OpcodeClient opcode;
    uint32_t requestId;
    bool ready;
    std::mutex mutex;
    std::condition_variable conditionVariable;
    std::vector<uint8_t> responseBody;
} IPCPendingRequest;

#ifdef _WIN32
#pragma pack(push, 1)
#define PACKED
#else
#define PACKED __attribute__((packed))
#endif

typedef struct PACKED _IPCPacketHeader {
    uint32_t size = 0;
    uint32_t requestId = 0;
    PacketType packetType = PacketType::Request;
    uint8_t opcode = 0;
} IPCPacketHeader;

#ifdef _WIN32
#pragma pack(pop)
#endif

typedef struct _IPCDevToolsMethodResult 
{
    int32_t messageId = 0;
    bool success = false;
    std::shared_ptr<std::vector<uint8_t>> result;
} IPCDevToolsMethodResult;

typedef struct _IPCProxyResponse 
{
    int32_t status_code = 0;
    std::string status_text = "";
    std::optional<std::string> media_type = std::nullopt;
    std::multimap<std::string, std::string> headers = {};
    std::optional<std::vector<uint8_t>> body = std::nullopt;
    std::shared_ptr<DataStream> bodyStream = nullptr;
} IPCProxyResponse;

typedef struct _IPCBridgeRpcResult
{
    bool success = false;
    std::optional<std::string> result_json = std::nullopt;
    std::optional<std::string> error = std::nullopt;
} IPCBridgeRpcResult;

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
} IPCWindowCreate;

class IPC {
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

    void Start();
    void Stop();

    std::vector<uint8_t> Echo(const uint8_t* data, size_t size);
    void Ping();
    void Print(const char* message, size_t size);
    void Print(const std::string& message);
    void StreamCancel(uint32_t identifier) { Call(OpcodeClient::StreamCancel, (uint8_t*)&identifier, sizeof(uint32_t)); }
    void WindowModifyRequest(int32_t identifier, CefRefPtr<CefRequest> request, bool modifyRequestBody);
    std::unique_ptr<IPCProxyResponse> WindowProxyRequest(int32_t identifier, CefRefPtr<CefRequest> request);
    IPCBridgeRpcResult WindowBridgeRpc(int32_t identifier, const std::string& method, const std::string& payload_json);
    
    void NotifyExit() { Notify(OpcodeClientNotification::Exit); }
    void NotifyReady() { Notify(OpcodeClientNotification::Ready); }

    void NotifyWindowOpened(CefRefPtr<CefBrowser> browser);
    void NotifyWindowClosed(CefRefPtr<CefBrowser> browser);
    void NotifyWindowFocused(CefRefPtr<CefBrowser> browser);
    void NotifyWindowUnfocused(CefRefPtr<CefBrowser> browser);
    //void NotifyWindowMinimized(CefRefPtr<CefBrowser> browser);
    //void NotifyWindowMaximized(CefRefPtr<CefBrowser> browser);
    //void NotifyWindowRestored(CefRefPtr<CefBrowser> browser);
    //void NotifyWindowKeyboardEvent(CefRefPtr<CefBrowser> browser, const cef_key_event_t& event);
    //void NotifyWindowResized(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height);
    //void NotifyWindowMoved(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height);
    void NotifyWindowFullscreenChanged(CefRefPtr<CefBrowser> browser, bool fullscreen);
    void NotifyWindowLoadStart(CefRefPtr<CefBrowser> browser, const CefString& url);
    void NotifyWindowLoadEnd(CefRefPtr<CefBrowser> browser, const CefString& url);
    void NotifyWindowLoadError(CefRefPtr<CefBrowser> browser, cef_errorcode_t errorCode, const CefString& errorText, const CefString& url);
    void NotifyWindowDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const uint8_t* result, size_t result_size);
    void QueueResponse(OpcodeController opcode, uint32_t requestId, const PacketWriter& writer);

    void QueueWork(std::function<void()> work) 
    {
        if (!IsAvailable())
            return;
            
        _worker.EnqueueWork(std::move(work));
    }

    bool QueueBackgroundWork(std::function<void()> work)
    {
        if (!IsAvailable())
            return false;

        return _threadPool.Enqueue(std::move(work));
    }

    void CloseStream(uint32_t identifier);
    void ReleaseIncomingStream(uint32_t identifier);
private:
    struct IncomingStreamDispatcher {
        std::mutex mutex;
        std::queue<std::function<void()>> queue;
        bool running = false;
    };

    void Run();
    std::vector<uint8_t> Call(OpcodeClient opcode, const uint8_t* body = nullptr, size_t size = 0, std::function<void()> afterWrite = nullptr);
    void Notify(OpcodeClientNotification opcode, const uint8_t* body = nullptr, size_t size = 0);
    void Notify(OpcodeClientNotification opcode, const PacketWriter& writer);
    bool HandleRequest(uint32_t requestId, OpcodeController opcode, PacketReader& reader, PacketWriter& writer);
    void HandleNotification(OpcodeControllerNotification opcode, PacketReader& reader);
    void WriteResponse(uint32_t requestId, uint8_t opcode, const uint8_t* body, size_t size);
    void WriteQueuedResponsePacket(const uint8_t* packet, size_t packetLength);
    bool QueueIncomingStreamWork(uint32_t identifier, std::function<void()> work);
    void ProcessIncomingStreamDispatcher(uint32_t identifier, std::shared_ptr<IncomingStreamDispatcher> dispatcher);
    std::shared_ptr<DataStream> FindIncomingStream(uint32_t identifier);
    std::shared_ptr<DataStream> GetOrCreateIncomingStream(uint32_t identifier);
    void QueueDeferredStreamWriters(std::vector<std::function<void()>> streamWriters);
    bool OpenClientStream(uint32_t identifier);
    bool StreamClientData(uint32_t identifier, const uint8_t* data, size_t size);
    void CloseClientStream(uint32_t identifier);
    std::shared_ptr<std::atomic<bool>> RegisterOutgoingStream(uint32_t identifier);
    std::shared_ptr<std::atomic<bool>> GetOutgoingStreamCancelFlag(uint32_t identifier);
    void RemoveOutgoingStream(uint32_t identifier);
    bool SerializePostData(PacketWriter& writer, CefRefPtr<CefPostData> postData, std::vector<std::function<void()>>& streamWriters);

    std::atomic<uint32_t> _requestIdCounter;
    std::atomic<uint32_t> _streamIdentifierCounter;

    std::atomic<bool> _stopped = true;
    std::atomic<bool> _startCalled = false;
    std::mutex _writeMutex;
    std::mutex _requestMapMutex;
    std::mutex _dataStreamsMutex;
    std::mutex _incomingStreamDispatchersMutex;
    std::mutex _outgoingStreamsMutex;
    std::vector<uint8_t> _sendBuffer;
    std::vector<uint8_t> _readBuffer;
    std::unordered_map<uint32_t, std::shared_ptr<IPCPendingRequest>> _pendingRequests;
    std::map<uint32_t, std::shared_ptr<DataStream>> _dataStreams;
    std::unordered_set<uint32_t> _canceledIncomingStreams;
    std::unordered_map<uint32_t, std::shared_ptr<IncomingStreamDispatcher>> _incomingStreamDispatchers;
    std::unordered_map<uint32_t, std::shared_ptr<std::atomic<bool>>> _outgoingStreams;
    std::thread _thread;
#if _WIN32
    DWORD _readThreadId = 0;
#endif
    WorkQueue _worker;
    ThreadPool _threadPool;
    BufferPool _ipcBufferPool;
    Pipe _pipe;
    //Exit fullscreen
};

void CloseEverything();
void HandleWindowCreate(PacketReader& reader, PacketWriter& writer);
void HandleWindowMaximize(PacketReader& reader, PacketWriter& writer);
void HandleWindowMinimize(PacketReader& reader, PacketWriter& writer);
void HandleWindowRestore(PacketReader& reader, PacketWriter& writer);
void HandleWindowShow(PacketReader& reader, PacketWriter& writer);
void HandleWindowHide(PacketReader& reader, PacketWriter& writer);
void HandleWindowActivate(PacketReader& reader, PacketWriter& writer);
void HandleWindowBringToTop(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetAlwaysOnTop(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetFullscreen(PacketReader& reader, PacketWriter& writer);
void HandleWindowCenterSelf(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetProxyRequests(PacketReader& reader, PacketWriter& writer);
void HandleWindowGetPosition(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetPosition(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetDevelopmentToolsEnabled(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetDevelopmentToolsVisible(PacketReader& reader, PacketWriter& writer);
void HandleWindowClose(PacketReader& reader, PacketWriter& writer);
void HandleWindowLoadUrl(PacketReader& reader, PacketWriter& writer);
void HandleWindowRequestFocus(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetModifyRequests(PacketReader& reader, PacketWriter& writer);
void HandleWindowOpenFilePicker(PacketReader& reader, PacketWriter& writer);
void HandleWindowOpenDirectoryPicker(PacketReader& reader, PacketWriter& writer);
void HandleWindowSaveFilePicker(PacketReader& reader, PacketWriter& writer);
void HandleWindowExecuteDevToolsMethod(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetTitle(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetIcon(PacketReader& reader, PacketWriter& writer);
void HandleAddUrlToProxy(PacketReader& reader, PacketWriter& writer);
void HandleRemoveUrlToProxy(PacketReader& reader, PacketWriter& writer);
void HandleAddDomainToProxy(PacketReader& reader, PacketWriter& writer);
void HandleRemoveDomainToProxy(PacketReader& reader, PacketWriter& writer);
void HandleAddUrlToModify(PacketReader& reader, PacketWriter& writer);
void HandleRemoveUrlToModify(PacketReader& reader, PacketWriter& writer);
void HandleWindowGetSize(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetSize(PacketReader& reader, PacketWriter& writer);
void HandleAddDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);
void HandleRemoveDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetZoom(PacketReader& reader, PacketWriter& writer);
void HandleWindowGetZoom(PacketReader& reader, PacketWriter& writer);
bool HandleWindowBridgeRpc(uint32_t requestId, PacketReader& reader, PacketWriter& writer);
CefRefPtr<Client> CreateBrowserWindow(const IPCWindowCreate& windowCreate);

#endif //IPC_H
