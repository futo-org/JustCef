#include "ipc.h"
#include "bridge.h"
#include "client.h"
#include "client_manager.h"
#include "client_util.h"

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_stream.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/views/cef_fill_layout.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "devtoolsclient.h"

#include <future>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <optional>
#include <include/cef_app.h>

template<class T>
std::string optionalToString(std::optional<T>& opt) {
    if (!opt) {
        return "std::nullopt";
    }

    return std::string(*opt);
}

namespace {

IPCBridgeRpcResult MakeBridgeRpcResult(bool success, const std::string& result_json, const std::string& error) {
    IPCBridgeRpcResult result;
    result.success = success;
    result.result_json = result_json;
    result.error = error;
    return result;
}

void WriteBridgeRpcResult(PacketWriter& writer, bool success, const std::string& result_json, const std::string& error) {
    writer.write<bool>(success);
    writer.writeSizePrefixedString(result_json);
    writer.writeSizePrefixedString(error);
}

}  // namespace

IPC IPC::Singleton;

IPC::IPC() : _ipcBufferPool(sizeof(IPCPacketHeader) + MAXIMUM_IPC_SIZE, 4)
{
    _requestIdCounter = 0;
    _readBuffer.resize(4096);
    _sendBuffer.resize(4096);
}

IPC::~IPC()
{
    Stop();
}

void IPC::Start()
{
    LOG(INFO) << "IPC start called.";

    _startCalled = true;
    if (!_stopped)
        return;

    LOG(INFO) << "Starting IPC.";

    _stopped = false;

    _worker.Start();
    _streamWorker.Start();
    _threadPool.AddWorkers(4);
    _thread = std::thread([this] () {
#if _WIN32
        _readThreadId = GetCurrentThreadId();
#endif
        LOG(INFO) << "Started IPC thread.";

        Run();
#if _WIN32
        _readThreadId = 0;
#endif
    });
    _thread.detach();

    LOG(INFO) << "Started IPC.";
}

void IPC::Stop()
{
    if (!_startCalled)
        return;

    LOG(INFO) << "IPC stop called.";
    if (_stopped)
        return;

    LOG(INFO) << "Stopping IPC.";

    _stopped = true;

#ifdef _WIN32
    if (_readThreadId != 0) {
        HANDLE hThread = OpenThread(THREAD_TERMINATE | THREAD_SUSPEND_RESUME, FALSE, _readThreadId);
        if (hThread) {
            CancelSynchronousIo(hThread);
            CloseHandle(hThread);
        }
    }
#endif
    _pipe.Close();

    LOG(INFO) << "Stopped pipe.";

    _worker.Stop();
    LOG(INFO) << "Stopped worker.";

    _streamWorker.Stop();
    LOG(INFO) << "Stopped stream worker.";

    _threadPool.Stop();
    LOG(INFO) << "Stopped thread pool.";

    LOG(INFO) << "Cancelling pending requests...";

    std::unordered_map<uint32_t, std::shared_ptr<IPCPendingRequest>> pendingRequests;

    {
        std::lock_guard<std::mutex> lk(_requestMapMutex);
        pendingRequests = _pendingRequests;
    }

    {
        for (auto& pendingRequest : pendingRequests)
        {
            {
                std::unique_lock lk(pendingRequest.second->mutex);
                pendingRequest.second->ready = true;
            }

            pendingRequest.second->conditionVariable.notify_one();
        }
    }

    LOG(INFO) << "Cancelled pending requests.";

    LOG(INFO) << "Closing data streams...";

    {
        std::lock_guard<std::mutex> lk(_dataStreamsMutex);
        for (auto& dataStream : _dataStreams)
            dataStream.second->Close();
        _dataStreams.clear();
    }

    LOG(INFO) << "Closed data streams.";

    /*LOG(INFO) << "Joining IPC threads...";

    if (_thread.joinable()) {
        _thread.join();
    }

    LOG(INFO) << "Joined IPC threads.";*/

    LOG(INFO) << "Stopped IPC.";
}

bool IPC::HasValidHandles()
{
    return _pipe.HasValidHandles();
}

bool IPC::IsAvailable()
{
    return HasValidHandles() && !_stopped && _startCalled;
}

void IPC::Run() 
{
    LOG(INFO) << "IPC running.";

    IPCPacketHeader header;

    while (IsAvailable())
    {
        size_t headerBytesRead = _pipe.Read(&header, sizeof(IPCPacketHeader), true);
        if (headerBytesRead == 0) 
        {
          LOG(INFO) << "Pipe closed. Parent process likely wants child to exit.";
          CloseEverything();
          return;
        }

        if (headerBytesRead != sizeof(IPCPacketHeader))
        {
            LOG(INFO) << "Invalid packet header (" << headerBytesRead << " bytes read). Shutting down.";
            CloseEverything();
            return;
        }

        size_t bodySize = header.size + sizeof(uint32_t) - sizeof(IPCPacketHeader);
        if (bodySize > MAXIMUM_IPC_SIZE) 
        {
            LOG(INFO) << "Invalid packet size (" << bodySize << " bytes). Shutting down.";
            CloseEverything();
            return;
        }

        if (_readBuffer.size() < bodySize)
            _readBuffer.resize(bodySize);
        
        size_t bodyBytesRead = _pipe.Read(_readBuffer.data(), bodySize, true);
        if (bodyBytesRead != bodySize)
        {
            LOG(INFO) << "Invalid body (bodyBytesRead = " << bodyBytesRead << ", bodySize = " << bodySize << "). Shutting down.";
            CloseEverything();
            return;
        }

        LOG(INFO) << "Received packet (packetType = " << (int)header.packetType << ", opcode = " << (int)header.opcode << ")";

        if (header.packetType == PacketType::Response)
        {
            std::shared_ptr<IPCPendingRequest> pPendingRequest;

            {
                std::lock_guard<std::mutex> lk(_requestMapMutex);
                pPendingRequest = _pendingRequests[header.requestId];
            }

            {
                std::unique_lock lk(pPendingRequest->mutex);
                pPendingRequest->ready = true;
                if (bodySize > 0)
                {
                    pPendingRequest->responseBody.resize(bodySize);
                    memcpy(pPendingRequest->responseBody.data(), _readBuffer.data(), bodySize);
                }
            }

            pPendingRequest->conditionVariable.notify_one();
        }
        else if (header.packetType == PacketType::Request)
        {
            std::shared_ptr<std::vector<uint8_t>> readBuffer = _ipcBufferPool.GetBuffer();
            if (readBuffer->size() < bodySize)
            {
                LOG(WARNING) << "Skipped packet that is too large for IPC buffer pool.";
                continue;
            }

            memcpy(readBuffer->data(), _readBuffer.data(), bodySize);

            auto packetHandler = [this, header, bodySize, readBuffer] ()
            {
                PacketReader reader(readBuffer->data(), bodySize);
                PacketWriter writer;
                bool should_write_response = HandleRequest(header.requestId, (OpcodeController)header.opcode, reader, writer);
                _ipcBufferPool.ReturnBuffer(readBuffer);
                if (!should_write_response) {
                    return;
                }

                WriteResponse(header.requestId, header.opcode, writer.data(), writer.size());
            };

            OpcodeController opcode = (OpcodeController)header.opcode;
            if (opcode == OpcodeController::StreamOpen || opcode == OpcodeController::StreamData || opcode == OpcodeController::StreamClose) {
                //Stream packets must always be handled in-order
                _streamWorker.EnqueueWork(packetHandler);
            } else {
                _threadPool.Enqueue(packetHandler);
            }
        }
        else if (header.packetType == PacketType::Notification)
        {
            std::shared_ptr<std::vector<uint8_t>> readBuffer = _ipcBufferPool.GetBuffer();
            if (readBuffer->size() < bodySize)
            {
                LOG(WARNING) << "Skipped packet that is too large for IPC buffer pool.";
                continue;
            }

            memcpy(readBuffer->data(), _readBuffer.data(), bodySize);

            _threadPool.Enqueue([this, header, bodySize, readBuffer] ()
            {
                PacketReader reader(readBuffer->data(), bodySize);
                HandleNotification((OpcodeControllerNotification)header.opcode, reader);
                _ipcBufferPool.ReturnBuffer(readBuffer);
            });
        }
        else
        {
            LOG(INFO) << "Unknown packet type.";
            CloseEverything();
            return;
        }
    }

    LOG(INFO) << "IPC stopped.";
}

