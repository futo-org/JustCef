#ifndef JUSTCEF_IPC_STREAM_RECEIVER_H_
#define JUSTCEF_IPC_STREAM_RECEIVER_H_

#include "ipc_types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipc
{

class StreamReceiver;

class IncomingStream : public std::enable_shared_from_this<IncomingStream>
{
public:
    enum class ReadResult
    {
        Data,
        Pending,
        End,
        Error
    };

    ReadResult Read(uint8_t* destination, size_t capacity, size_t& read);
    bool SetWakeup(std::function<void()> wakeup);
    void Cancel();
    int64_t Length() const { return _length; }
    uint32_t Id() const { return _id; }
    std::string ErrorMessage() const;

private:
    friend class StreamReceiver;
    IncomingStream(std::weak_ptr<StreamReceiver> receiver, uint32_t id, int64_t length) : _receiver(std::move(receiver)), _id(id), _length(length) {}

    void Push(const uint8_t* data, size_t size);
    void Finish(bool error, const std::string& message);
    std::function<void()> TakeWakeupLocked();

    std::weak_ptr<StreamReceiver> _receiver;
    const uint32_t _id;
    const int64_t _length;
    mutable std::mutex _mutex;
    std::deque<std::vector<uint8_t>> _chunks;
    size_t _chunkOffset = 0;
    size_t _buffered = 0;
    uint32_t _consumedSinceGrant = 0;
    bool _ended = false;
    bool _failed = false;
    bool _canceled = false;
    std::string _error;
    std::function<void()> _wakeup;
};

class StreamReceiver : public std::enable_shared_from_this<StreamReceiver>
{
public:
    using NotifyFunction = std::function<bool(uint8_t opcode, const std::vector<uint8_t>& body)>;

    static std::shared_ptr<StreamReceiver> Create(NotifyFunction notify, uint8_t creditOpcode, uint8_t cancelOpcode);

    std::shared_ptr<IncomingStream> Open(uint32_t id, int64_t length);
    void OnData(uint32_t id, const uint8_t* data, size_t size);
    void OnEnd(uint32_t id, uint64_t total);
    void OnError(uint32_t id, const std::string& message);
    void FailAll(const std::string& message);
    size_t OpenCount() const;

private:
    friend class IncomingStream;
    StreamReceiver(NotifyFunction notify, uint8_t creditOpcode, uint8_t cancelOpcode) : _notify(std::move(notify)), _creditOpcode(creditOpcode), _cancelOpcode(cancelOpcode) {}

    std::shared_ptr<IncomingStream> Find(uint32_t id);
    void Remove(uint32_t id);
    void SendCredit(uint32_t id, uint32_t bytes);
    void SendCancel(uint32_t id);

    NotifyFunction _notify;
    const uint8_t _creditOpcode;
    const uint8_t _cancelOpcode;
    mutable std::mutex _mutex;
    std::unordered_map<uint32_t, std::shared_ptr<IncomingStream>> _streams;
    std::unordered_map<uint32_t, bool> _canceledIds;
    std::deque<uint32_t> _canceledOrder;
};

} // namespace ipc

#endif // JUSTCEF_IPC_STREAM_RECEIVER_H_
