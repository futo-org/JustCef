#include "endpoint.h"

#include "packet_reader.h"
#include "packet_writer.h"

namespace ipc
{

namespace
{

std::vector<uint8_t> BuildResponseBody(StatusCode status, const uint8_t* payload, size_t size, const std::string& message)
{
    std::vector<uint8_t> body;
    if (status == StatusCode::Ok)
    {
        body.resize(1 + size);
        body[0] = static_cast<uint8_t>(StatusCode::Ok);
        if (size > 0)
            std::memcpy(body.data() + 1, payload, size);
        return body;
    }

    const int32_t length = static_cast<int32_t>(message.size());
    body.resize(1 + sizeof(int32_t) + message.size());
    body[0] = static_cast<uint8_t>(status);
    std::memcpy(body.data() + 1, &length, sizeof(int32_t));
    if (!message.empty())
        std::memcpy(body.data() + 1 + sizeof(int32_t), message.data(), message.size());
    return body;
}

StatusCode WireStatus(StatusCode status)
{
    if (status == StatusCode::Timeout || status == StatusCode::Disconnected)
        return StatusCode::Error;
    return status;
}

} // namespace

void CallHandle::Cancel() const
{
    if (auto endpoint = _endpoint.lock())
        endpoint->CancelCall(_requestId, StatusCode::Canceled);
}

Reply::~Reply()
{
    if (!_state || _state->sent)
        return;
    auto endpoint = _state->endpoint.lock();
    Fail(endpoint && !endpoint->IsClosed() ? StatusCode::Error : StatusCode::ShuttingDown, "The request was not answered.");
}

Reply& Reply::operator=(Reply&& other) noexcept
{
    if (this != &other)
    {
        Reply discarded(std::move(*this));
        _state = std::move(other._state);
    }
    return *this;
}

void Reply::Ok(const std::vector<uint8_t>& payload)
{
    Ok(payload.data(), payload.size());
}

void Reply::Ok(const uint8_t* payload, size_t size)
{
    if (!_state)
        return;
    if (auto endpoint = _state->endpoint.lock())
        endpoint->SendReply(*_state, StatusCode::Ok, payload, size, std::string());
    else
        _state->sent = true;
}

void Reply::Fail(StatusCode status, const std::string& message)
{
    if (!_state)
        return;
    if (auto endpoint = _state->endpoint.lock())
        endpoint->SendReply(*_state, WireStatus(status), nullptr, 0, message);
    else
        _state->sent = true;
}

bool Reply::IsCanceled() const
{
    return _state && _state->canceled;
}

void Reply::OnCanceled(std::function<void()> callback)
{
    if (!_state)
        return;
    bool invokeNow = false;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (_state->canceled)
            invokeNow = true;
        else
            _state->cancelCallback = callback;
    }
    if (invokeNow && callback)
        callback();
}

uint8_t Reply::Opcode() const
{
    return _state ? _state->opcode : 0;
}

std::shared_ptr<Endpoint> Endpoint::Create(Transport& transport, EventLoop& loop)
{
    return std::shared_ptr<Endpoint>(new Endpoint(transport, loop));
}

CallHandle Endpoint::Call(uint8_t opcode, std::vector<uint8_t> body, std::chrono::milliseconds timeout, ResponseCallback callback)
{
    uint32_t requestId = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_closed)
        {
            do
            {
                requestId = ++_nextRequestId;
            } while (requestId == 0 || _pending.count(requestId) != 0);
            _pending.emplace(requestId, PendingCall{opcode, std::move(callback), 0});
        }
    }

    if (requestId == 0)
    {
        Complete(std::move(callback), Response{StatusCode::Disconnected, "IPC is not connected.", {}});
        return CallHandle();
    }

    if (body.size() > kMaxBodySize)
    {
        CancelCall(requestId, StatusCode::TooLarge);
        return CallHandle();
    }

    if (!_transport.Enqueue(PacketType::Request, opcode, requestId, body))
    {
        CancelCall(requestId, StatusCode::Disconnected);
        return CallHandle();
    }

    if (timeout.count() > 0)
    {
        std::weak_ptr<Endpoint> weak = weak_from_this();
        const EventLoop::TimerId timer = _loop.PostDelayed(timeout,
                                                           [weak, requestId]
                                                           {
                                                               if (auto endpoint = weak.lock())
                                                                   endpoint->CancelCall(requestId, StatusCode::Timeout);
                                                           });
        std::lock_guard<std::mutex> lock(_mutex);
        auto pending = _pending.find(requestId);
        if (pending != _pending.end())
            pending->second.timer = timer;
        else
            _loop.CancelTimer(timer);
    }

    return CallHandle(weak_from_this(), requestId);
}

bool Endpoint::Notify(uint8_t opcode, const uint8_t* body, size_t size)
{
    if (_closed)
        return false;
    return _transport.Enqueue(PacketType::Notification, opcode, 0, body, size);
}