std::vector<uint8_t> IPC::Call(OpcodeClient opcode, const uint8_t* body, size_t size)
{
    if (!IsAvailable())
        return std::vector<uint8_t>();

    if (CefCurrentlyOn(TID_UI)) {
        LOG(ERROR) << "!!!!!!WARNING!!!!!! Do not make remote calls on UI thread !!!!!!WARNING!!!!!!";
    }

    uint32_t requestId = ++_requestIdCounter;
    std::shared_ptr<IPCPendingRequest> pPendingRequest = std::make_shared<IPCPendingRequest>();
    pPendingRequest->requestId = requestId;
    pPendingRequest->ready = false;
    pPendingRequest->opcode = opcode;

    {
        std::lock_guard<std::mutex> lk(_requestMapMutex);
        _pendingRequests[requestId] = pPendingRequest;
    }

    {
        std::lock_guard<std::mutex> lk(_writeMutex);

        size_t packetLength = sizeof(IPCPacketHeader) + size;
        if (_sendBuffer.size() < packetLength)
            _sendBuffer.resize(packetLength);

        IPCPacketHeader* pHeader = (IPCPacketHeader*)_sendBuffer.data();
        pHeader->size = (uint32_t)(packetLength - sizeof(uint32_t));
        pHeader->opcode = (uint8_t)opcode;
        pHeader->packetType = PacketType::Request;
        pHeader->requestId = requestId;


        LOG(INFO) << "Sent request (packetType = " << (int)pHeader->packetType << ", opcode = " << (int)pHeader->opcode << "), waiting for response";

        if (body && size > 0)
            memcpy(_sendBuffer.data() + sizeof(IPCPacketHeader), body, size);

        _pipe.Write(_sendBuffer.data(), packetLength, true);
    }

    {
        std::unique_lock lk(pPendingRequest->mutex);
        pPendingRequest->conditionVariable.wait(lk, [pPendingRequest]{ return pPendingRequest->ready; });
    }

    LOG(INFO) << "Got response";

    {
        std::lock_guard<std::mutex> lk(_requestMapMutex);
        _pendingRequests.erase(requestId);
    }

    return pPendingRequest->responseBody;
}

void IPC::Notify(OpcodeClientNotification opcode, const PacketWriter& writer)
{
    Notify(opcode, writer.data(), writer.size());
}

void IPC::Notify(OpcodeClientNotification opcode, const uint8_t* body, size_t size)
{
    if (!IsAvailable())
        return;

    if (CefCurrentlyOn(TID_UI)) {
        LOG(ERROR) << "!!!!!!WARNING!!!!!! Do not make remote calls on UI thread !!!!!!WARNING!!!!!!";
    }

    std::lock_guard<std::mutex> lk(_writeMutex);

    size_t packetLength = sizeof(IPCPacketHeader) + size;
    if (_sendBuffer.size() < packetLength)
        _sendBuffer.resize(packetLength);

    IPCPacketHeader* pHeader = (IPCPacketHeader*)_sendBuffer.data();
    pHeader->size = (uint32_t)(packetLength - sizeof(uint32_t));
    pHeader->opcode = (uint8_t)opcode;
    pHeader->packetType = PacketType::Notification;
    pHeader->requestId = 0;

    LOG(INFO) << "Sent notification (packetType = " << (int)pHeader->packetType << ", opcode = " << (int)pHeader->opcode << ")"; 

    if (body && size > 0)
        memcpy(_sendBuffer.data() + sizeof(IPCPacketHeader), body, size);

    _pipe.Write(_sendBuffer.data(), packetLength, true);
}

void IPC::QueueResponse(OpcodeController opcode, uint32_t requestId, const PacketWriter& writer)
{
    if (!IsAvailable())
        return;

    size_t packetLength = sizeof(IPCPacketHeader) + writer.size();
    std::shared_ptr<std::vector<uint8_t>> packet = _ipcBufferPool.GetBuffer();
    if (packet->size() < packetLength) {
        LOG(ERROR) << "Queued response packet exceeds buffer pool capacity.";
        _ipcBufferPool.ReturnBuffer(packet);
        return;
    }

    IPCPacketHeader* pHeader = reinterpret_cast<IPCPacketHeader*>(packet->data());
    pHeader->size = static_cast<uint32_t>(packetLength - sizeof(uint32_t));
    pHeader->opcode = static_cast<uint8_t>(opcode);
    pHeader->packetType = PacketType::Response;
    pHeader->requestId = requestId;

    if (writer.size() > 0) {
        memcpy(packet->data() + sizeof(IPCPacketHeader), writer.data(), writer.size());
    }

    if (!QueueBackgroundWork([this, packet, packetLength] () {
        WriteQueuedResponsePacket(packet->data(), packetLength);
        _ipcBufferPool.ReturnBuffer(packet);
    })) {
        _ipcBufferPool.ReturnBuffer(packet);
    }
}

void IPC::WriteResponse(uint32_t requestId, uint8_t opcode, const uint8_t* body, size_t size)
{
    if (!IsAvailable())
        return;

    std::lock_guard<std::mutex> lk(_writeMutex);

    size_t packetLength = sizeof(IPCPacketHeader) + size;
    if (_sendBuffer.size() < packetLength)
        _sendBuffer.resize(packetLength);

    IPCPacketHeader* pHeader = (IPCPacketHeader*)_sendBuffer.data();
    pHeader->size = (uint32_t)(packetLength - sizeof(uint32_t));
    pHeader->opcode = opcode;
    pHeader->packetType = PacketType::Response;
    pHeader->requestId = requestId;

    LOG(INFO) << "Sent response (packetType = " << (int)pHeader->packetType
              << ", opcode = " << (int)pHeader->opcode << ")";

    if (body && size > 0)
        memcpy(_sendBuffer.data() + sizeof(IPCPacketHeader), body, size);

    if (_pipe.Write(_sendBuffer.data(), packetLength, true) != packetLength)
    {
        LOG(INFO) << "Failed to write entire response packet.";
        CloseEverything();
    }
}

void IPC::WriteQueuedResponsePacket(const uint8_t* packet, size_t packetLength)
{
    if (!IsAvailable())
        return;

    std::lock_guard<std::mutex> lk(_writeMutex);

    const IPCPacketHeader* pHeader = reinterpret_cast<const IPCPacketHeader*>(packet);
    LOG(INFO) << "Sent queued response (packetType = " << static_cast<int>(pHeader->packetType)
              << ", opcode = " << static_cast<int>(pHeader->opcode) << ")";

    if (_pipe.Write(packet, packetLength, true) != packetLength)
    {
        LOG(INFO) << "Failed to write entire queued response packet.";
        CloseEverything();
    }
}

