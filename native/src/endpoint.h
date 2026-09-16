#ifndef JUSTCEF_IPC_ENDPOINT_H_
#define JUSTCEF_IPC_ENDPOINT_H_

#include "event_loop.h"
#include "ipc_types.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipc
{

struct Response
{
    StatusCode status = StatusCode::Ok;
    std::string message;
    std::vector<uint8_t> payload;
};

using ResponseCallback = std::function<void(Response)>;

class Endpoint;

class CallHandle
{
public:
    CallHandle() = default;
    void Cancel() const;

private:
    friend class Endpoint;
    CallHandle(std::weak_ptr<Endpoint> endpoint, uint32_t requestId) : _endpoint(std::move(endpoint)), _requestId(requestId) {}

    std::weak_ptr<Endpoint> _endpoint;
    uint32_t _requestId = 0;
};

struct ReplyState;

class Reply
{
public:
    Reply() = default;
    ~Reply();
    Reply(Reply&& other) noexcept = default;
    Reply& operator=(Reply&& other) noexcept;
    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;

    void Ok(const std::vector<uint8_t>& payload = {});
    void Ok(const uint8_t* payload, size_t size);
    void Fail(StatusCode status, const std::string& message = std::string());
    bool IsCanceled() const;
    void OnCanceled(std::function<void()> callback);
    uint8_t Opcode() const;
    explicit operator bool() const { return static_cast<bool>(_state); }

private:
    friend class Endpoint;
    explicit Reply(std::shared_ptr<ReplyState> state) : _state(std::move(state)) {}

    std::shared_ptr<ReplyState> _state;
};

struct IncomingRequest
{
    uint8_t opcode = 0;
    uint32_t requestId = 0;
    std::vector<uint8_t> body;
};

class Endpoint : public std::enable_shared_from_this<Endpoint>
{
public:
    using RequestHandler = std::function<void(IncomingRequest, Reply)>;
    using NotificationHandler = std::function<void(uint8_t, std::vector<uint8_t>)>;

    static std::shared_ptr<Endpoint> Create(Transport& transport, EventLoop& loop);

    CallHandle Call(uint8_t opcode, std::vector<uint8_t> body, std::chrono::milliseconds timeout, ResponseCallback callback);
    bool Notify(uint8_t opcode, const uint8_t* body, size_t size);
    bool Notify(uint8_t opcode, const std::vector<uint8_t>& body) { return Notify(opcode, body.data(), body.size()); }
    void HandlePacket(Packet&& packet);
    void Close();
    bool IsClosed() const { return _closed; }
    void SetRequestHandler(RequestHandler handler) { _requestHandler = std::move(handler); }
    void SetNotificationHandler(NotificationHandler handler) { _notificationHandler = std::move(handler); }
    size_t PendingCallCount() const;

private:
    friend class CallHandle;
    friend class Reply;
    friend struct ReplyState;

    struct PendingCall
    {
        uint8_t opcode = 0;
        ResponseCallback callback;
        EventLoop::TimerId timer = 0;
    };

    Endpoint(Transport& transport, EventLoop& loop) : _transport(transport), _loop(loop) {}

    void CancelCall(uint32_t requestId, StatusCode status);
    void Complete(ResponseCallback callback, Response response);
    void SendReply(ReplyState& state, StatusCode status, const uint8_t* payload, size_t size, const std::string& message);
    void HandleResponse(Packet&& packet);
    void HandleRequest(Packet&& packet);
    void HandleCancel(const Packet& packet);

    Transport& _transport;
    EventLoop& _loop;
    mutable std::mutex _mutex;
    std::unordered_map<uint32_t, PendingCall> _pending;
    std::unordered_map<uint32_t, std::weak_ptr<ReplyState>> _inflight;
    uint32_t _nextRequestId = 0;
    std::atomic<bool> _closed{false};
    RequestHandler _requestHandler;
    NotificationHandler _notificationHandler;
};

struct ReplyState
{
    std::weak_ptr<Endpoint> endpoint;
    uint32_t requestId = 0;
    uint8_t opcode = 0;
    std::atomic<bool> sent{false};
    std::atomic<bool> canceled{false};
    std::mutex mutex;
    std::function<void()> cancelCallback;
};

} // namespace ipc

#endif // JUSTCEF_IPC_ENDPOINT_H_
