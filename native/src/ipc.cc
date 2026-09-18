#include "ipc.h"
#include "bridge.h"
#include "client.h"
#include "client_manager.h"
#include "client_util.h"
#include "justcef_view_host.h"
#include "widevine_util.h"

#include "devtoolsclient.h"
#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_stream.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <future>
#include <include/cef_app.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>

template <class T> std::string optionalToString(std::optional<T>& opt)
{
    if (!opt)
    {
        return "std::nullopt";
    }

    return std::string(*opt);
}

namespace
{

constexpr auto kShutdownFlushDeadline = std::chrono::milliseconds(2000);
constexpr auto kViewCreatedTimeout = std::chrono::milliseconds(10000);

IPCBridgeRpcResult MakeBridgeRpcResult(bool success, const std::string& result_json, const std::string& error)
{
    IPCBridgeRpcResult result;
    result.success = success;
    result.result_json = result_json;
    result.error = error;
    return result;
}

bool WriteBytes(PacketWriter& writer, const uint8_t* payload, size_t size)
{
    return writer.write<uint32_t>(static_cast<uint32_t>(size)) && writer.writeBytes(payload, size);
}

std::optional<std::string> ReadBytesAsString(PacketReader& reader)
{
    std::optional<uint32_t> size = reader.read<uint32_t>();
    if (!size)
        return std::nullopt;
    return reader.readString(*size);
}

std::vector<uint8_t> ToVector(const PacketWriter& writer)
{
    return std::vector<uint8_t>(writer.data(), writer.data() + writer.size());
}

void RunLater(ipc::EventLoop& loop, std::function<void()> task)
{
    if (!loop.Post(task))
        task();
}

bool IsBrowserRequest(OpcodeController opcode)
{
    switch (opcode)
    {
    case OpcodeController::WindowSetDevelopmentToolsEnabled:
    case OpcodeController::WindowLoadUrl:
    case OpcodeController::WindowSetZoom:
    case OpcodeController::WindowGetPosition:
    case OpcodeController::WindowSetPosition:
    case OpcodeController::WindowMaximize:
    case OpcodeController::WindowMinimize:
    case OpcodeController::WindowRestore:
    case OpcodeController::WindowShow:
    case OpcodeController::WindowHide:
    case OpcodeController::WindowClose:
    case OpcodeController::WindowRequestFocus:
    case OpcodeController::WindowActivate:
    case OpcodeController::WindowBringToTop:
    case OpcodeController::WindowSetAlwaysOnTop:
    case OpcodeController::WindowSetFullscreen:
    case OpcodeController::WindowCenterSelf:
    case OpcodeController::WindowSetProxyRequests:
    case OpcodeController::WindowSetModifyRequests:
    case OpcodeController::PickFile:
    case OpcodeController::PickDirectory:
    case OpcodeController::SaveFile:
    case OpcodeController::WindowExecuteDevToolsMethod:
    case OpcodeController::WindowSetDevelopmentToolsVisible:
    case OpcodeController::WindowSetTitle:
    case OpcodeController::WindowSetIcon:
    case OpcodeController::WindowAddUrlToProxy:
    case OpcodeController::WindowRemoveUrlToProxy:
    case OpcodeController::WindowAddUrlToModify:
    case OpcodeController::WindowRemoveUrlToModify:
    case OpcodeController::WindowGetSize:
    case OpcodeController::WindowSetSize:
    case OpcodeController::WindowAddDevToolsEventMethod:
    case OpcodeController::WindowRemoveDevToolsEventMethod:
    case OpcodeController::WindowAddDomainToProxy:
    case OpcodeController::WindowRemoveDomainToProxy:
    case OpcodeController::WindowGetZoom:
    case OpcodeController::WindowBridgeRpc:
        return true;
    default:
        return false;
    }
}

void ApplyModifyResponse(CefRefPtr<CefRequest> request, bool modifyRequestBody, const std::vector<uint8_t>& response);

} // namespace

IPC IPC::Singleton;

IPC::IPC() = default;

IPC::~IPC()
{
    Stop();
}

#ifndef JUSTCEF_NATIVE_VERSION
#define JUSTCEF_NATIVE_VERSION 0
#endif

void IPC::Start()
{
    LOG(INFO) << "JustCef native runtime v" << JUSTCEF_NATIVE_VERSION;
    LOG(INFO) << "IPC start called.";

    _startCalled = true;
    if (!_stopped)
        return;

    LOG(INFO) << "Starting IPC.";

    CefRefPtr<CefCommandLine> commandLine = CefCommandLine::GetGlobalCommandLine();
    _debugEnabled = commandLine && commandLine->HasSwitch("enable-ipc-debug");

    _loop.Start();
    _endpoint = ipc::Endpoint::Create(_transport, _loop);
    std::weak_ptr<ipc::Endpoint> weakEndpoint = _endpoint;
    _streams = ipc::StreamReceiver::Create(
        [weakEndpoint](uint8_t opcode, const std::vector<uint8_t>& body)
        {
            auto endpoint = weakEndpoint.lock();
            return endpoint && endpoint->Notify(opcode, body);
        },
        static_cast<uint8_t>(OpcodeClientNotification::StreamCredit), static_cast<uint8_t>(OpcodeClientNotification::StreamCancel));
    _endpoint->SetRequestHandler(
        [this](ipc::IncomingRequest request, ipc::Reply reply)
        {
            OnRequest(std::move(request), std::move(reply));
        });
    _endpoint->SetNotificationHandler(
        [this](uint8_t opcode, std::vector<uint8_t> body)
        {
            PacketReader reader(body.data(), body.size());
            HandleNotification((OpcodeControllerNotification)opcode, reader);
        });

    _stopped = false;

    const bool started = _transport.Start(
        [this](ipc::Packet&& packet)
        {
            auto shared = std::make_shared<ipc::Packet>(std::move(packet));
            _loop.Post(
                [this, shared]()
                {
                    _endpoint->HandlePacket(std::move(*shared));
                });
        },
        [this]()
        {
            _loop.Post(
                [this]()
                {
                    OnTransportClosed();
                });
        });
    if (!started)
    {
        LOG(ERROR) << "Failed to start the IPC transport.";
        _stopped = true;
        return;
    }

    LOG(INFO) << "Started IPC.";
}

void IPC::Stop()
{
    if (!_startCalled)
        return;

    LOG(INFO) << "IPC stop called.";
    if (_stopCalled.exchange(true))
        return;

    LOG(INFO) << "Stopping IPC.";

    NotifyExit();
    _transport.Shutdown(kShutdownFlushDeadline);

    LOG(INFO) << "Stopped pipe.";

    _loop.Post(
        [this]()
        {
            _stopped = true;
            if (_endpoint)
                _endpoint->Close();
            if (_streams)
                _streams->FailAll("IPC stopped.");
        });
    _loop.Stop();

    LOG(INFO) << "Stopped IPC.";
}

bool IPC::HasValidHandles()
{
    return _transport.HasValidHandles();
}

bool IPC::IsAvailable()
{
    return HasValidHandles() && !_stopped && _startCalled;
}

void IPC::OnTransportClosed()
{
    if (_stopped.exchange(true))
        return;

    LOG(INFO) << "Pipe closed. Parent process likely wants child to exit.";
    if (_endpoint)
        _endpoint->Close();
    if (_streams)
        _streams->FailAll("IPC connection closed.");
    if (!_stopCalled)
        CloseEverything();
}

void IPC::OnRequest(ipc::IncomingRequest request, ipc::Reply reply)
{
    const OpcodeController opcode = (OpcodeController)request.opcode;
    if (opcode == OpcodeController::Ping || opcode == OpcodeController::Print || opcode == OpcodeController::Echo || opcode == OpcodeController::Debug)
    {
        RunRequest(std::move(request), std::move(reply));
        return;
    }

    CefPostTask(TID_UI, base::BindOnce(&IPC::RunRequest, base::Unretained(this), std::move(request), std::move(reply)));
}

