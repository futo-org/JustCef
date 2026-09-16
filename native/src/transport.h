#ifndef JUSTCEF_IPC_TRANSPORT_H_
#define JUSTCEF_IPC_TRANSPORT_H_

#include "ipc_types.h"
#include "pipe.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ipc
{

class Transport
{
public:
    using PacketHandler = std::function<void(Packet&&)>;
    using ClosedHandler = std::function<void()>;

    Transport() = default;
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    void SetHandles(NativeHandle readHandle, NativeHandle writeHandle) { _pipe.SetHandles(readHandle, writeHandle); }
    bool HasValidHandles() { return _pipe.HasValidHandles(); }
    bool Start(PacketHandler onPacket, ClosedHandler onClosed);
    bool Enqueue(PacketType type, uint8_t opcode, uint32_t requestId, const uint8_t* body, size_t size);
    bool Enqueue(PacketType type, uint8_t opcode, uint32_t requestId, const std::vector<uint8_t>& body) { return Enqueue(type, opcode, requestId, body.data(), body.size()); }
    void Shutdown(std::chrono::milliseconds flushDeadline);
    bool IsOpen() const { return _open; }
    size_t QueuedBytes() const;

private:
    void ReadLoop();
    void WriteLoop();
    void MarkClosed();

    Pipe _pipe;
    PacketHandler _onPacket;
    ClosedHandler _onClosed;
    std::thread _reader;
    std::thread _writer;
    mutable std::mutex _mutex;
    std::condition_variable _queueChanged;
    std::deque<std::vector<uint8_t>> _queue;
    size_t _queuedBytes = 0;
    std::atomic<size_t> _queueWarningBytes{64u * 1024u * 1024u};
    bool _writing = false;
    bool _stopWriter = false;
    std::atomic<bool> _open{false};
    std::atomic<bool> _accepting{false};
    std::atomic<bool> _closedNotified{false};
    std::atomic<bool> _started{false};
    std::atomic<bool> _readerExited{false};
    std::atomic<bool> _writerExited{false};
    std::once_flag _shutdownOnce;
};

} // namespace ipc

#endif // JUSTCEF_IPC_TRANSPORT_H_
