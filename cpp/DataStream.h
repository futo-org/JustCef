#ifndef DATASTREAM_H
#define DATASTREAM_H

#include "AsyncSignal.h"
#include "IpcTypes.h"
#include "Packet.h"

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class DataStream : public std::enable_shared_from_this<DataStream>
{
public:
    using Sender = std::function<bool(justcef::detail::OutgoingPacket)>;
    using FinishedCallback = std::function<void(uint32_t)>;

    DataStream(uint32_t identifier, std::shared_ptr<justcef::ByteStream> source, std::optional<uint64_t> length, Sender sender);
    ~DataStream() { CloseSource(); }

    void Start(asio::any_io_executor executor, FinishedCallback onFinished);
    void AddCredit(uint32_t bytes);
    void Cancel();
    void CloseSource();

    uint32_t GetIdentifier() const { return _identifier; }

private:
    static asio::awaitable<void> Pump(std::shared_ptr<DataStream> self, FinishedCallback onFinished);
    asio::awaitable<void> WaitForCredit();
    uint64_t AvailableCredit();
    bool IsCanceled();
    void Send(justcef::detail::OpcodeControllerNotification opcode, std::vector<uint8_t> body);

    uint32_t _identifier;
    std::shared_ptr<justcef::ByteStream> _source;
    std::optional<uint64_t> _length;
    Sender _sender;
    std::mutex _mutex;
    uint64_t _credit = justcef::detail::kStreamInitialCredit;
    bool _isCanceled = false;
    justcef::detail::Completion<void()> _waiter;
    std::atomic<bool> _isSourceClosed = false;
};

#endif // DATASTREAM_H