void IPC::RunRequest(ipc::IncomingRequest request, ipc::Reply reply)
{
    const OpcodeController opcode = (OpcodeController)request.opcode;
    if (IsBrowserRequest(opcode))
    {
        PacketReader peek(request.body.data(), request.body.size());
        std::optional<int32_t> identifier = peek.read<int32_t>();
        if (identifier && !ClientManager::GetInstance()->AcquirePointer(*identifier))
        {
            reply.Fail(ipc::StatusCode::NotFound, "Browser " + std::to_string(*identifier) + " does not exist.");
            return;
        }
    }

    PacketReader reader(request.body.data(), request.body.size());
    PacketWriter writer;
    ipc::StatusCode status = ipc::StatusCode::Ok;
    if (!HandleRequest(reply, opcode, reader, writer, status))
        return;

    if (status == ipc::StatusCode::Ok)
        reply.Ok(writer.data(), writer.size());
    else if (status == ipc::StatusCode::NotFound)
        reply.Fail(status, "The browser does not exist.");
    else if (status == ipc::StatusCode::InvalidRequest)
        reply.Fail(status, "The request is malformed.");
    else
        reply.Fail(status, "The request failed.");
}

ipc::CallHandle IPC::CallAsync(OpcodeClient opcode, std::vector<uint8_t> body, std::chrono::milliseconds timeout, std::function<void(ipc::Response)> callback)
{
    if (!IsAvailable() || !_endpoint)
    {
        RunLater(_loop,
                 [callback]()
                 {
                     callback(ipc::Response{ipc::StatusCode::Disconnected, "IPC is not available.", {}});
                 });
        return ipc::CallHandle();
    }

    return _endpoint->Call(static_cast<uint8_t>(opcode), std::move(body), timeout, std::move(callback));
}

void IPC::Notify(OpcodeClientNotification opcode, const PacketWriter& writer)
{
    Notify(opcode, writer.data(), writer.size());
}

void IPC::Notify(OpcodeClientNotification opcode, const uint8_t* body, size_t size)
{
    if (!IsAvailable() || !_endpoint)
        return;

    std::lock_guard<std::mutex> lk(_holdMutex);
    if (_holdDepth > 0)
    {
        _held.emplace_back(opcode, std::vector<uint8_t>(body, body + size));
        return;
    }

    _endpoint->Notify(static_cast<uint8_t>(opcode), body, size);
}

void IPC::BeginAnnounceHold()
{
    std::lock_guard<std::mutex> lk(_holdMutex);
    ++_holdDepth;
}

void IPC::EndAnnounceHold()
{
    std::lock_guard<std::mutex> lk(_holdMutex);
    if (_holdDepth > 0 && --_holdDepth > 0)
        return;

    std::vector<std::pair<OpcodeClientNotification, std::vector<uint8_t>>> held;
    held.swap(_held);
    if (!_endpoint)
        return;
    for (auto& entry : held)
        _endpoint->Notify(static_cast<uint8_t>(entry.first), entry.second);
}

bool IPC::HandleRequest(ipc::Reply& reply, OpcodeController opcode, PacketReader& reader, PacketWriter& writer, ipc::StatusCode& status)
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
        reader.copyTo(
            [&writer](const uint8_t* data, size_t size)
            {
                return writer.writeBytes(data, size);
            },
            reader.remainingSize());
        return true;
    case OpcodeController::WindowCreate:
        HandleWindowCreate(reader, std::move(reply));
        return false;
    case OpcodeController::WindowMaximize:
        status = HandleWindowMaximize(reader, writer);
        return true;
    case OpcodeController::WindowMinimize:
        status = HandleWindowMinimize(reader, writer);
        return true;
    case OpcodeController::WindowRestore:
        status = HandleWindowRestore(reader, writer);
        return true;
    case OpcodeController::WindowShow:
        status = HandleWindowShow(reader, writer);
        return true;
    case OpcodeController::WindowHide:
        status = HandleWindowHide(reader, writer);
        return true;
    case OpcodeController::WindowActivate:
        status = HandleWindowActivate(reader, writer);
        return true;
    case OpcodeController::WindowBringToTop:
        status = HandleWindowBringToTop(reader, writer);
        return true;
    case OpcodeController::WindowSetAlwaysOnTop:
        status = HandleWindowSetAlwaysOnTop(reader, writer);
        return true;
    case OpcodeController::WindowSetFullscreen:
        status = HandleWindowSetFullscreen(reader, writer);
        return true;
    case OpcodeController::WindowCenterSelf:
        status = HandleWindowCenterSelf(reader, writer);
        return true;
    case OpcodeController::WindowSetProxyRequests:
        status = HandleWindowSetProxyRequests(reader, writer);
        return true;
    case OpcodeController::WindowSetPosition:
        status = HandleWindowSetPosition(reader, writer);
        return true;
    case OpcodeController::WindowGetPosition:
        status = HandleWindowGetPosition(reader, writer);
        return true;
    case OpcodeController::WindowSetDevelopmentToolsEnabled:
        status = HandleWindowSetDevelopmentToolsEnabled(reader, writer);
        return true;
    case OpcodeController::WindowSetDevelopmentToolsVisible:
        status = HandleWindowSetDevelopmentToolsVisible(reader, writer);
        return true;
    case OpcodeController::WindowClose:
        status = HandleWindowClose(reader, writer);
        return true;
    case OpcodeController::WindowLoadUrl:
        status = HandleWindowLoadUrl(reader, writer);
        return true;
    case OpcodeController::WindowSetZoom:
        status = HandleWindowSetZoom(reader, writer);
        return true;
    case OpcodeController::WindowGetZoom:
        status = HandleWindowGetZoom(reader, writer);
        return true;
    case OpcodeController::GetWidevineStatus:
        status = HandleGetWidevineStatus(reader, writer);
        return true;
    case OpcodeController::WindowRequestFocus:
        status = HandleWindowRequestFocus(reader, writer);
        return true;
    case OpcodeController::WindowSetModifyRequests:
        status = HandleWindowSetModifyRequests(reader, writer);
        return true;
    case OpcodeController::PickDirectory:
        HandleWindowOpenDirectoryPicker(reader, std::move(reply));
        return false;
    case OpcodeController::PickFile:
        HandleWindowOpenFilePicker(reader, std::move(reply));
        return false;
    case OpcodeController::SaveFile:
        HandleWindowSaveFilePicker(reader, std::move(reply));
        return false;
    case OpcodeController::WindowExecuteDevToolsMethod:
        HandleWindowExecuteDevToolsMethodRequest(std::move(reply), reader);
        return false;
    case OpcodeController::WindowSetTitle:
        status = HandleWindowSetTitle(reader, writer);
        return true;
    case OpcodeController::WindowSetIcon:
        status = HandleWindowSetIcon(reader, writer);
        return true;
    case OpcodeController::WindowAddUrlToProxy:
        status = HandleAddUrlToProxy(reader, writer);
        return true;
    case OpcodeController::WindowRemoveUrlToProxy:
        status = HandleRemoveUrlToProxy(reader, writer);
        return true;
    case OpcodeController::WindowAddDomainToProxy:
        status = HandleAddDomainToProxy(reader, writer);
        return true;
    case OpcodeController::WindowRemoveDomainToProxy:
        status = HandleRemoveDomainToProxy(reader, writer);
        return true;
    case OpcodeController::WindowAddUrlToModify:
        status = HandleAddUrlToModify(reader, writer);
        return true;
    case OpcodeController::WindowRemoveUrlToModify:
        status = HandleRemoveUrlToModify(reader, writer);
        return true;
    case OpcodeController::WindowGetSize:
        status = HandleWindowGetSize(reader, writer);
        return true;
    case OpcodeController::WindowSetSize:
        status = HandleWindowSetSize(reader, writer);
        return true;
    case OpcodeController::WindowAddDevToolsEventMethod:
        status = HandleAddDevToolsEventMethod(reader, writer);
        return true;
    case OpcodeController::WindowRemoveDevToolsEventMethod:
        status = HandleRemoveDevToolsEventMethod(reader, writer);
        return true;
    case OpcodeController::WindowBridgeRpc:
        HandleWindowBridgeRpcRequest(std::move(reply), reader);
        return false;
    case OpcodeController::Debug:
        HandleDebug(std::move(reply), reader);
        return false;
    default:
        LOG(ERROR) << "Unknown opcode " << (uint32_t)opcode << ".";
        reply.Fail(ipc::StatusCode::Unsupported, "Unknown opcode " + std::to_string((uint32_t)opcode) + ".");
        return false;
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
    case OpcodeControllerNotification::StreamData:
    {
        std::optional<uint32_t> identifier = reader.read<uint32_t>();
        if (identifier && _streams)
        {
            reader.copyTo(
                [this, identifier](const uint8_t* data, size_t size)
                {
                    _streams->OnData(*identifier, data, size);
                    return true;
                },
                reader.remainingSize());
        }
        break;
    }
    case OpcodeControllerNotification::StreamEnd:
    {
        std::optional<uint32_t> identifier = reader.read<uint32_t>();
        std::optional<uint64_t> totalBytes = reader.read<uint64_t>();
        if (identifier && totalBytes && _streams)
            _streams->OnEnd(*identifier, *totalBytes);
        break;
    }
    case OpcodeControllerNotification::StreamError:
    {
        std::optional<uint32_t> identifier = reader.read<uint32_t>();
        std::optional<std::string> message = reader.readSizePrefixedString();
        if (identifier && _streams)
            _streams->OnError(*identifier, message.value_or("The stream failed."));
        break;
    }
    default:
        LOG(ERROR) << "Unknown notification opcode " << (uint32_t)opcode << ".";
        break;
    }
}