bool IPC::HandleRequest(uint32_t requestId, OpcodeController opcode, PacketReader& reader, PacketWriter& writer)
{
    switch (opcode)
    {
        case OpcodeController::Ping:
            return true;
        case OpcodeController::Print:
        {
            std::optional<std::string> str = reader.readString((uint32_t)reader.remainingSize());
            if (str)
                LOG(INFO) << *str;
            return true;
        }
        case OpcodeController::Echo:
            reader.copyTo([&writer] (const uint8_t* data, size_t size) {
                return writer.writeBytes(data, size);
            }, reader.remainingSize());
            return true;
        case OpcodeController::WindowCreate:
            HandleWindowCreate(reader, writer);
            return true;
        case OpcodeController::WindowMaximize:
            HandleWindowMaximize(reader, writer);
            return true;
        case OpcodeController::WindowMinimize:
            HandleWindowMinimize(reader, writer);
            return true;
        case OpcodeController::WindowRestore:
            HandleWindowRestore(reader, writer);
            return true;
        case OpcodeController::WindowShow:
            HandleWindowShow(reader, writer);
            return true;
        case OpcodeController::WindowHide:
            HandleWindowHide(reader, writer);
            return true;
        case OpcodeController::WindowActivate:
            HandleWindowActivate(reader, writer);
            return true;
        case OpcodeController::WindowBringToTop:
            HandleWindowBringToTop(reader, writer);
            return true;
        case OpcodeController::WindowSetAlwaysOnTop:
            HandleWindowSetAlwaysOnTop(reader, writer);
            return true;
        case OpcodeController::WindowSetFullscreen:
            HandleWindowSetFullscreen(reader, writer);
            return true;
        case OpcodeController::WindowCenterSelf:
            HandleWindowCenterSelf(reader, writer);
            return true;
        case OpcodeController::WindowSetProxyRequests:
            HandleWindowSetProxyRequests(reader, writer);
            return true;
        case OpcodeController::WindowSetPosition:
            HandleWindowSetPosition(reader, writer);
            return true;
        case OpcodeController::WindowGetPosition:
            HandleWindowGetPosition(reader, writer);
            return true;
        case OpcodeController::WindowSetDevelopmentToolsEnabled:
            HandleWindowSetDevelopmentToolsEnabled(reader, writer);
            return true;
        case OpcodeController::WindowSetDevelopmentToolsVisible:
            HandleWindowSetDevelopmentToolsVisible(reader, writer);
            return true;
        case OpcodeController::WindowClose:
            HandleWindowClose(reader, writer);
            return true;
        case OpcodeController::WindowLoadUrl:
            HandleWindowLoadUrl(reader, writer);
            return true;
        case OpcodeController::WindowSetZoom:
            HandleWindowSetZoom(reader, writer);
            return true;
        case OpcodeController::WindowGetZoom:
            HandleWindowGetZoom(reader, writer);
            return true;
        case OpcodeController::WindowRequestFocus:
            HandleWindowRequestFocus(reader, writer);
            return true;
        case OpcodeController::WindowSetModifyRequests:
            HandleWindowSetModifyRequests(reader, writer);
            return true;
        case OpcodeController::StreamOpen:
        {
            std::lock_guard<std::mutex> lk(_dataStreamsMutex);
            std::optional<uint32_t> identifier = reader.read<uint32_t>();
            if (identifier)
            {
                LOG(INFO) << "Stream opened with identifier (via open packet) " << *identifier;
                auto itr = _dataStreams.find(*identifier);
                if (itr == _dataStreams.end())
                    _dataStreams[*identifier] = std::make_shared<DataStream>(*identifier);
                else
                    LOG(INFO) << "Stream not opened, was already open (via open packet) " << *identifier;
            }
            return true;
        }
        case OpcodeController::StreamData:
        {
            //TODO: Somehow initially it fails sometimes (particularly initially or when skipping in a video)
            std::optional<uint32_t> identifier = reader.read<uint32_t>();
            if (identifier)
            {
                std::shared_ptr<DataStream> dataStream = nullptr;

                {
                    std::lock_guard<std::mutex> lk(_dataStreamsMutex);

                    auto itr = _dataStreams.find(*identifier);
                    if (itr != _dataStreams.end())
                        dataStream = (*itr).second;
                }

                if (dataStream)
                {
                    reader.copyTo([dataStream] (const uint8_t* data, size_t size) {
                        dataStream->Write(data, size);
                        return true;
                    }, reader.remainingSize());
                    writer.write<bool>(true);
                }
                else
                    writer.write<bool>(false);
            }

            return true;
        }
        case OpcodeController::StreamClose:
        {
            std::lock_guard<std::mutex> lk(_dataStreamsMutex);
            std::optional<uint32_t> identifier = reader.read<uint32_t>();
            if (identifier)
            {
                LOG(INFO) << "Stream closed with identifier " << *identifier;
                auto itr = _dataStreams.find(*identifier);
                if (itr != _dataStreams.end())
                {
                    (*itr).second->Close();
                    _dataStreams.erase(itr);
                }
            }
            return true;
        }
        case OpcodeController::PickDirectory:
            HandleWindowOpenDirectoryPicker(reader, writer);
            return true;
        case OpcodeController::PickFile:
            HandleWindowOpenFilePicker(reader, writer);
            return true;
        case OpcodeController::SaveFile:
            HandleWindowSaveFilePicker(reader, writer);
            return true;
        case OpcodeController::WindowExecuteDevToolsMethod:
            HandleWindowExecuteDevToolsMethod(reader, writer);
            return true;
        case OpcodeController::WindowSetTitle:
            HandleWindowSetTitle(reader, writer);
            return true;
        case OpcodeController::WindowSetIcon:
            HandleWindowSetIcon(reader, writer);
            return true;
        case OpcodeController::WindowAddUrlToProxy:
            HandleAddUrlToProxy(reader, writer);
            return true;
        case OpcodeController::WindowRemoveUrlToProxy:
            HandleRemoveUrlToProxy(reader, writer);
            return true;
        case OpcodeController::WindowAddDomainToProxy:
            HandleAddDomainToProxy(reader, writer);
            return true;
        case OpcodeController::WindowRemoveDomainToProxy:
            HandleRemoveDomainToProxy(reader, writer);
            return true;
        case OpcodeController::WindowAddUrlToModify:
            HandleAddUrlToModify(reader, writer);
            return true;
        case OpcodeController::WindowRemoveUrlToModify:
            HandleRemoveUrlToModify(reader, writer);
            return true;
        case OpcodeController::WindowGetSize:
            HandleWindowGetSize(reader, writer);
            return true;
        case OpcodeController::WindowSetSize:
            HandleWindowSetSize(reader, writer);
            return true;
        case OpcodeController::WindowAddDevToolsEventMethod:
            HandleAddDevToolsEventMethod(reader, writer);
            return true;
        case OpcodeController::WindowRemoveDevToolsEventMethod:
            HandleRemoveDevToolsEventMethod(reader, writer);
            return true;
        case OpcodeController::WindowBridgeRpc:
            return HandleWindowBridgeRpc(requestId, reader, writer);
        default:
            LOG(ERROR) << "Unknown opcode " << (uint32_t)opcode << ".";
            return true;
    }
}

void IPC::HandleNotification(OpcodeControllerNotification opcode, PacketReader& reader)
{
    switch (opcode)
    {
        case OpcodeControllerNotification::Exit:
            LOG(ERROR) << "Exit received.";
            CloseEverything();
            break;
        default:
            LOG(ERROR) << "Unknown notification opcode " << (uint32_t)opcode << ".";
            break;
    }
}

std::vector<uint8_t> IPC::Echo(const uint8_t* data, size_t size)
{
    return Call(OpcodeClient::Echo, data, size);
}

void IPC::Ping()
{
    Call(OpcodeClient::Ping);
}

void IPC::Print(const char* message, size_t size)
{
    Call(OpcodeClient::Print, (uint8_t*)message, size);
}

void IPC::Print(const std::string& message)
{
    Call(OpcodeClient::Print, (uint8_t*)message.data(), message.size());
}

void IPC::CloseStream(uint32_t identifier)
{
    LOG(INFO) << "Closed stream with identifier " << identifier;

    std::lock_guard<std::mutex> lk(_dataStreamsMutex);
    auto itr = _dataStreams.find(identifier);
    if (itr != _dataStreams.end())
    {
        (*itr).second->Close();
        _dataStreams.erase(itr);
    }
    
    if (!IsAvailable()) {
        return;
    }

    StreamClose(identifier);
}

std::unique_ptr<IPCProxyResponse> IPC::WindowProxyRequest(int32_t identifier, CefRefPtr<CefRequest> request)
{
    if (!IsAvailable()) {
        return nullptr;
    }

    PacketWriter writer;
    writer.write<int32_t>(identifier);
    writer.writeSizePrefixedString(request->GetMethod());
    writer.writeSizePrefixedString(request->GetURL());

    CefRequest::HeaderMap headers;
    request->GetHeaderMap(headers);
    writer.write<int32_t>((int32_t)headers.size());
    for (auto& header : headers) 
    {
        writer.writeSizePrefixedString(header.first);
        writer.writeSizePrefixedString(header.second);
    }

    CefRefPtr<CefPostData> postData = request->GetPostData();
    if (postData.get()) //TODO: Is postData ever nullptr ?
    {
        writer.write<int32_t>((int32_t)postData->GetElementCount());

        if (postData->GetElementCount())
        {
            std::vector<CefRefPtr<CefPostDataElement>> elements;
            postData->GetElements(elements);
            for (auto& element : elements)
            {
                uint8_t elementType = static_cast<uint8_t>(element->GetType());
                writer.write<uint8_t>(elementType);
                if (elementType == CefPostDataElement::Type::PDE_TYPE_BYTES) 
                {
                    size_t dataSize = element->GetBytesCount();
                    std::vector<uint8_t> data(dataSize);
                    element->GetBytes(dataSize, data.data());
                    uint32_t dataSize32 = static_cast<uint32_t>(dataSize);
                    writer.write<uint32_t>(dataSize32);
                    writer.writeBytes(data.data(), data.size());
                } 
                else if (elementType == CefPostDataElement::Type::PDE_TYPE_FILE) 
                    writer.writeSizePrefixedString(element->GetFile());
            }
        }
    }
    else
        writer.write<int32_t>(0);

    std::vector<uint8_t> response = Call(OpcodeClient::WindowProxyRequest, writer.data(), writer.size());
    std::unique_ptr<IPCProxyResponse> result = nullptr;

    if (!response.empty())
    {
        PacketReader reader(response.data(), response.size());

        // Deserialize method
        std::optional<uint32_t> statusCode = reader.read<uint32_t>();
        if (!statusCode) {
            LOG(ERROR) << "Failed to read status code.";
            return nullptr;
        }

        std::optional<std::string> statusText = reader.readSizePrefixedString();
        if (!statusText) {
            LOG(ERROR) << "Failed to read status text.";
            return nullptr;
        }

        // Deserialize headers
        std::optional<uint32_t> responseHeaderCount = reader.read<uint32_t>();
        if (!responseHeaderCount) {
            LOG(ERROR) << "Failed to read response header count.";
            return nullptr;
        }

        std::optional<std::string> mediaType = std::nullopt;
        std::multimap<std::string, std::string> responseHeaders;
        for (uint32_t i = 0; i < *responseHeaderCount; ++i) 
        {
            std::optional<std::string> key = reader.readSizePrefixedString();
            if (!key) {
                LOG(ERROR) << "Failed to read response header key text.";
                return nullptr;
            }

            std::optional<std::string> value = reader.readSizePrefixedString();
            if (!value) {
                LOG(ERROR) << "Failed to read response header value text.";
                return nullptr;
            }

            if (key && value && (*key).c_str() && (*value).c_str() && 
                #ifdef _WIN32
                stricmp((*key).c_str(), "content-type") == 0
                #else
                strcasecmp((*key).c_str(), "content-type") == 0
                #endif
                ) {
                size_t semicolonPos = (*value).find(';');
                mediaType = semicolonPos != std::string::npos ? (*value).substr(0, semicolonPos) : *value;
            }

            responseHeaders.insert({ *key, *value });
        }

        // Deserialize elements
        std::optional<uint8_t> bodyType = reader.read<uint8_t>();
        if (!bodyType) {
            LOG(ERROR) << "Failed to read body type.";
            return nullptr;
        }

        std::optional<std::vector<uint8_t>> body = std::nullopt;
        std::shared_ptr<DataStream> bodyStream = nullptr;
        if (*bodyType == 1)
        {
            std::optional<uint32_t> bodySize = reader.read<uint32_t>();
            if (!bodySize) {
                LOG(ERROR) << "Failed to read body size.";
                return nullptr;
            }

            if (*bodySize > 0)
            {
                std::vector<uint8_t> data(*bodySize);
                if (!reader.readBytes(data.data(), *bodySize))
                {
                    LOG(ERROR) << "Proxy missing body (bodySize = " << *bodySize << ", remainingSize = " << reader.remainingSize() << ")";
                    return nullptr;
                }

                body = data;
            }
        }
        else if (*bodyType == 2)
        {
            std::optional<uint32_t> streamId = reader.read<uint32_t>();
            if (!streamId) {
                LOG(ERROR) << "Failed to read stream id.";
                return nullptr;
            }

            std::lock_guard<std::mutex> lk(_dataStreamsMutex);
            auto itr = _dataStreams.find(*streamId);
            if (itr != _dataStreams.end()) {
                bodyStream = (*itr).second;
            } else {
                LOG(INFO) << "Stream opened with identifier (was not opened via open packet)" << *streamId;
                bodyStream = std::make_shared<DataStream>(*streamId);
                _dataStreams[*streamId] = bodyStream;
            }
        }

        result = std::unique_ptr<IPCProxyResponse>(new IPCProxyResponse());
        result->status_code = (int32_t)*statusCode;
        result->status_text = *statusText;
        result->headers = responseHeaders;
        result->media_type = mediaType;
        result->body = body;
        result->bodyStream = bodyStream;
        
        return result;
    }

    return nullptr;
}

