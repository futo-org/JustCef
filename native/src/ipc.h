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
#include <vector>
#include <condition_variable>
#include <optional>
#include <stdint.h>

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
    //WindowSetZoom = 9, //double zoom
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
    PickFile = 38,
    PickDirectory = 39,
    SaveFile = 40,
    WindowExecuteDevToolsMethod = 41,
    WindowSetDevelopmentToolsVisible = 42,
    WindowSetTitle = 43,
    WindowSetIcon = 44,
    WindowAddUrlToProxy = 45,
    WindowRemoveUrlToProxy = 46,
    WindowAddUrlToModify = 47,
    WindowRemoveUrlToModify = 48,
    WindowGetSize = 49,
    WindowSetSize = 50,
    WindowAddDevToolsEventMethod = 51,
    WindowRemoveDevToolsEventMethod = 52
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
    StreamClose = 5
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
    std::map<std::string, std::string> headers = {};
    std::optional<std::vector<uint8_t>> body = std::nullopt;
    std::shared_ptr<DataStream> bodyStream = nullptr;
} IPCProxyResponse;

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
    int minimumWidth = 800;
    int minimumHeight = 600;
    int preferredWidth = 1024;
    int preferredHeight = 768;
    std::string url = "";
    std::optional<std::string> title = std::nullopt;
    std::optional<std::string> iconPath = std::nullopt;
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

    void Start();
    void Stop();

    std::vector<uint8_t> Echo(const uint8_t* data, size_t size);
    void Ping();
    void Print(const char* message, size_t size);
    void Print(const std::string& message);
    void StreamClose(uint32_t identifier) { Call(OpcodeClient::StreamClose, (uint8_t*)&identifier, sizeof(uint32_t)); }
    void WindowModifyRequest(int32_t identifier, CefRefPtr<CefRequest> request, bool modifyRequestBody);
    std::unique_ptr<IPCProxyResponse> WindowProxyRequest(int32_t identifier, CefRefPtr<CefRequest> request);
    
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

    void QueueWork(std::function<void()> work) 
    { 
        if (_stopped)
            return;
            
        _worker.EnqueueWork(work); 
    }

    void CloseStream(uint32_t identifier);
private:
    void QueueStreamWork(std::function<void()> work) 
    { 
        if (_stopped)
            return;
            
        _streamWorker.EnqueueWork(work); 
    }

    void Run();
    std::vector<uint8_t> Call(OpcodeClient opcode, const uint8_t* body = nullptr, size_t size = 0);
    void Notify(OpcodeClientNotification opcode, const uint8_t* body = nullptr, size_t size = 0);
    void Notify(OpcodeClientNotification opcode, const PacketWriter& writer);
    void HandleRequest(OpcodeController opcode, PacketReader& reader, PacketWriter& writer);
    void HandleNotification(OpcodeControllerNotification opcode, PacketReader& reader);

    std::atomic<uint32_t> _requestIdCounter;

    bool _stopped = true;
    bool _startCalled = false;
    std::mutex _writeMutex;
    std::mutex _requestMapMutex;
    std::mutex _dataStreamsMutex;
    std::vector<uint8_t> _sendBuffer;
    std::vector<uint8_t> _readBuffer;
    std::unordered_map<uint32_t, std::shared_ptr<IPCPendingRequest>> _pendingRequests;
    std::map<uint32_t, std::shared_ptr<DataStream>> _dataStreams;
    std::thread _thread;
#if _WIN32
    DWORD _readThreadId = 0;
#endif
    WorkQueue _worker;
    ThreadPool _threadPool;
    WorkQueue _streamWorker;
    BufferPool _readBufferPool;
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
void HandleAddUrlToModify(PacketReader& reader, PacketWriter& writer);
void HandleRemoveUrlToModify(PacketReader& reader, PacketWriter& writer);
void HandleWindowGetSize(PacketReader& reader, PacketWriter& writer);
void HandleWindowSetSize(PacketReader& reader, PacketWriter& writer);
void HandleAddDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);
void HandleRemoveDevToolsEventMethod(PacketReader& reader, PacketWriter& writer);

#endif //IPC_H