bool IPC::SerializePostData(PacketWriter& writer, CefRefPtr<CefPostData> postData)
{
    if (!postData.get())
    {
        return writer.write<int32_t>(0);
    }

    size_t elementCount = postData->GetElementCount();
    if (!writer.write<int32_t>(static_cast<int32_t>(elementCount)))
    {
        return false;
    }

    if (elementCount == 0)
    {
        return true;
    }

    std::vector<CefRefPtr<CefPostDataElement>> elements;
    postData->GetElements(elements);
    for (auto& element : elements)
    {
        uint8_t elementType = static_cast<uint8_t>(element->GetType());
        if (elementType == CefPostDataElement::Type::PDE_TYPE_BYTES)
        {
            size_t dataSize = element->GetBytesCount();
            if (dataSize > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
            {
                return false;
            }

            std::vector<uint8_t> data(dataSize);
            element->GetBytes(dataSize, data.data());
            uint32_t dataSize32 = static_cast<uint32_t>(dataSize);
            if (!writer.write<uint8_t>(elementType) || !writer.write<uint32_t>(dataSize32) || !writer.writeBytes(data.data(), data.size()))
            {
                return false;
            }
        }
        else if (elementType == CefPostDataElement::Type::PDE_TYPE_FILE)
        {
            if (!writer.write<uint8_t>(elementType) || !writer.writeSizePrefixedString(element->GetFile()))
            {
                return false;
            }
        }
        else
        {
            LOG(ERROR) << "Unsupported post data element type " << static_cast<int>(elementType) << ".";
            return false;
        }
    }

    return true;
}

ipc::CallHandle IPC::Echo(std::vector<uint8_t> data, std::chrono::milliseconds timeout, std::function<void(ipc::Response)> callback)
{
    return CallAsync(OpcodeClient::Echo, std::move(data), timeout, std::move(callback));
}

ipc::CallHandle IPC::WindowProxyRequest(int32_t identifier, CefRefPtr<CefRequest> request, std::chrono::milliseconds timeout,
                                        std::function<void(ipc::StatusCode, std::unique_ptr<IPCProxyResponse>)> callback)
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
    if (!SerializePostData(writer, postData))
    {
        LOG(ERROR) << "Failed to serialize proxy request post data.";
        RunLater(_loop,
                 [callback]()
                 {
                     callback(ipc::StatusCode::TooLarge, nullptr);
                 });
        return ipc::CallHandle();
    }

    return CallAsync(OpcodeClient::WindowProxyRequest, ToVector(writer), timeout,
                     [this, callback](ipc::Response response)
                     {
                         if (response.status != ipc::StatusCode::Ok)
                         {
                             callback(response.status, nullptr);
                             return;
                         }

                         std::unique_ptr<IPCProxyResponse> result = ParseProxyResponse(response.payload);
                         const auto status = result ? ipc::StatusCode::Ok : ipc::StatusCode::InvalidRequest;
                         callback(status, std::move(result));
                     });
}

std::unique_ptr<IPCProxyResponse> IPC::ParseProxyResponse(const std::vector<uint8_t>& response)
{
    std::unique_ptr<IPCProxyResponse> result = nullptr;

    if (!response.empty())
    {
        PacketReader reader(response.data(), response.size());

        // Deserialize method
        std::optional<uint32_t> statusCode = reader.read<uint32_t>();
        if (!statusCode)
        {
            LOG(ERROR) << "Failed to read status code.";
            return nullptr;
        }

        std::optional<std::string> statusText = reader.readSizePrefixedString();
        if (!statusText)
        {
            LOG(ERROR) << "Failed to read status text.";
            return nullptr;
        }

        // Deserialize headers
        std::optional<uint32_t> responseHeaderCount = reader.read<uint32_t>();
        if (!responseHeaderCount)
        {
            LOG(ERROR) << "Failed to read response header count.";
            return nullptr;
        }

        std::optional<std::string> mediaType = std::nullopt;
        std::multimap<std::string, std::string> responseHeaders;
        for (uint32_t i = 0; i < *responseHeaderCount; ++i)
        {
            std::optional<std::string> key = reader.readSizePrefixedString();
            if (!key)
            {
                LOG(ERROR) << "Failed to read response header key text.";
                return nullptr;
            }

            std::optional<std::string> value = reader.readSizePrefixedString();
            if (!value)
            {
                LOG(ERROR) << "Failed to read response header value text.";
                return nullptr;
            }

            if (key && value && (*key).c_str() && (*value).c_str() &&
#ifdef _WIN32
                stricmp((*key).c_str(), "content-type") == 0
#else
                strcasecmp((*key).c_str(), "content-type") == 0
#endif
            )
            {
                size_t semicolonPos = (*value).find(';');
                mediaType = semicolonPos != std::string::npos ? (*value).substr(0, semicolonPos) : *value;
            }

            responseHeaders.insert({*key, *value});
        }

        // Deserialize elements
        std::optional<uint8_t> bodyType = reader.read<uint8_t>();
        if (!bodyType)
        {
            LOG(ERROR) << "Failed to read body type.";
            return nullptr;
        }

        std::optional<std::vector<uint8_t>> body = std::nullopt;
        std::shared_ptr<ipc::IncomingStream> bodyStream = nullptr;
        int64_t streamBodyLength = -1;
        if (*bodyType == 1)
        {
            std::optional<uint32_t> bodySize = reader.read<uint32_t>();
            if (!bodySize)
            {
                LOG(ERROR) << "Failed to read body size.";
                return nullptr;
            }

            if (*bodySize > 0)
            {
                if (!reader.hasAvailable(*bodySize))
                {
                    LOG(ERROR) << "Proxy missing body (bodySize = " << *bodySize << ", remainingSize = " << reader.remainingSize() << ")";
                    return nullptr;
                }

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
            std::optional<int64_t> bodyLength = reader.read<int64_t>();
            std::optional<uint32_t> streamId = reader.read<uint32_t>();
            if (!bodyLength || !streamId || *streamId == 0 || !_streams)
            {
                LOG(ERROR) << "Failed to read stream body length / id.";
                return nullptr;
            }

            streamBodyLength = *bodyLength;
            bodyStream = _streams->Open(*streamId, *bodyLength);
        }

        result = std::unique_ptr<IPCProxyResponse>(new IPCProxyResponse());
        result->status_code = (int32_t)*statusCode;
        result->status_text = *statusText;
        result->headers = responseHeaders;
        result->media_type = mediaType;
        result->body = body;
        result->bodyStream = bodyStream;
        result->bodyLength = streamBodyLength;

        return result;
    }

    return nullptr;
}

ipc::CallHandle IPC::WindowModifyRequest(int32_t identifier, CefRefPtr<CefRequest> request, bool modifyRequestBody, std::chrono::milliseconds timeout,
                                         std::function<void(ipc::StatusCode)> callback)
{
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
        if (modifyRequestBody)
        {
            if (!SerializePostData(writer, postData))
            {
                LOG(ERROR) << "Failed to serialize modify request post data.";
                CefPostTask(TID_IO, base::BindOnce(
                                        [](std::function<void(ipc::StatusCode)> callback)
                                        {
                                            callback(ipc::StatusCode::TooLarge);
                                        },
                                        callback));
                return ipc::CallHandle();
            }
        }
        else if (!writer.write<int32_t>(0))
        {
            LOG(ERROR) << "Failed to serialize empty modify request body.";
            CefPostTask(TID_IO, base::BindOnce(
                                    [](std::function<void(ipc::StatusCode)> callback)
                                    {
                                        callback(ipc::StatusCode::TooLarge);
                                    },
                                    callback));
            return ipc::CallHandle();
        }

        return CallAsync(OpcodeClient::WindowModifyRequest, ToVector(writer), timeout,
                         [request, modifyRequestBody, callback](ipc::Response response)
                         {
                             CefPostTask(TID_IO, base::BindOnce(
                                                     [](CefRefPtr<CefRequest> request, bool modifyRequestBody, ipc::Response response, std::function<void(ipc::StatusCode)> callback)
                                                     {
                                                         if (response.status == ipc::StatusCode::Ok && !request->IsReadOnly())
                                                             ApplyModifyResponse(request, modifyRequestBody, response.payload);
                                                         callback(response.status);
                                                     },
                                                     request, modifyRequestBody, std::move(response), callback));
                         });
    }
}