void IPC::WindowModifyRequest(int32_t identifier, CefRefPtr<CefRequest> request, bool modifyRequestBody)
{
    if (!IsAvailable()) {
        return;
    }

    std::vector<uint8_t> response;
    {
        PacketWriter writer;
        writer.write<int32_t>(identifier);
        writer.writeSizePrefixedString(request->GetMethod());
        writer.writeSizePrefixedString(request->GetURL());
            
        CefRequest::HeaderMap headers;
        request->GetHeaderMap(headers);
        writer.write<int32_t>((int32_t)headers.size());
        for (auto& header : headers) 
        {
            writer.writeSizePrefixedString(header.first);
            writer.writeSizePrefixedString(header.second);
        }

        CefRefPtr<CefPostData> postData = request->GetPostData();
        if (modifyRequestBody && postData.get()) //TODO: Is postData ever nullptr ?
        {
            writer.write<int32_t>((int32_t)postData->GetElementCount());

            if (postData->GetElementCount())
            {
                std::vector<CefRefPtr<CefPostDataElement>> elements;
                postData->GetElements(elements);
                for (auto& element : elements)
                {
                    uint8_t elementType = static_cast<uint8_t>(element->GetType());
                    writer.write<uint8_t>(elementType);
                    if (elementType == CefPostDataElement::Type::PDE_TYPE_BYTES) 
                    {
                        size_t dataSize = element->GetBytesCount();
                        std::vector<uint8_t> data(dataSize);
                        element->GetBytes(dataSize, data.data());
                        uint32_t dataSize32 = static_cast<uint32_t>(dataSize);

                        writer.write<uint32_t>(dataSize32);
                        writer.writeBytes(data.data(), data.size());
                    } 
                    else if (elementType == CefPostDataElement::Type::PDE_TYPE_FILE) 
                        writer.writeSizePrefixedString(element->GetFile());
                }
            }
        }
        else
            writer.write<int32_t>(0);

        response = Call(OpcodeClient::WindowModifyRequest, writer.data(), writer.size());
    }
    
    if (!response.empty())
    {
        PacketReader reader(response.data(), response.size());

        std::optional<std::string> method = reader.readSizePrefixedString();
        if (!method) {
            LOG(ERROR) << "Failed to read method.";
            return;
        }

        std::optional<std::string> url = reader.readSizePrefixedString();
        if (!url) {
            LOG(ERROR) << "Failed to read url.";
            return;
        }

        // Deserialize headers
        std::optional<uint32_t> headerCount = reader.read<uint32_t>();
        if (!headerCount) {
            LOG(ERROR) << "Failed to read header count.";
            return;
        }

        CefRequest::HeaderMap headers;
        for (uint32_t i = 0; i < *headerCount; ++i) 
        {
            std::optional<std::string> key = reader.readSizePrefixedString();
            if (!key) {
                LOG(ERROR) << "Failed to read key.";
                return;
            }
            std::optional<std::string> value = reader.readSizePrefixedString();
            if (!value) {
                LOG(ERROR) << "Failed to read value.";
                return;
            }

            headers.insert(std::make_pair(*key, *value));
        }

        // Deserialize elements
        std::optional<uint32_t> elementCount = reader.read<uint32_t>();
        if (!elementCount) {
            LOG(ERROR) << "Failed to read element count.";
            return;
        }

        if (modifyRequestBody)
        {
            CefRefPtr<CefPostData> postData = CefPostData::Create();
            for (uint32_t i = 0; i < *elementCount; ++i) 
            {
                std::optional<uint8_t> elementType = reader.read<uint8_t>();
                if (!elementType) {
                    LOG(ERROR) << "Failed to read element type.";
                    return;
                }

                if (*elementType == CefPostDataElement::Type::PDE_TYPE_BYTES) 
                {
                    std::optional<uint32_t> dataSize = reader.read<uint32_t>();
                    if (!dataSize) {
                        LOG(ERROR) << "Failed to read data size.";
                        return;
                    }
                    if (!reader.hasAvailable(*dataSize)) {
                        LOG(ERROR) << "Not enough data available to read body.";
                        return;
                    }

                    CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
                    reader.copyTo([element] (const uint8_t* data, size_t size) {
                        element->SetToBytes(size, data);
                        return true;
                    }, *dataSize);

                    postData->AddElement(element);
                } 
                else if (*elementType == CefPostDataElement::Type::PDE_TYPE_FILE) 
                {
                    std::optional<std::string> fileName = reader.readSizePrefixedString();
                    if (!fileName) {
                        LOG(ERROR) << "Failed to read file name.";
                        return;
                    }
                    CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
                    element->SetToFile(*fileName);
                    postData->AddElement(element);
                }
            }

            request->SetPostData(postData);
        }

        //LOG(INFO) << "Request modifier:\n  Method: " << *method << "\nURL: " << *url << "\nHeaders: ";
        //for (const auto& header : headers) {
        //    LOG(INFO) << "  " << header.first << ": " << header.second;
        //}

        request->SetMethod(*method);
        request->SetURL(*url);
        request->SetHeaderMap(headers);
    }
}

IPCBridgeRpcResult IPC::WindowBridgeRpc(int32_t identifier, const std::string& method, const std::string& payload_json)
{
    if (!IsAvailable()) {
        return MakeBridgeRpcResult(false, "null", "IPC is not available.");
    }

    PacketWriter writer;
    writer.write<int32_t>(identifier);
    writer.writeSizePrefixedString(method);
    writer.writeSizePrefixedString(payload_json);

    std::vector<uint8_t> response = Call(OpcodeClient::WindowBridgeRpc, writer.data(), writer.size());
    if (response.empty()) {
        return MakeBridgeRpcResult(false, "null", "Bridge RPC returned an empty response.");
    }

    PacketReader reader(response.data(), response.size());
    std::optional<bool> success = reader.read<bool>();
    std::optional<std::string> result_json = reader.readSizePrefixedString();
    std::optional<std::string> error = reader.readSizePrefixedString();
    if (!success || !result_json || !error) {
        return MakeBridgeRpcResult(false, "null", "Failed to parse the bridge RPC response.");
    }

    return MakeBridgeRpcResult(*success, *result_json, *error);
}