void Endpoint::CancelCall(uint32_t requestId, StatusCode status)
{
    PendingCall call;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto pending = _pending.find(requestId);
        if (pending == _pending.end())
            return;
        call = std::move(pending->second);
        _pending.erase(pending);
    }

    _loop.CancelTimer(call.timer);
    if (status == StatusCode::Timeout || status == StatusCode::Canceled)
        _transport.Enqueue(PacketType::Cancel, call.opcode, requestId, nullptr, 0);

    const char* message = status == StatusCode::Timeout ? "The request timed out." : status == StatusCode::Canceled ? "The request was canceled." : status == StatusCode::TooLarge ? "The request is too large." : "IPC is not connected.";
    Complete(std::move(call.callback), Response{status, message, {}});
}

void Endpoint::Complete(ResponseCallback callback, Response response)
{
    if (!callback)
        return;
    if (_loop.RunsTasksOnCurrentThread())
    {
        callback(std::move(response));
        return;
    }

    auto shared = std::make_shared<std::pair<ResponseCallback, Response>>(std::move(callback), std::move(response));
    if (!_loop.Post([shared] { shared->first(std::move(shared->second)); }))
        shared->first(std::move(shared->second));
}

void Endpoint::HandlePacket(Packet&& packet)
{
    switch (packet.type)
    {
    case PacketType::Response:
        HandleResponse(std::move(packet));
        break;
    case PacketType::Request:
        HandleRequest(std::move(packet));
        break;
    case PacketType::Notification:
        if (_notificationHandler)
            _notificationHandler(packet.opcode, std::move(packet.body));
        break;
    case PacketType::Cancel:
        HandleCancel(packet);
        break;
    }
}

void Endpoint::HandleResponse(Packet&& packet)
{
    PendingCall call;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto pending = _pending.find(packet.requestId);
        if (pending == _pending.end())
            return;
        call = std::move(pending->second);
        _pending.erase(pending);
    }
    _loop.CancelTimer(call.timer);

    Response response;
    if (packet.body.empty())
    {
        response.status = StatusCode::InvalidRequest;
        response.message = "The response has no status.";
    }
    else
    {
        response.status = static_cast<StatusCode>(packet.body[0]);
        if (response.status == StatusCode::Ok)
        {
            response.payload.assign(packet.body.begin() + 1, packet.body.end());
        }
        else
        {
            PacketReader reader(packet.body.data() + 1, packet.body.size() - 1);
            std::optional<std::string> message = reader.readSizePrefixedString();
            if (message)
                response.message = std::move(*message);
        }
    }
    Complete(std::move(call.callback), std::move(response));
}

void Endpoint::HandleRequest(Packet&& packet)
{
    if (packet.requestId == 0)
    {
        LOG(ERROR) << "Received a request with request id 0. Ignored.";
        return;
    }

    auto state = std::make_shared<ReplyState>();
    state->endpoint = weak_from_this();
    state->requestId = packet.requestId;
    state->opcode = packet.opcode;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_inflight.count(packet.requestId) != 0)
        {
            LOG(ERROR) << "Received a duplicate request id " << packet.requestId << ".";
            state->sent = true;
            _transport.Enqueue(PacketType::Response, packet.opcode, packet.requestId,
                               BuildResponseBody(StatusCode::InvalidRequest, nullptr, 0, "The request id is already in flight."));
            return;
        }
        _inflight[packet.requestId] = state;
    }

    Reply reply(state);
    if (_closed)
    {
        reply.Fail(StatusCode::ShuttingDown, "IPC is shutting down.");
        return;
    }
    if (!_requestHandler)
    {
        reply.Fail(StatusCode::Unsupported, "No request handler.");
        return;
    }

    _requestHandler(IncomingRequest{packet.opcode, packet.requestId, std::move(packet.body)}, std::move(reply));
}

void Endpoint::HandleCancel(const Packet& packet)
{
    std::shared_ptr<ReplyState> state;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto inflight = _inflight.find(packet.requestId);
        if (inflight == _inflight.end())
            return;
        state = inflight->second.lock();
    }
    if (!state || state->opcode != packet.opcode)
        return;

    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->canceled.exchange(true))
            return;
        callback = std::move(state->cancelCallback);
    }
    if (callback)
        callback();

    SendReply(*state, StatusCode::Canceled, nullptr, 0, "The request was canceled.");
}

void Endpoint::SendReply(ReplyState& state, StatusCode status, const uint8_t* payload, size_t size, const std::string& message)
{
    if (state.sent.exchange(true))
        return;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto inflight = _inflight.find(state.requestId);
        if (inflight != _inflight.end() && inflight->second.lock().get() == &state)
            _inflight.erase(inflight);
    }

    if (status == StatusCode::Ok && size > kMaxBodySize - 1)
    {
        status = StatusCode::TooLarge;
        payload = nullptr;
        size = 0;
    }

    std::vector<uint8_t> body = BuildResponseBody(status, payload, size, status == StatusCode::TooLarge && message.empty() ? "The response is too large." : message);
    _transport.Enqueue(PacketType::Response, state.opcode, state.requestId, body);
}

void Endpoint::Close()
{
    std::unordered_map<uint32_t, PendingCall> pending;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_closed.exchange(true))
            return;
        pending.swap(_pending);
    }

    for (auto& entry : pending)
    {
        _loop.CancelTimer(entry.second.timer);
        Complete(std::move(entry.second.callback), Response{StatusCode::Disconnected, "IPC is not connected.", {}});
    }
}

size_t Endpoint::PendingCallCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _pending.size();
}

} // namespace ipc