namespace
{

void ApplyModifyResponse(CefRefPtr<CefRequest> request, bool modifyRequestBody, const std::vector<uint8_t>& response)
{
    if (!response.empty())
    {
        PacketReader reader(response.data(), response.size());

        std::optional<std::string> method = reader.readSizePrefixedString();
        if (!method)
        {
            LOG(ERROR) << "Failed to read method.";
            return;
        }

        std::optional<std::string> url = reader.readSizePrefixedString();
        if (!url)
        {
            LOG(ERROR) << "Failed to read url.";
            return;
        }

        // Deserialize headers
        std::optional<uint32_t> headerCount = reader.read<uint32_t>();
        if (!headerCount)
        {
            LOG(ERROR) << "Failed to read header count.";
            return;
        }

        CefRequest::HeaderMap headers;
        for (uint32_t i = 0; i < *headerCount; ++i)
        {
            std::optional<std::string> key = reader.readSizePrefixedString();
            if (!key)
            {
                LOG(ERROR) << "Failed to read key.";
                return;
            }
            std::optional<std::string> value = reader.readSizePrefixedString();
            if (!value)
            {
                LOG(ERROR) << "Failed to read value.";
                return;
            }

            headers.insert(std::make_pair(*key, *value));
        }

        // Deserialize elements
        std::optional<uint32_t> elementCount = reader.read<uint32_t>();
        if (!elementCount)
        {
            LOG(ERROR) << "Failed to read element count.";
            return;
        }

        if (modifyRequestBody)
        {
            CefRefPtr<CefPostData> postData = CefPostData::Create();
            for (uint32_t i = 0; i < *elementCount; ++i)
            {
                std::optional<uint8_t> elementType = reader.read<uint8_t>();
                if (!elementType)
                {
                    LOG(ERROR) << "Failed to read element type.";
                    return;
                }

                if (*elementType == CefPostDataElement::Type::PDE_TYPE_BYTES)
                {
                    std::optional<uint32_t> dataSize = reader.read<uint32_t>();
                    if (!dataSize)
                    {
                        LOG(ERROR) << "Failed to read data size.";
                        return;
                    }
                    if (!reader.hasAvailable(*dataSize))
                    {
                        LOG(ERROR) << "Not enough data available to read body.";
                        return;
                    }

                    CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
                    reader.copyTo(
                        [element](const uint8_t* data, size_t size)
                        {
                            element->SetToBytes(size, data);
                            return true;
                        },
                        *dataSize);

                    postData->AddElement(element);
                }
                else if (*elementType == CefPostDataElement::Type::PDE_TYPE_FILE)
                {
                    std::optional<std::string> fileName = reader.readSizePrefixedString();
                    if (!fileName)
                    {
                        LOG(ERROR) << "Failed to read file name.";
                        return;
                    }
                    CefRefPtr<CefPostDataElement> element = CefPostDataElement::Create();
                    element->SetToFile(*fileName);
                    postData->AddElement(element);
                }
                else
                {
                    LOG(ERROR) << "Unsupported body element type " << static_cast<int>(*elementType) << ".";
                    return;
                }
            }

            request->SetPostData(postData);
        }

        // LOG(INFO) << "Request modifier:\n  Method: " << *method << "\nURL: " << *url << "\nHeaders: ";
        // for (const auto& header : headers) {
        //     LOG(INFO) << "  " << header.first << ": " << header.second;
        // }

        request->SetMethod(*method);
        request->SetURL(*url);
        request->SetHeaderMap(headers);
    }
}

} // namespace

ipc::CallHandle IPC::WindowBridgeRpc(int32_t identifier, const std::string& method, const std::string& payload_json, std::function<void(IPCBridgeRpcResult)> callback)
{
    PacketWriter writer;
    writer.write<int32_t>(identifier);
    writer.writeSizePrefixedString(method);
    if (!WriteBytes(writer, reinterpret_cast<const uint8_t*>(payload_json.data()), payload_json.size()))
    {
        RunLater(_loop,
                 [callback]()
                 {
                     callback(MakeBridgeRpcResult(false, "null", "Failed to serialize the bridge RPC payload."));
                 });
        return ipc::CallHandle();
    }

    return CallAsync(OpcodeClient::WindowBridgeRpc, ToVector(writer), std::chrono::milliseconds(0),
                     [callback](ipc::Response response)
                     {
                         if (response.status != ipc::StatusCode::Ok)
                         {
                             callback(MakeBridgeRpcResult(false, "null", response.message.empty() ? ipc::StatusName(response.status) : response.message));
                             return;
                         }

                         PacketReader reader(response.payload.data(), response.payload.size());
                         std::optional<std::string> payload = ReadBytesAsString(reader);
                         if (!payload)
                         {
                             callback(MakeBridgeRpcResult(false, "null", "Failed to parse the bridge RPC response payload."));
                             return;
                         }

                         callback(MakeBridgeRpcResult(true, *payload, ""));
                     });
}

void IPC::WindowViewCreated(int32_t parentIdentifier, int32_t viewIdentifier, const std::string& src, std::function<void(bool allow)> callback)
{
    PacketWriter writer;
    writer.write<int32_t>(parentIdentifier);
    writer.write<int32_t>(viewIdentifier);
    writer.writeSizePrefixedString(src);

    std::vector<uint8_t> body(writer.data(), writer.data() + writer.size());
    CallAsync(OpcodeClient::WindowViewCreated, std::move(body), kViewCreatedTimeout,
              [callback = std::move(callback)](ipc::Response response)
              {
                  bool allow = false;
                  if (response.status == ipc::StatusCode::Ok && !response.payload.empty())
                  {
                      PacketReader reader(response.payload.data(), response.payload.size());
                      std::optional<bool> value = reader.read<bool>();
                      allow = value.value_or(false);
                  }

                  callback(allow);
              });
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

void IPC::NotifyWindowFrameLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(frame ? frame->GetIdentifier() : "");
    writer.write(frame ? frame->IsMain() : false);
    writer.writeSizePrefixedString(frame ? frame->GetURL() : "");
    Notify(OpcodeClientNotification::WindowFrameLoadStart, writer);
}