void IPC::NotifyWindowOpened(CefRefPtr<CefBrowser> browser)
{
    uint8_t packet[sizeof(int32_t)];
    *(int32_t*)packet = browser->GetIdentifier();
    Notify(OpcodeClientNotification::WindowOpened, packet, sizeof(packet));
}

void IPC::NotifyWindowClosed(CefRefPtr<CefBrowser> browser)
{
    uint8_t packet[sizeof(int32_t)];
    *(int32_t*)packet = browser->GetIdentifier();
    Notify(OpcodeClientNotification::WindowClosed, packet, sizeof(packet));
}

void IPC::NotifyWindowFocused(CefRefPtr<CefBrowser> browser)
{
    uint8_t packet[sizeof(int32_t)];
    *(int32_t*)packet = browser->GetIdentifier();
    Notify(OpcodeClientNotification::WindowFocused, packet, sizeof(packet));
}

void IPC::NotifyWindowUnfocused(CefRefPtr<CefBrowser> browser)
{
    uint8_t packet[sizeof(int32_t)];
    *(int32_t*)packet = browser->GetIdentifier();
    Notify(OpcodeClientNotification::WindowUnfocused, packet, sizeof(packet));
}

/*void IPC::NotifyWindowResized(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height) 
{
    uint8_t packet[sizeof(CefBrowser*) + 4 * sizeof(int)];
    *(int32_t*)packet = browser->GetIdentifier();
    int* resizeEvent = (int*)((uint8_t*)packet + sizeof(CefBrowser*));
    resizeEvent[0] = x;
    resizeEvent[1] = y;
    resizeEvent[2] = width;
    resizeEvent[3] = height;
    Notify(OpcodeClientNotification::WindowResized, packet, sizeof(packet)); 
}

void IPC::NotifyWindowMoved(CefRefPtr<CefBrowser> browser, int x, int y, int width, int height) 
{ 
    uint8_t packet[sizeof(CefBrowser*) + 4 * sizeof(int)];
    *(int32_t*)packet = browser->GetIdentifier();
    int* resizeEvent = (int*)((uint8_t*)packet + sizeof(CefBrowser*));
    resizeEvent[0] = x;
    resizeEvent[1] = y;
    resizeEvent[2] = width;
    resizeEvent[3] = height;
    Notify(OpcodeClientNotification::WindowMoved, packet, sizeof(packet)); 
}

void IPC::NotifyWindowKeyboardEvent(CefRefPtr<CefBrowser> browser, const cef_key_event_t& event)
{    
    uint8_t packet[sizeof(CefBrowser*) + sizeof(IPCKeyEvent)];
    *(int32_t*)packet = browser->GetIdentifier();
    IPCKeyEvent* pKeyEvent = (IPCKeyEvent*)((uint8_t*)packet + sizeof(CefBrowser*));
    pKeyEvent->type = event.type;
    pKeyEvent->modifiers = event.modifiers;
    pKeyEvent->windows_key_code = event.windows_key_code;
    pKeyEvent->native_key_code = event.native_key_code;
    pKeyEvent->is_system_key = event.is_system_key;
    pKeyEvent->character = event.character;
    pKeyEvent->unmodified_character = event.character;
    pKeyEvent->focus_on_editable_field = event.focus_on_editable_field;
    Notify(OpcodeClientNotification::WindowKeyboardEvent, packet, sizeof(packet)); 
}

void IPC::NotifyWindowMinimized(CefRefPtr<CefBrowser> browser)
{
    Notify(OpcodeClientNotification::WindowMinimized, (const uint8_t*)browser->GetIdentifier(), sizeof(CefBrowser*));
}

void IPC::NotifyWindowMaximized(CefRefPtr<CefBrowser> browser)
{
    Notify(OpcodeClientNotification::WindowMaximized, (const uint8_t*)browser->GetIdentifier(), sizeof(CefBrowser*));
}

void IPC::NotifyWindowRestored(CefRefPtr<CefBrowser> browser)
{
    Notify(OpcodeClientNotification::WindowRestored, (const uint8_t*)browser->GetIdentifier(), sizeof(CefBrowser*));
}*/

void IPC::NotifyWindowFullscreenChanged(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.write(fullscreen);
    Notify(OpcodeClientNotification::WindowFullscreenChanged, writer);
}

void IPC::NotifyWindowLoadStart(CefRefPtr<CefBrowser> browser, const CefString& url)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(url);
    Notify(OpcodeClientNotification::WindowLoadStart, writer);
}

void IPC::NotifyWindowLoadEnd(CefRefPtr<CefBrowser> browser, const CefString& url)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(url);
    Notify(OpcodeClientNotification::WindowLoadEnd, writer);
}

void IPC::NotifyWindowDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const uint8_t* result, size_t result_size)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(method);
    writer.write((int)result_size);
    writer.writeBytes(result, result_size);
    Notify(OpcodeClientNotification::WindowDevToolsEvent, writer);
}

void IPC::NotifyWindowLoadError(CefRefPtr<CefBrowser> browser, cef_errorcode_t errorCode, const CefString& errorText, const CefString& url)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.write((int32_t)errorCode);
    writer.writeSizePrefixedString(errorText);
    writer.writeSizePrefixedString(url);
    Notify(OpcodeClientNotification::WindowLoadError, writer);
}

#ifdef _WIN32
void IPC::SetHandles(HANDLE readHandle, HANDLE writeHandle) 
{
    _pipe.SetHandles(readHandle, writeHandle);
}
#else
void IPC::SetHandles(int readFd, int writeFd) 
{
    _pipe.SetHandles(readFd, writeFd);
}
#endif

class WindowDelegate : public CefWindowDelegate {
public:
    explicit WindowDelegate(CefRefPtr<CefBrowserView> browser_view, cef_runtime_style_t runtime_style, cef_show_state_t initial_show_state, const IPCWindowCreate& settings)
        : browser_view_(browser_view), _settings(settings), runtime_style_(runtime_style), initial_show_state_(initial_show_state) {}

    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        window->AddChildView(browser_view_);
        if (initial_show_state_ != CEF_SHOW_STATE_HIDDEN) {
            window->Show();
        }
    }

    void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
        browser_view_ = nullptr;
    }

    bool CanClose(CefRefPtr<CefWindow> window) override {
        CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
        if (browser) {
            return browser->GetHost()->TryCloseBrowser();
        }
        return true;
    }

    cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow> window) override {
        return initial_show_state_;
    }

    cef_runtime_style_t GetWindowRuntimeStyle() override {
        return runtime_style_;
    }

#if defined(OS_LINUX)
    bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window, CefLinuxWindowProperties& properties) override {
        CefString(&properties.wayland_app_id) = CefString(&properties.wm_class_class) = CefString(&properties.wm_class_name) = CefString(&properties.wm_role_name) = _settings.appId ? *_settings.appId : "cef";
        return true;
    }
#endif

    bool IsFrameless(CefRefPtr<CefWindow> window) override { return _settings.frameless == 1; }
    bool CanResize(CefRefPtr<CefWindow> window) override { return _settings.resizable == 1; }

    CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
        return CefSize(_settings.preferredWidth, _settings.preferredHeight);
    }

    /*CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
        return CefSize(800, 600);
    }*/

    /*CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
        return CefSize(_settings.minimumWidth, _settings.minimumHeight);
    }*/

private:
    CefRefPtr<CefBrowserView> browser_view_;
    const IPCWindowCreate& _settings;
    const cef_runtime_style_t runtime_style_;
    const cef_show_state_t initial_show_state_;

    IMPLEMENT_REFCOUNTING(WindowDelegate);
    DISALLOW_COPY_AND_ASSIGN(WindowDelegate);
};

class DevToolsWindowDelegate : public CefWindowDelegate {
    public:
        explicit DevToolsWindowDelegate(CefRefPtr<CefBrowserView> browser_view)
            : browser_view_(browser_view) {}
    
        void OnWindowCreated(CefRefPtr<CefWindow> window) override {
            window->AddChildView(browser_view_);
        }
    
        void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
            browser_view_ = nullptr;
        }
    
        bool CanClose(CefRefPtr<CefWindow> window) override {
            CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
            if (browser) {
                return browser->GetHost()->TryCloseBrowser();
            }
            return true;
        }

    private:
        CefRefPtr<CefBrowserView> browser_view_;
    
        IMPLEMENT_REFCOUNTING(DevToolsWindowDelegate);
        DISALLOW_COPY_AND_ASSIGN(DevToolsWindowDelegate);
    };

class BrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit BrowserViewDelegate(const IPCWindowCreate& settings, cef_runtime_style_t runtime_style)
      : _settings(settings), runtime_style_(runtime_style) {}

    bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools) override {
        if (is_devtools) {
            CefWindow::CreateTopLevelWindow(new DevToolsWindowDelegate(popup_browser_view));
        } else {
            cef_show_state_t showState = _settings.shown 
                ? (_settings.fullscreen ? CEF_SHOW_STATE_FULLSCREEN : CEF_SHOW_STATE_NORMAL)
                : CEF_SHOW_STATE_HIDDEN;
            CefWindow::CreateTopLevelWindow(new WindowDelegate(popup_browser_view, runtime_style_, showState, _settings));
        }
        return true;
    }

    cef_runtime_style_t GetBrowserRuntimeStyle() override {
        return runtime_style_;
    }

 private:
    const IPCWindowCreate& _settings;
    const cef_runtime_style_t runtime_style_;

    IMPLEMENT_REFCOUNTING(BrowserViewDelegate);
    DISALLOW_COPY_AND_ASSIGN(BrowserViewDelegate);
};

CefRefPtr<Client> CreateBrowserWindow(const IPCWindowCreate& windowCreate)
{
    CEF_REQUIRE_UI_THREAD();

    LOG(INFO) << "Window create (URL = '" << windowCreate.url << "')";

    CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
    const bool headless = command_line->HasSwitch("headless");

    // Check if Alloy style will be used.
    cef_runtime_style_t runtime_style = CEF_RUNTIME_STYLE_DEFAULT;
    bool use_alloy_style = command_line->HasSwitch("use-alloy-style");
    if (use_alloy_style) {
        runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    }
    bool use_chrome_style = command_line->HasSwitch("use-chrome-style");
    if (use_chrome_style) {
        runtime_style = CEF_RUNTIME_STYLE_CHROME;
    }

    CefRefPtr<Client> client = new Client(windowCreate);
    CefBrowserSettings settings;
    CefRefPtr<CefDictionaryValue> extra_info = CreateBridgeExtraInfo(windowCreate.bridgeEnabled);

    if (headless) {
        CefWindowInfo wi;
        wi.SetAsWindowless(kNullWindowHandle);
        wi.bounds.width = windowCreate.preferredWidth;
        wi.bounds.height = windowCreate.preferredHeight;
        CefBrowserHost::CreateBrowserSync(wi, client, windowCreate.url, settings, extra_info, nullptr);

        return client;
    }

    const bool use_views = !command_line->HasSwitch("use-native");
    LOG(INFO) << "Use views = " << (use_views ? "true" : "false");

    cef_show_state_t showState = windowCreate.shown 
        ? (windowCreate.fullscreen ? CEF_SHOW_STATE_FULLSCREEN : CEF_SHOW_STATE_NORMAL)
        : CEF_SHOW_STATE_HIDDEN;

    if (use_views)
    {
        CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(client, windowCreate.url, settings, extra_info, nullptr, new BrowserViewDelegate(windowCreate, runtime_style));
        CefWindow::CreateTopLevelWindow(new WindowDelegate(browser_view, runtime_style, showState, windowCreate));
    } 
    else 
    {
        CefWindowInfo window_info;
        window_info.bounds.width = windowCreate.preferredWidth;
        window_info.bounds.height = windowCreate.preferredHeight;
        window_info.runtime_style = runtime_style;

#if defined(OS_WIN)
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        if (windowCreate.shown) {
            style |= WS_VISIBLE;
        }
        window_info.style = style;
        window_info.parent_window = nullptr;
        window_info.bounds.x = CW_USEDEFAULT;
        window_info.bounds.y = CW_USEDEFAULT;

        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            typedef HRESULT(WINAPI* GetDpiForMonitorPtr)(HMONITOR, int, UINT*, UINT*);
            GetDpiForMonitorPtr GetDpiForMonitor = reinterpret_cast<GetDpiForMonitorPtr>(GetProcAddress(shcore, "GetDpiForMonitor"));
            if (GetDpiForMonitor) {
                POINT placementPoint = {
                    (window_info.bounds.x == CW_USEDEFAULT) ? 0 : window_info.bounds.x, 
                    (window_info.bounds.y == CW_USEDEFAULT) ? 0 : window_info.bounds.y 
                };

                HMONITOR monitor = MonitorFromPoint(placementPoint, MONITOR_DEFAULTTONEAREST);
                
                UINT dpiX = 96, dpiY = 96; // Default DPI (96 DPI = 100% scaling)
                if (SUCCEEDED(GetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY))) {
                    float scaleFactor = dpiX / 96.0f;
                    window_info.bounds.width = scaleFactor * window_info.bounds.width;
                    window_info.bounds.height = scaleFactor * window_info.bounds.height;
                }
            }
        }
#endif

        // TODO: Copy over window name
        // cef_string_copy(windowName.c_str(), windowName.length(), &window_name);

        CefBrowserHost::CreateBrowserSync(window_info, client, windowCreate.url, settings, extra_info, nullptr);
    }

    return client;
}

CefRefPtr<Client> HandleWindowCreateInternal(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<CefRefPtr<Client>> promise;
        std::future<CefRefPtr<Client>> future = promise.get_future();
        CefPostTask(TID_UI, base::BindOnce([](std::promise<CefRefPtr<Client>> promise, PacketReader& reader, PacketWriter& writer) {
            promise.set_value(HandleWindowCreateInternal(reader, writer));
        }, std::move(promise), std::ref(reader), std::ref(writer)));
        
        return future.get();
    }

    std::optional<bool> resizable = reader.read<bool>();
    std::optional<bool> frameless = reader.read<bool>();
    std::optional<bool> fullscreen = reader.read<bool>();
    std::optional<bool> centered = reader.read<bool>();
    std::optional<bool> shown = reader.read<bool>();
    std::optional<bool> contextMenuEnable = reader.read<bool>();
    std::optional<bool> developerToolsEnabled = reader.read<bool>();
    std::optional<bool> modifyRequests = reader.read<bool>();
    std::optional<bool> modifyRequestBody = reader.read<bool>();
    std::optional<bool> proxyRequests = reader.read<bool>();
    std::optional<bool> logConsole = reader.read<bool>();
    std::optional<bool> bridgeEnabled = reader.read<bool>();
    std::optional<int32_t> minimumWidth = reader.read<int32_t>();
    std::optional<int32_t> minimumHeight = reader.read<int32_t>();
    std::optional<int32_t> preferredWidth = reader.read<int32_t>();
    std::optional<int32_t> preferredHeight = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    std::optional<std::string> title = reader.readSizePrefixedString();
    std::optional<std::string> iconPath = reader.readSizePrefixedString();
    std::optional<std::string> appId = reader.readSizePrefixedString();
    if (!resizable || !frameless || !fullscreen || !centered || !shown || !contextMenuEnable || !developerToolsEnabled || !modifyRequests
        || !modifyRequestBody || !proxyRequests || !logConsole || !bridgeEnabled || !minimumWidth || !minimumHeight || !preferredWidth
        || !preferredHeight || !url) {

        LOG(ERROR) << "HandleWindowCreate called without valid data. Ignored.";
        return nullptr;
    }

    IPCWindowCreate windowCreate;
    windowCreate.resizable = *resizable;
    windowCreate.frameless = *frameless;
    windowCreate.fullscreen = *fullscreen;
    windowCreate.centered = *centered;
    windowCreate.shown = *shown;
    windowCreate.contextMenuEnable = *contextMenuEnable;
    windowCreate.developerToolsEnabled = *developerToolsEnabled;
    windowCreate.modifyRequests = *modifyRequests;
    windowCreate.modifyRequestBody = *modifyRequestBody;
    windowCreate.proxyRequests = *proxyRequests;
    windowCreate.logConsole = *logConsole;
    windowCreate.bridgeEnabled = *bridgeEnabled;
    windowCreate.minimumWidth = *minimumWidth;
    windowCreate.minimumHeight = *minimumHeight;
    windowCreate.preferredWidth = *preferredWidth;
    windowCreate.preferredHeight = *preferredHeight;
    windowCreate.url = *url;
    windowCreate.title = title;
    windowCreate.iconPath = iconPath;
    windowCreate.appId = appId;
    return CreateBrowserWindow(windowCreate);
}

void HandleWindowCreate(PacketReader& reader, PacketWriter& writer)
{
    CefRefPtr<Client> client = HandleWindowCreateInternal(reader, writer);
    LOG(INFO) << "Client created with identifier " << client->GetIdentifier();
    writer.write<int32_t>(client->GetIdentifier());
}