void IPC::NotifyWindowFrameLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(frame ? frame->GetIdentifier() : "");
    writer.write(frame ? frame->IsMain() : false);
    writer.writeSizePrefixedString(frame ? frame->GetURL() : "");
    writer.write<int32_t>(httpStatusCode);
    Notify(OpcodeClientNotification::WindowFrameLoadEnd, writer);
}

void IPC::NotifyWindowFrameLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, cef_errorcode_t errorCode, const CefString& errorText, const CefString& url)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(frame ? frame->GetIdentifier() : "");
    writer.write(frame ? frame->IsMain() : false);
    writer.write((int32_t)errorCode);
    writer.writeSizePrefixedString(errorText);
    writer.writeSizePrefixedString(url);
    Notify(OpcodeClientNotification::WindowFrameLoadError, writer);
}

void IPC::NotifyWindowLoadingStateChanged(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.write(isLoading);
    writer.write(canGoBack);
    writer.write(canGoForward);
    Notify(OpcodeClientNotification::WindowLoadingStateChanged, writer);
}

void IPC::NotifyWindowDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const uint8_t* result, size_t result_size)
{
    PacketWriter writer;
    writer.write(browser->GetIdentifier());
    writer.writeSizePrefixedString(method);

    if (!WriteBytes(writer, result, result_size))
    {
        LOG(ERROR) << "Failed to serialize DevTools event payload.";
        return;
    }

    Notify(OpcodeClientNotification::WindowDevToolsEvent, writer);
}

void IPC::NotifyDebug(uint32_t sequence)
{
    Notify(OpcodeClientNotification::Debug, reinterpret_cast<const uint8_t*>(&sequence), sizeof(sequence));
}

#ifdef _WIN32
void IPC::SetHandles(HANDLE readHandle, HANDLE writeHandle)
{
    _transport.SetHandles(readHandle, writeHandle);
}
#else
void IPC::SetHandles(int readFd, int writeFd)
{
    _transport.SetHandles(readFd, writeFd);
}
#endif

class WindowDelegate : public CefWindowDelegate
{
public:
    explicit WindowDelegate(CefRefPtr<CefBrowserView> browser_view, cef_runtime_style_t runtime_style, cef_show_state_t initial_show_state, const IPCWindowCreate& settings)
        : browser_view_(browser_view), _settings(settings), runtime_style_(runtime_style), initial_show_state_(initial_show_state)
    {
    }

    void OnWindowCreated(CefRefPtr<CefWindow> window) override
    {
        window->AddChildView(browser_view_);
        if (initial_show_state_ != CEF_SHOW_STATE_HIDDEN)
        {
            window->Show();
        }
    }

    void OnWindowClosing(CefRefPtr<CefWindow> window) override { justcef_view::OnHostWindowClosing(window); }

    void OnWindowDestroyed(CefRefPtr<CefWindow> window) override
    {
        justcef_view::OnHostWindowClosing(window);
        browser_view_ = nullptr;
    }

    void OnLayoutChanged(CefRefPtr<CefView> view, const CefRect& new_bounds) override
    {
        CefRefPtr<CefPanel> panel = view ? view->AsPanel() : nullptr;
        CefRefPtr<CefWindow> window = panel ? panel->AsWindow() : nullptr;
        if (window)
            justcef_view::OnHostWindowLayoutChanged(window);
    }

    bool CanClose(CefRefPtr<CefWindow> window) override
    {
        CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
        if (browser)
        {
            return browser->GetHost()->TryCloseBrowser();
        }
        return true;
    }

    cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow> window) override { return initial_show_state_; }

    cef_runtime_style_t GetWindowRuntimeStyle() override { return runtime_style_; }

#if defined(OS_LINUX)
    bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window, CefLinuxWindowProperties& properties) override
    {
        CefString(&properties.wayland_app_id) = CefString(&properties.wm_class_class) = CefString(&properties.wm_class_name) = CefString(&properties.wm_role_name) =
            _settings.appId ? *_settings.appId : "cef";
        return true;
    }
#endif

    bool IsFrameless(CefRefPtr<CefWindow> window) override { return _settings.frameless == 1; }
    bool CanResize(CefRefPtr<CefWindow> window) override { return _settings.resizable == 1; }

    CefSize GetPreferredSize(CefRefPtr<CefView> view) override { return CefSize(_settings.preferredWidth, _settings.preferredHeight); }

    /*CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
        return CefSize(800, 600);
    }*/

    /*CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
        return CefSize(_settings.minimumWidth, _settings.minimumHeight);
    }*/

private:
    CefRefPtr<CefBrowserView> browser_view_;
    const IPCWindowCreate _settings;
    const cef_runtime_style_t runtime_style_;
    const cef_show_state_t initial_show_state_;

    IMPLEMENT_REFCOUNTING(WindowDelegate);
    DISALLOW_COPY_AND_ASSIGN(WindowDelegate);
};

class DevToolsWindowDelegate : public CefWindowDelegate
{
public:
    explicit DevToolsWindowDelegate(CefRefPtr<CefBrowserView> browser_view) : browser_view_(browser_view) {}

    void OnWindowCreated(CefRefPtr<CefWindow> window) override { window->AddChildView(browser_view_); }

    void OnWindowDestroyed(CefRefPtr<CefWindow> window) override { browser_view_ = nullptr; }

    bool CanClose(CefRefPtr<CefWindow> window) override
    {
        CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
        if (browser)
        {
            return browser->GetHost()->TryCloseBrowser();
        }
        return true;
    }

private:
    CefRefPtr<CefBrowserView> browser_view_;

    IMPLEMENT_REFCOUNTING(DevToolsWindowDelegate);
    DISALLOW_COPY_AND_ASSIGN(DevToolsWindowDelegate);
};

class BrowserViewDelegate : public CefBrowserViewDelegate
{
public:
    explicit BrowserViewDelegate(const IPCWindowCreate& settings, cef_runtime_style_t runtime_style) : _settings(settings), runtime_style_(runtime_style) {}

    bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools) override
    {
        if (is_devtools)
        {
            CefWindow::CreateTopLevelWindow(new DevToolsWindowDelegate(popup_browser_view));
        }
        else
        {
            cef_show_state_t showState = _settings.shown ? (_settings.fullscreen ? CEF_SHOW_STATE_FULLSCREEN : CEF_SHOW_STATE_NORMAL) : CEF_SHOW_STATE_HIDDEN;
            CefWindow::CreateTopLevelWindow(new WindowDelegate(popup_browser_view, runtime_style_, showState, _settings));
        }
        return true;
    }

    cef_runtime_style_t GetBrowserRuntimeStyle() override { return runtime_style_; }

private:
    const IPCWindowCreate _settings;
    const cef_runtime_style_t runtime_style_;

    IMPLEMENT_REFCOUNTING(BrowserViewDelegate);
    DISALLOW_COPY_AND_ASSIGN(BrowserViewDelegate);
};

cef_runtime_style_t GetConfiguredRuntimeStyle()
{
    CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();

    // Check if Alloy style will be used.
    cef_runtime_style_t runtime_style = CEF_RUNTIME_STYLE_DEFAULT;
    bool use_alloy_style = command_line->HasSwitch("use-alloy-style");
    if (use_alloy_style)
    {
        runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    }
    bool use_chrome_style = command_line->HasSwitch("use-chrome-style");
    if (use_chrome_style)
    {
        runtime_style = CEF_RUNTIME_STYLE_CHROME;
    }

    return runtime_style;
}

void CreateTopLevelPopupWindow(CefRefPtr<CefBrowserView> popup_browser_view, bool is_devtools, const IPCWindowCreate& settings)
{
    if (is_devtools)
    {
        CefWindow::CreateTopLevelWindow(new DevToolsWindowDelegate(popup_browser_view));
        return;
    }

    cef_show_state_t showState = settings.shown ? (settings.fullscreen ? CEF_SHOW_STATE_FULLSCREEN : CEF_SHOW_STATE_NORMAL) : CEF_SHOW_STATE_HIDDEN;
    CefWindow::CreateTopLevelWindow(new WindowDelegate(popup_browser_view, GetConfiguredRuntimeStyle(), showState, settings));
}