bool HandleWindowBridgeRpc(uint32_t requestId, PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    std::optional<std::string> payload_json = reader.readSizePrefixedString();
    if (!identifier || !method || !payload_json) {
        WriteBridgeRpcResult(writer, false, "null", "WindowBridgeRpc called without valid data.");
        return true;
    }

    if (CefCurrentlyOn(TID_UI)) {
        WriteBridgeRpcResult(writer, false, "null", "WindowBridgeRpc cannot block the CEF UI thread.");
        return true;
    }

    if (!CefPostTask(TID_UI, 
        base::BindOnce([](uint32_t requestId, int32_t identifier, std::string method, std::string payload_json) {
            PacketWriter writer;
            CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(identifier);
            if (!browser) {
                WriteBridgeRpcResult(writer, false, "null", "HandleWindowBridgeRpc called while the browser is already closed.");
                IPC::Singleton.QueueResponse(OpcodeController::WindowBridgeRpc, requestId, writer);
                return;
            }

            CefRefPtr<CefClient> cef_client = browser->GetHost()->GetClient();
            Client* client = static_cast<Client*>(cef_client.get());
            if (!client) {
                WriteBridgeRpcResult(writer, false, "null", "HandleWindowBridgeRpc failed to acquire the client.");
                IPC::Singleton.QueueResponse(OpcodeController::WindowBridgeRpc, requestId, writer);
                return;
            }

            client->StartBridgeRpcCall(browser, method, payload_json, requestId);
        }, requestId, *identifier, *method, *payload_json))
    ) {
        WriteBridgeRpcResult(writer, false, "null", "WindowBridgeRpc failed to post work to the CEF UI thread.");
        return true;
    }
    return false;
}

void HandleWindowMaximize(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowMaximize(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowMaximize called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowMaximize called while CefBrowser " << *identifier << " is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browserView = CefBrowserView::GetForBrowser(browser);
    if (browserView)
    {
        CefRefPtr<CefWindow> window = browserView->GetWindow();
        window->Maximize();
    } else {
        shared::PlatformMaximize(browser);
    }
}

void HandleWindowMinimize(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowMinimize(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowMinimize called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowMinimize called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Minimize();
    } else {
        shared::PlatformMinimize(browser);
    }

}

void HandleWindowRestore(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowRestore(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowRestore called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowRestore called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Restore();
    } else {
        shared::PlatformRestore(browser);
    }
}

void HandleWindowShow(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowShow(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowShow called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowShow called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Show();
    } else {
        shared::PlatformShow(browser);
    }
}

void HandleWindowHide(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowHide(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowHide called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowHide called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Hide();
    } else {
        shared::PlatformHide(browser);
    }
}

void HandleWindowActivate(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowActivate(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowActivate called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowActivate called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Activate();
    } else {
        shared::PlatformActivate(browser);
    }
}

void HandleWindowBringToTop(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowBringToTop(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowBringToTop called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowBringToTop called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->BringToTop();
    } else {
        shared::PlatformBringToTop(browser);
    }
}

void HandleWindowSetAlwaysOnTop(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetAlwaysOnTop(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> alwaysOnTop = reader.read<bool>();
    if (!identifier || !alwaysOnTop)
    {
        LOG(ERROR) << "HandleWindowSetAlwaysOnTop called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetAlwaysOnTop called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetAlwaysOnTop(*alwaysOnTop);
    } else {
        shared::PlatformSetAlwaysOnTop(browser, *alwaysOnTop);
    }
}

void HandleWindowSetFullscreen(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetFullscreen(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> fullscreen = reader.read<bool>();
    if (!identifier || !fullscreen)
    {
        LOG(ERROR) << "HandleWindowSetFullscreen called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetFullscreen called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetFullscreen(*fullscreen ? 1 : 0);
    } else {
        shared::PlatformSetFullscreen(browser, *fullscreen == 1);
    }
}

void HandleWindowCenterSelf(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowCenterSelf(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowCenterSelf called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowCenterSelf called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->CenterWindow(window->GetSize());
    } else {
        shared::PlatformCenterWindow(browser, shared::PlatformGetWindowSize(browser));
    }
}

void HandleWindowSetProxyRequests(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetProxyRequests(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> setProxyRequests = reader.read<bool>();
    if (!identifier || !setProxyRequests)
    {
        LOG(ERROR) << "HandleWindowSetProxyRequests called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetProxyRequests called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    pClient->settings.proxyRequests = *setProxyRequests;
}

void HandleWindowGetPosition(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> getPositionPromise;
        std::future<void> getPositionFuture = getPositionPromise.get_future();
        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> getPositionPromise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowGetPosition(reader, writer);
            getPositionPromise.set_value();
        }, std::move(getPositionPromise), std::ref(reader), std::ref(writer)));
        return getPositionFuture.get();
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetPosition called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetPosition called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefPoint position;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        position = window->GetPosition();
    } else {
        position = shared::PlatformGetWindowPosition(browser);
    }

    writer.write<int32_t>(position.x);
    writer.write<int32_t>(position.y);
}

void HandleWindowSetPosition(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetPosition(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<int32_t> x = reader.read<int32_t>();
    std::optional<int32_t> y = reader.read<int32_t>();
    if (!identifier || !x || !y)
    {
        LOG(ERROR) << "HandleWindowSetPosition called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetPosition called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefPoint position;
    position.x = *x;
    position.y = *y;

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetPosition(position);
    } else {
        shared::PlatformSetWindowPosition(browser, position);
    }
}

void HandleWindowSetDevelopmentToolsEnabled(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetDevelopmentToolsEnabled(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> developerToolsEnabled = reader.read<bool>();
    if (!identifier || !developerToolsEnabled)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsEnabled called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsEnabled called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    pClient->settings.developerToolsEnabled = *developerToolsEnabled;

    if (!pClient->settings.developerToolsEnabled && browser->GetHost()->HasDevTools())
        browser->GetHost()->CloseDevTools();
}

void HandleWindowSetDevelopmentToolsVisible(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetDevelopmentToolsVisible(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> developerToolsVisible = reader.read<bool>();
    if (!identifier || !developerToolsVisible)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsVisible called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsVisible called while CefBrowser is already closed. Ignored.";
        return;
    }

    if (!*developerToolsVisible && browser->GetHost()->HasDevTools())
    {
        browser->GetHost()->CloseDevTools();
    }
    else if (*developerToolsVisible && !browser->GetHost()->HasDevTools())
    {
        CefBrowserSettings browser_settings;
        CefWindowInfo window_info;
        CefPoint inspect_element_at;
        browser->GetHost()->ShowDevTools(window_info, new DevToolsClient(), browser_settings, inspect_element_at);
    }
}

void HandleWindowClose(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowClose(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> forceClosed = reader.read<bool>();
    if (!identifier || !forceClosed)
    {
        LOG(ERROR) << "HandleWindowClose called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowClose called while CefBrowser is already closed. Ignored.";
        return;
    }
    browser->GetHost()->CloseBrowser(*forceClosed);
}
void HandleWindowLoadUrl(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowLoadUrl(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleWindowLoadUrl called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowLoadUrl called while CefBrowser is already closed. Ignored.";
        return;
    }
    
    browser->GetMainFrame()->LoadURL(*url);
}

void HandleWindowSetZoom(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetZoom(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<double> zoom = reader.read<double>();
    if (!identifier || !zoom)
    {
        LOG(ERROR) << "HandleWindowSetZoom called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetZoom called while CefBrowser is already closed. Ignored.";
        return;
    }
    
    browser->GetHost()->SetZoomLevel(*zoom);
}

void HandleWindowGetZoom(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowGetZoom(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetZoom called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetZoom called while CefBrowser is already closed. Ignored.";
        return;
    }
    
    writer.write<double>(browser->GetHost()->GetZoomLevel());
}

void HandleWindowRequestFocus(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI))
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowRequestFocus(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowRequestFocus called without CefBrowser. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowRequestFocus called while CefBrowser is already closed. Ignored.";
        return;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->RequestFocus();
    } else {
        shared::PlatformWindowRequestFocus(browser);
    }
}

void HandleWindowSetModifyRequests(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetModifyRequests(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<uint8_t> flags = reader.read<uint8_t>();
    if (!identifier || !flags)
    {
        LOG(ERROR) << "HandleWindowSetModifyRequests called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetModifyRequests called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    pClient->settings.modifyRequests = (*flags & 1) ? true : false;
    pClient->settings.modifyRequestBody = (*flags & 2) ? true : false;
}

void HandleWindowOpenDirectoryPicker(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowOpenDirectoryPicker called without valid data. Ignored.";
        return;
    }

    std::string path = shared::PlatformPickDirectory(*identifier).get();
    writer.writeSizePrefixedString(path);
}

void HandleWindowOpenFilePicker(const std::vector<std::string>& paths, PacketWriter& writer)
{
    writer.write<uint32_t>((uint32_t)paths.size());

    for (uint32_t i = 0; i < paths.size(); i++)
        writer.writeSizePrefixedString(paths[i]);
}

void HandleWindowOpenFilePicker(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> multiple = reader.read<bool>();
    std::optional<uint32_t> filterCount = reader.read<uint32_t>();
    std::vector<std::pair<std::string, std::string>> filters;
    if (!identifier || !multiple || !filterCount)
    {
        LOG(ERROR) << "HandleWindowOpenFilePicker called without valid data. Ignored.";
        return;
    }

    filters.reserve(*filterCount);
    for (uint32_t i = 0; i < filterCount; i++)
    {
        std::optional<std::string> name = reader.readSizePrefixedString();
        std::optional<std::string> pattern = reader.readSizePrefixedString();
        if (name == std::nullopt || pattern == std::nullopt)
        {
            LOG(ERROR) << "HandleWindowOpenFilePicker called without valid data (filter invalid). Ignored.";
            return;
        }

        filters.push_back(std::pair<std::string, std::string>(name.value(), pattern.value()));
    }

    std::vector<std::string> paths = shared::PlatformPickFiles(*identifier, *multiple, filters).get();
    return HandleWindowOpenFilePicker(paths, writer);
}

void HandleWindowSaveFilePicker(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> defaultName = reader.readSizePrefixedString();
    std::optional<uint32_t> filterCount = reader.read<uint32_t>();
    if (!identifier || !defaultName || !filterCount) 
    {
        LOG(ERROR) << "HandleWindowSaveFilePicker called without valid data. Ignored.";
        return;
    }

    std::vector<std::pair<std::string, std::string>> filters;
    filters.reserve(*filterCount);
    for (uint32_t i = 0; i < filterCount; i++)
    {
        std::optional<std::string> name = reader.readSizePrefixedString();
        std::optional<std::string> pattern = reader.readSizePrefixedString();
        if (!name || !pattern)
        {
            LOG(ERROR) << "HandleWindowSaveFilePicker called without valid data (filter invalid). Ignored.";
            return;
        }

        filters.push_back(std::pair<std::string, std::string>(name.value(), pattern.value()));
    }

    std::string path = shared::PlatformSaveFile(*identifier, *defaultName, filters).get();
    writer.writeSizePrefixedString(path);
}

void CloseEverything()
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&CloseEverything));
        return;
    }

    IPC::Singleton.Stop();
    if (ClientManager::GetInstance()->GetBrowserCount() > 0) {
        ClientManager::GetInstance()->CloseAllBrowsers(true);
    } else {
        CefQuitMessageLoop();
    }
}

std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> HandleWindowExecuteDevToolsMethodInternal(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<std::optional<std::future<std::optional<IPCDevToolsMethodResult>>>> executeDevToolsMethodPromise;
        std::future<std::optional<std::future<std::optional<IPCDevToolsMethodResult>>>> executeDevToolsMethodFuture = executeDevToolsMethodPromise.get_future();
        CefPostTask(TID_UI, base::BindOnce([](std::promise<std::optional<std::future<std::optional<IPCDevToolsMethodResult>>>> executeDevToolsMethodPromise, PacketReader& reader, PacketWriter& writer) {
            executeDevToolsMethodPromise.set_value(HandleWindowExecuteDevToolsMethodInternal(reader, writer));
        }, std::move(executeDevToolsMethodPromise), std::ref(reader), std::ref(writer)));
        return executeDevToolsMethodFuture.get();
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    std::optional<std::string> json = reader.readSizePrefixedString();
    if (!identifier || !method)
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod called without valid data. Ignored.";
        return std::nullopt;
    }

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod called while CefBrowser is already closed. Ignored.";
        return std::nullopt;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod client is null. Ignored.";
        return std::nullopt;
    }

    return json 
        ? pClient->ExecuteDevToolsMethod(browser, *method, *json)
        : pClient->ExecuteDevToolsMethod(browser, *method);
}

void HandleWindowExecuteDevToolsMethod(PacketReader& reader, PacketWriter& writer)
{
    //TODO: Make promise instead of blocking?
    std::optional<std::future<std::optional<IPCDevToolsMethodResult>>> result = HandleWindowExecuteDevToolsMethodInternal(reader, writer);
    if (result) 
    {
        std::optional<IPCDevToolsMethodResult> r = (*result).get();
        if (r)
        {
            writer.write(r->success);
            writer.write<uint32_t>((uint32_t)r->result->size());
            if (r->result->size() > 0)
                writer.writeBytes(r->result->data(), r->result->size());
        }
        else
        {
            writer.write(false);
            writer.write<uint32_t>(0);
        }
    }
    else
    {
        writer.write(false);
        writer.write<uint32_t>(0);
    }
}

void HandleWindowSetTitle(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetTitle(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> title = reader.readSizePrefixedString();
    if (!identifier || !title)
    {
        LOG(ERROR) << "HandleWindowSetTitle called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetTitle called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleWindowSetTitle client is null. Ignored.";
        return;
    }

    pClient->OverrideTitle(browser, *title);
}

void HandleWindowSetIcon(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetIcon(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> iconPath = reader.readSizePrefixedString();
    if (!identifier || !iconPath)
    {
        LOG(ERROR) << "HandleWindowSetIcon called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetIcon called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleWindowSetIcon client is null. Ignored.";
        return;
    }

    pClient->OverrideIcon(browser, *iconPath);
}

void HandleAddUrlToProxy(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleAddUrlToProxy(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleAddUrlToProxy called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddUrlToProxy called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleAddUrlToProxy client is null. Ignored.";
        return;
    }

    pClient->AddUrlToProxy(*url);
    LOG(INFO) << "Added URL to proxy: " + *url;
}

void HandleRemoveUrlToProxy(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleRemoveUrlToProxy(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy client is null. Ignored.";
        return;
    }

    pClient->RemoveUrlToProxy(*url);
    LOG(INFO) << "Removed URL to proxy: " + *url;
}

void HandleAddDomainToProxy(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleAddDomainToProxy(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> domain = reader.readSizePrefixedString();
    if (!identifier || !domain)
    {
        LOG(ERROR) << "HandleAddDomainToProxy called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddDomainToProxy called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleAddDomainToProxy client is null. Ignored.";
        return;
    }

    pClient->AddDomainToProxy(*domain);
    LOG(INFO) << "Added domain to proxy: " + *domain;
}

void HandleRemoveDomainToProxy(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleRemoveDomainToProxy(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy client is null. Ignored.";
        return;
    }

    pClient->RemoveDomainToProxy(*url);
    LOG(INFO) << "Removed domain to proxy: " + *url;
}

void HandleAddUrlToModify(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleAddUrlToModify(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleAddUrlToModify called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddUrlToModify called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleAddUrlToModify client is null. Ignored.";
        return;
    }

    pClient->AddUrlToModify(*url);
    LOG(INFO) << "Added URL to modify: " + *url;
}

void HandleRemoveUrlToModify(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleRemoveUrlToModify(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveUrlToModify called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveUrlToModify called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleRemoveUrlToModify client is null. Ignored.";
        return;
    }

    pClient->RemoveUrlToModify(*url);
    LOG(INFO) << "Removed URL to modify: " + *url;
}

void HandleWindowGetSize(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> getSizePromise;
        std::future<void> getSizeFuture = getSizePromise.get_future();
        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> getSizePromise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowGetSize(reader, writer);
            getSizePromise.set_value();
        }, std::move(getSizePromise), std::ref(reader), std::ref(writer)));
        return getSizeFuture.get();
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetSize called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetSize called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefSize size;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        size = window->GetSize();
    } else {
        size = shared::PlatformGetWindowSize(browser);
    }

    writer.write<int32_t>(size.width);
    writer.write<int32_t>(size.height);
}

void HandleWindowSetSize(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleWindowSetSize(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<int32_t> width = reader.read<int32_t>();
    std::optional<int32_t> height = reader.read<int32_t>();
    if (!identifier || !width || !height)
    {
        LOG(ERROR) << "HandleWindowSetSize called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetSize called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefSize size(*width, *height);
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetSize(size);
    } else 
    {
        shared::PlatformSetWindowSize(browser, size);
    }
}

void HandleAddDevToolsEventMethod(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleAddDevToolsEventMethod(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    if (!identifier || !method)
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod client is null. Ignored.";
        return;
    }

    pClient->AddDevToolsEventMethod(browser, *method);
    LOG(INFO) << "Added DevTools event method: " + *method;
}

void HandleRemoveDevToolsEventMethod(PacketReader& reader, PacketWriter& writer)
{
    if (!CefCurrentlyOn(TID_UI)) 
    {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();

        CefPostTask(TID_UI, base::BindOnce([](std::promise<void> promise, PacketReader& reader, PacketWriter& writer) {
            HandleRemoveDevToolsEventMethod(reader, writer);
            promise.set_value();
        }, std::move(promise), std::ref(reader), std::ref(writer)));

        future.wait();
        return;
    }

    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    if (!identifier || !method)
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod called without valid data. Ignored.";
        return;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod called while CefBrowser is already closed. Ignored.";
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient) 
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod client is null. Ignored.";
        return;
    }

    pClient->RemoveDevToolsEventMethod(browser, *method);
    LOG(INFO) << "Removed DevTools event method: " + *method;
}