CefRefPtr<Client> CreateBrowserWindow(const IPCWindowCreate& windowCreate)
{
    CEF_REQUIRE_UI_THREAD();

    LOG(INFO) << "Window create (URL = '" << windowCreate.url << "')";

    CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
    const bool headless = command_line->HasSwitch("headless");
    const cef_runtime_style_t runtime_style = GetConfiguredRuntimeStyle();

    CefRefPtr<Client> client = new Client(windowCreate);
    CefBrowserSettings settings;
    CefRefPtr<CefDictionaryValue> extra_info = CreateBridgeExtraInfo(windowCreate.bridgeEnabled, nullptr, windowCreate.viewsEnabled);

    if (headless)
    {
        CefWindowInfo wi;
        wi.SetAsWindowless(kNullWindowHandle);
        wi.bounds.width = windowCreate.preferredWidth;
        wi.bounds.height = windowCreate.preferredHeight;
        CefBrowserHost::CreateBrowserSync(wi, client, windowCreate.url, settings, extra_info, nullptr);

        return client;
    }

    const bool use_views = !command_line->HasSwitch("use-native");
    LOG(INFO) << "Use views = " << (use_views ? "true" : "false");

    cef_show_state_t showState = windowCreate.shown ? (windowCreate.fullscreen ? CEF_SHOW_STATE_FULLSCREEN : CEF_SHOW_STATE_NORMAL) : CEF_SHOW_STATE_HIDDEN;

    if (use_views)
    {
        CefRefPtr<CefBrowserView> browser_view =
            CefBrowserView::CreateBrowserView(client, windowCreate.url, settings, extra_info, nullptr, new BrowserViewDelegate(windowCreate, runtime_style));
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
        if (windowCreate.shown)
        {
            style |= WS_VISIBLE;
        }
        window_info.style = style;
        window_info.parent_window = nullptr;
        window_info.bounds.x = CW_USEDEFAULT;
        window_info.bounds.y = CW_USEDEFAULT;

        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore)
        {
            typedef HRESULT(WINAPI * GetDpiForMonitorPtr)(HMONITOR, int, UINT*, UINT*);
            GetDpiForMonitorPtr GetDpiForMonitor = reinterpret_cast<GetDpiForMonitorPtr>(GetProcAddress(shcore, "GetDpiForMonitor"));
            if (GetDpiForMonitor)
            {
                POINT placementPoint = {(window_info.bounds.x == CW_USEDEFAULT) ? 0 : window_info.bounds.x, (window_info.bounds.y == CW_USEDEFAULT) ? 0 : window_info.bounds.y};

                HMONITOR monitor = MonitorFromPoint(placementPoint, MONITOR_DEFAULTTONEAREST);

                UINT dpiX = 96, dpiY = 96; // Default DPI (96 DPI = 100% scaling)
                if (SUCCEEDED(GetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)))
                {
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

CefRefPtr<Client> HandleWindowCreateInternal(PacketReader& reader, std::string& initialUrl)
{
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
    std::optional<bool> viewsEnabled = reader.read<bool>();
    std::optional<uint32_t> modifyTimeoutMs = reader.read<uint32_t>();
    std::optional<uint8_t> modifyTimeoutPolicy = reader.read<uint8_t>();
    std::optional<uint32_t> proxyOpenTimeoutMs = reader.read<uint32_t>();
    if (!resizable || !frameless || !fullscreen || !centered || !shown || !contextMenuEnable || !developerToolsEnabled || !modifyRequests || !modifyRequestBody || !proxyRequests ||
        !logConsole || !bridgeEnabled || !minimumWidth || !minimumHeight || !preferredWidth || !preferredHeight || !url)
    {
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
    windowCreate.title = title;
    windowCreate.iconPath = iconPath;
    windowCreate.appId = appId;
    windowCreate.viewsEnabled = viewsEnabled.value_or(false);
    windowCreate.modifyTimeoutMs = modifyTimeoutMs.value_or(0);
    windowCreate.modifyTimeoutPolicy = modifyTimeoutPolicy.value_or(0) == 1 ? ModifyTimeoutPolicy::Cancel : ModifyTimeoutPolicy::Continue;
    windowCreate.proxyOpenTimeoutMs = proxyOpenTimeoutMs.value_or(0);
    initialUrl = *url;
    return CreateBrowserWindow(windowCreate);
}

void HandleWindowCreate(PacketReader& reader, ipc::Reply reply)
{
    std::string url;
    IPC::Singleton.BeginAnnounceHold();
    CefRefPtr<Client> client = HandleWindowCreateInternal(reader, url);
    if (!client)
    {
        IPC::Singleton.EndAnnounceHold();
        reply.Fail(ipc::StatusCode::Error, "Failed to create the window.");
        return;
    }

    LOG(INFO) << "Client created with identifier " << client->GetIdentifier();
    PacketWriter writer;
    writer.write<int32_t>(client->GetIdentifier());
    reply.Ok(writer.data(), writer.size());
    IPC::Singleton.EndAnnounceHold();

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(client->GetIdentifier());
    if (!url.empty() && browser && browser->GetMainFrame())
        browser->GetMainFrame()->LoadURL(url);
}

void IPC::HandleWindowBridgeRpcRequest(ipc::Reply reply, PacketReader& reader)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    std::optional<std::string> payload_json = ReadBytesAsString(reader);
    if (!identifier || !method || !payload_json)
    {
        reply.Fail(ipc::StatusCode::InvalidRequest, "WindowBridgeRpc called without valid data.");
        return;
    }

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        reply.Fail(ipc::StatusCode::NotFound, "HandleWindowBridgeRpc called while the browser is already closed.");
        return;
    }

    CefRefPtr<CefClient> cef_client = browser->GetHost()->GetClient();
    Client* client = static_cast<Client*>(cef_client.get());
    if (!client)
    {
        reply.Fail(ipc::StatusCode::Error, "HandleWindowBridgeRpc failed to acquire the client.");
        return;
    }

    client->StartBridgeRpcCall(browser, *method, *payload_json, std::move(reply));
}

ipc::StatusCode HandleWindowMaximize(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowMaximize called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowMaximize called while CefBrowser " << *identifier << " is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browserView = CefBrowserView::GetForBrowser(browser);
    if (browserView)
    {
        CefRefPtr<CefWindow> window = browserView->GetWindow();
        window->Maximize();
    }
    else
    {
        shared::PlatformMaximize(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowMinimize(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowMinimize called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowMinimize called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Minimize();
    }
    else
    {
        shared::PlatformMinimize(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowRestore(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowRestore called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowRestore called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Restore();
    }
    else
    {
        shared::PlatformRestore(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowShow(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowShow called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowShow called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Show();
    }
    else
    {
        shared::PlatformShow(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowHide(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowHide called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowHide called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Hide();
    }
    else
    {
        shared::PlatformHide(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowActivate(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowActivate called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowActivate called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->Activate();
    }
    else
    {
        shared::PlatformActivate(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowBringToTop(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowBringToTop called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowBringToTop called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->BringToTop();
    }
    else
    {
        shared::PlatformBringToTop(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetAlwaysOnTop(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> alwaysOnTop = reader.read<bool>();
    if (!identifier || !alwaysOnTop)
    {
        LOG(ERROR) << "HandleWindowSetAlwaysOnTop called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetAlwaysOnTop called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetAlwaysOnTop(*alwaysOnTop);
    }
    else
    {
        shared::PlatformSetAlwaysOnTop(browser, *alwaysOnTop);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetFullscreen(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> fullscreen = reader.read<bool>();
    if (!identifier || !fullscreen)
    {
        LOG(ERROR) << "HandleWindowSetFullscreen called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetFullscreen called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetFullscreen(*fullscreen ? 1 : 0);
    }
    else
    {
        shared::PlatformSetFullscreen(browser, *fullscreen == 1);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowCenterSelf(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowCenterSelf called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowCenterSelf called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->CenterWindow(window->GetSize());
    }
    else
    {
        shared::PlatformCenterWindow(browser, shared::PlatformGetWindowSize(browser));
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetProxyRequests(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> setProxyRequests = reader.read<bool>();
    if (!identifier || !setProxyRequests)
    {
        LOG(ERROR) << "HandleWindowSetProxyRequests called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetProxyRequests called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowSetProxyRequests client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->SetProxyRequests(*setProxyRequests);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowGetPosition(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetPosition called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetPosition called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefPoint position;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        position = window->GetPosition();
    }
    else
    {
        position = shared::PlatformGetWindowPosition(browser);
    }

    writer.write<int32_t>(position.x);
    writer.write<int32_t>(position.y);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetPosition(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<int32_t> x = reader.read<int32_t>();
    std::optional<int32_t> y = reader.read<int32_t>();
    if (!identifier || !x || !y)
    {
        LOG(ERROR) << "HandleWindowSetPosition called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetPosition called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefPoint position;
    position.x = *x;
    position.y = *y;

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetPosition(position);
    }
    else
    {
        shared::PlatformSetWindowPosition(browser, position);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetDevelopmentToolsEnabled(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> developerToolsEnabled = reader.read<bool>();
    if (!identifier || !developerToolsEnabled)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsEnabled called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsEnabled called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsEnabled client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->settings.developerToolsEnabled = *developerToolsEnabled;

    if (!pClient->settings.developerToolsEnabled && browser->GetHost()->HasDevTools())
        browser->GetHost()->CloseDevTools();
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetDevelopmentToolsVisible(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> developerToolsVisible = reader.read<bool>();
    if (!identifier || !developerToolsVisible)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsVisible called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetDevelopmentToolsVisible called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
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
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowClose(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<bool> forceClosed = reader.read<bool>();
    if (!identifier || !forceClosed)
    {
        LOG(ERROR) << "HandleWindowClose called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowClose called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    browser->GetHost()->CloseBrowser(*forceClosed);
    return ipc::StatusCode::Ok;
}
ipc::StatusCode HandleWindowLoadUrl(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleWindowLoadUrl called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowLoadUrl called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    if (!frame)
    {
        LOG(ERROR) << "HandleWindowLoadUrl called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    frame->LoadURL(*url);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetZoom(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<double> zoom = reader.read<double>();
    if (!identifier || !zoom)
    {
        LOG(ERROR) << "HandleWindowSetZoom called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetZoom called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    browser->GetHost()->SetZoomLevel(*zoom);
    justcef_view::PropagateHostZoom(browser, *zoom);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleGetWidevineStatus(PacketReader& reader, PacketWriter& writer)
{
    const shared::WidevineStatus status = shared::GetWidevineStatus();

    writer.write<int32_t>(status.state);
    writer.writeSizePrefixedString(status.version);
    writer.write<uint8_t>(status.registered ? 1 : 0);
    writer.write<uint8_t>(status.installed ? 1 : 0);
    writer.write<uint8_t>(status.requiresRestart ? 1 : 0);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowGetZoom(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetZoom called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetZoom called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    writer.write<double>(browser->GetHost()->GetZoomLevel());
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowRequestFocus(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowRequestFocus called without CefBrowser. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowRequestFocus called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->RequestFocus();
        browser_view->RequestFocus();
    }
    else
    {
        shared::PlatformWindowRequestFocus(browser);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetModifyRequests(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<uint8_t> flags = reader.read<uint8_t>();
    if (!identifier || !flags)
    {
        LOG(ERROR) << "HandleWindowSetModifyRequests called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetModifyRequests called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowSetModifyRequests client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->SetModifyRequests((*flags & 1) != 0, (*flags & 2) != 0);
    return ipc::StatusCode::Ok;
}

void HandleWindowOpenDirectoryPicker(PacketReader& reader, ipc::Reply reply)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowOpenDirectoryPicker called without valid data. Ignored.";
        return;
    }

    auto sharedReply = std::make_shared<ipc::Reply>(std::move(reply));
    const int32_t browserIdentifier = *identifier;
    sharedReply->OnCanceled(
        [browserIdentifier]()
        {
            shared::CancelPendingFileDialogs(browserIdentifier);
        });
    shared::PlatformPickDirectory(*identifier,
                                  [sharedReply](std::string path)
                                  {
                                      PacketWriter writer;
                                      writer.writeSizePrefixedString(path);
                                      sharedReply->Ok(writer.data(), writer.size());
                                  });
}

void HandleWindowOpenFilePicker(const std::vector<std::string>& paths, PacketWriter& writer)
{
    writer.write<uint32_t>((uint32_t)paths.size());

    for (uint32_t i = 0; i < paths.size(); i++)
        writer.writeSizePrefixedString(paths[i]);
}

void HandleWindowOpenFilePicker(PacketReader& reader, ipc::Reply reply)
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

    if (*filterCount > reader.remainingSize() / (2 * sizeof(int32_t)))
    {
        LOG(ERROR) << "HandleWindowOpenFilePicker called with an impossible filter count. Ignored.";
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

    auto sharedReply = std::make_shared<ipc::Reply>(std::move(reply));
    const int32_t browserIdentifier = *identifier;
    sharedReply->OnCanceled(
        [browserIdentifier]()
        {
            shared::CancelPendingFileDialogs(browserIdentifier);
        });
    shared::PlatformPickFiles(*identifier, *multiple, filters,
                              [sharedReply](std::vector<std::string> paths)
                              {
                                  PacketWriter writer;
                                  HandleWindowOpenFilePicker(paths, writer);
                                  sharedReply->Ok(writer.data(), writer.size());
                              });
}

void HandleWindowSaveFilePicker(PacketReader& reader, ipc::Reply reply)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> defaultName = reader.readSizePrefixedString();
    std::optional<uint32_t> filterCount = reader.read<uint32_t>();
    if (!identifier || !defaultName || !filterCount)
    {
        LOG(ERROR) << "HandleWindowSaveFilePicker called without valid data. Ignored.";
        return;
    }

    if (*filterCount > reader.remainingSize() / (2 * sizeof(int32_t)))
    {
        LOG(ERROR) << "HandleWindowSaveFilePicker called with an impossible filter count. Ignored.";
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

    auto sharedReply = std::make_shared<ipc::Reply>(std::move(reply));
    const int32_t browserIdentifier = *identifier;
    sharedReply->OnCanceled(
        [browserIdentifier]()
        {
            shared::CancelPendingFileDialogs(browserIdentifier);
        });
    shared::PlatformSaveFile(*identifier, *defaultName, filters,
                             [sharedReply](std::string path)
                             {
                                 PacketWriter writer;
                                 writer.writeSizePrefixedString(path);
                                 sharedReply->Ok(writer.data(), writer.size());
                             });
}

void CloseEverything()
{
    if (!CefCurrentlyOn(TID_UI))
    {
        CefPostTask(TID_UI, base::BindOnce(&CloseEverything));
        return;
    }

    if (ClientManager::GetInstance()->GetBrowserCount() > 0)
    {
        ClientManager::GetInstance()->CloseAllBrowsers(true);
    }
    else
    {
        CefQuitMessageLoop();
    }
}

void IPC::HandleWindowExecuteDevToolsMethodRequest(ipc::Reply reply, PacketReader& reader)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    std::optional<bool> hasJson = reader.read<bool>();
    if (!identifier || !method || !hasJson)
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod called without valid data. Ignored.";
        reply.Fail(ipc::StatusCode::InvalidRequest, "HandleWindowExecuteDevToolsMethod called without valid data.");
        return;
    }

    CefRefPtr<CefDictionaryValue> params = nullptr;
    if (*hasJson)
    {
        std::optional<std::string> json = ReadBytesAsString(reader);
        CefRefPtr<CefValue> value = json ? CefParseJSON(*json, cef_json_parser_options_t::JSON_PARSER_RFC) : nullptr;
        if (!value || value->GetType() != VTYPE_DICTIONARY)
        {
            LOG(ERROR) << "HandleWindowExecuteDevToolsMethod called without valid DevTools params payload. Ignored.";
            reply.Fail(ipc::StatusCode::InvalidRequest, "DevTools params must be a JSON object.");
            return;
        }

        params = value->GetDictionary();
    }

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod called while CefBrowser is already closed. Ignored.";
        reply.Fail(ipc::StatusCode::NotFound, "The browser is already closed.");
        return;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowExecuteDevToolsMethod client is null. Ignored.";
        reply.Fail(ipc::StatusCode::Error, "The client is not available.");
        return;
    }

    auto sharedReply = std::make_shared<ipc::Reply>(std::move(reply));
    const bool started = pClient->ExecuteDevToolsMethodAsync(browser, *method, params,
                                                              [sharedReply](bool success, std::string result)
                                                              {
                                                                  PacketWriter writer;
                                                                  if (!writer.write(success) || !WriteBytes(writer, reinterpret_cast<const uint8_t*>(result.data()), result.size()))
                                                                  {
                                                                      sharedReply->Fail(ipc::StatusCode::TooLarge, "The DevTools result is too large.");
                                                                      return;
                                                                  }

                                                                  sharedReply->Ok(writer.data(), writer.size());
                                                              });
    if (!started)
    {
        PacketWriter writer;
        writer.write(false);
        writer.write<uint32_t>(0);
        sharedReply->Ok(writer.data(), writer.size());
    }
}

ipc::StatusCode HandleWindowSetTitle(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> title = reader.readSizePrefixedString();
    if (!identifier || !title)
    {
        LOG(ERROR) << "HandleWindowSetTitle called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetTitle called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowSetTitle client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->OverrideTitle(browser, *title);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetIcon(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> iconPath = reader.readSizePrefixedString();
    if (!identifier || !iconPath)
    {
        LOG(ERROR) << "HandleWindowSetIcon called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetIcon called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleWindowSetIcon client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->OverrideIcon(browser, *iconPath);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleAddUrlToProxy(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleAddUrlToProxy called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddUrlToProxy called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleAddUrlToProxy client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->AddUrlToProxy(*url);
    LOG(INFO) << "Added URL to proxy: " + *url;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleRemoveUrlToProxy(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleRemoveUrlToProxy client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->RemoveUrlToProxy(*url);
    LOG(INFO) << "Removed URL to proxy: " + *url;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleAddDomainToProxy(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> domain = reader.readSizePrefixedString();
    if (!identifier || !domain)
    {
        LOG(ERROR) << "HandleAddDomainToProxy called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddDomainToProxy called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleAddDomainToProxy client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->AddDomainToProxy(*domain);
    LOG(INFO) << "Added domain to proxy: " + *domain;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleRemoveDomainToProxy(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleRemoveDomainToProxy client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->RemoveDomainToProxy(*url);
    LOG(INFO) << "Removed domain to proxy: " + *url;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleAddUrlToModify(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleAddUrlToModify called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddUrlToModify called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleAddUrlToModify client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->AddUrlToModify(*url);
    LOG(INFO) << "Added URL to modify: " + *url;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleRemoveUrlToModify(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> url = reader.readSizePrefixedString();
    if (!identifier || !url)
    {
        LOG(ERROR) << "HandleRemoveUrlToModify called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveUrlToModify called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleRemoveUrlToModify client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->RemoveUrlToModify(*url);
    LOG(INFO) << "Removed URL to modify: " + *url;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowGetSize(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    if (!identifier)
    {
        LOG(ERROR) << "HandleWindowGetSize called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowGetSize called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefSize size;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        size = window->GetSize();
    }
    else
    {
        size = shared::PlatformGetWindowSize(browser);
    }

    writer.write<int32_t>(size.width);
    writer.write<int32_t>(size.height);
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleWindowSetSize(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<int32_t> width = reader.read<int32_t>();
    std::optional<int32_t> height = reader.read<int32_t>();
    if (!identifier || !width || !height)
    {
        LOG(ERROR) << "HandleWindowSetSize called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleWindowSetSize called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefSize size(*width, *height);
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view)
    {
        CefRefPtr<CefWindow> window = browser_view->GetWindow();
        window->SetSize(size);
    }
    else
    {
        shared::PlatformSetWindowSize(browser, size);
    }
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleAddDevToolsEventMethod(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    if (!identifier || !method)
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleAddDevToolsEventMethod client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->AddDevToolsEventMethod(browser, *method);
    LOG(INFO) << "Added DevTools event method: " + *method;
    return ipc::StatusCode::Ok;
}

ipc::StatusCode HandleRemoveDevToolsEventMethod(PacketReader& reader, PacketWriter& writer)
{
    std::optional<int32_t> identifier = reader.read<int32_t>();
    std::optional<std::string> method = reader.readSizePrefixedString();
    if (!identifier || !method)
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod called without valid data. Ignored.";
        return ipc::StatusCode::InvalidRequest;
    }
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(*identifier);
    if (!browser)
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod called while CefBrowser is already closed. Ignored.";
        return ipc::StatusCode::NotFound;
    }

    CefRefPtr<CefClient> client = browser->GetHost()->GetClient();
    Client* pClient = (Client*)client.get();
    if (!pClient)
    {
        LOG(ERROR) << "HandleRemoveDevToolsEventMethod client is null. Ignored.";
        return ipc::StatusCode::Error;
    }

    pClient->RemoveDevToolsEventMethod(browser, *method);
    LOG(INFO) << "Removed DevTools event method: " + *method;
    return ipc::StatusCode::Ok;
}

void IPC::HandleDebug(ipc::Reply reply, PacketReader& reader)
{
    if (!_debugEnabled)
    {
        reply.Fail(ipc::StatusCode::Unsupported, "IPC debugging is disabled.");
        return;
    }

    std::optional<uint8_t> action = reader.read<uint8_t>();
    std::optional<uint32_t> value = reader.read<uint32_t>();
    if (!action || !value)
    {
        reply.Fail(ipc::StatusCode::InvalidRequest, "Malformed debug request.");
        return;
    }

    if (*action == 0)
    {
        for (uint32_t i = 0; i < *value; ++i)
            NotifyDebug(i);
        reply.Ok();
        return;
    }

    if (*action == 1)
    {
        CefPostTask(TID_UI, base::BindOnce(
                                [](uint32_t milliseconds, ipc::Reply reply)
                                {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
                                    reply.Ok();
                                },
                                *value, std::move(reply)));
        return;
    }

    if (*action == 2)
    {
        auto sharedReply = std::make_shared<ipc::Reply>(std::move(reply));
        CefPostTask(TID_IO, base::BindOnce(
                                [](uint32_t count, std::shared_ptr<ipc::Reply> sharedReply)
                                {
                                    auto remaining = std::make_shared<std::atomic<uint32_t>>(count);
                                    auto completed = std::make_shared<std::atomic<uint32_t>>(0);
                                    if (count == 0)
                                    {
                                        PacketWriter writer;
                                        writer.write<uint32_t>(0);
                                        sharedReply->Ok(writer.data(), writer.size());
                                        return;
                                    }

                                    for (uint32_t i = 0; i < count; ++i)
                                    {
                                        IPC::Singleton.Echo(std::vector<uint8_t>(16, static_cast<uint8_t>(i)), std::chrono::milliseconds(30000),
                                                            [sharedReply, remaining, completed](ipc::Response response)
                                                            {
                                                                if (response.status == ipc::StatusCode::Ok)
                                                                    ++*completed;
                                                                if (--*remaining == 0)
                                                                {
                                                                    PacketWriter writer;
                                                                    writer.write<uint32_t>(completed->load());
                                                                    sharedReply->Ok(writer.data(), writer.size());
                                                                }
                                                            });
                                    }
                                },
                                *value, sharedReply));
        return;
    }

    reply.Fail(ipc::StatusCode::InvalidRequest, "Unknown debug action.");
}
