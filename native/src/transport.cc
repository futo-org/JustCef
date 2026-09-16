#include "transport.h"

namespace ipc
{

Transport::~Transport()
{
    Shutdown(std::chrono::milliseconds(0));
}

bool Transport::Start(PacketHandler onPacket, ClosedHandler onClosed)
{
    if (_started.exchange(true))
        return false;
    if (!_pipe.Open())
        return false;

    _onPacket = std::move(onPacket);
    _onClosed = std::move(onClosed);
    _open = true;
    _accepting = true;
    _writer = std::thread([this] { WriteLoop(); });
    _reader = std::thread([this] { ReadLoop(); });
    return true;
}

bool Transport::Enqueue(PacketType type, uint8_t opcode, uint32_t requestId, const uint8_t* body, size_t size)
{
    if (size > kMaxBodySize || !_accepting)
        return false;

    std::vector<uint8_t> packet = EncodePacket(type, opcode, requestId, body, size);
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_accepting || !_open)
            return false;
        _queuedBytes += packet.size();
        queued = _queuedBytes;
        _queue.push_back(std::move(packet));
    }

    if (queued > _queueWarningBytes)
    {
        LOG(WARNING) << "The IPC send queue holds " << queued << " bytes. The controller is not reading.";
        _queueWarningBytes = queued * 2;
    }
    _queueChanged.notify_all();
    return true;
}

size_t Transport::QueuedBytes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _queuedBytes;
}

void Transport::ReadLoop()
{
    _pipe.BindReaderThread();
    uint8_t header[kHeaderSize];
    while (_open)
    {
        if (_pipe.Read(header, kHeaderSize, true) != kHeaderSize)
            break;

        uint32_t length = 0;
        Packet packet;
        std::memcpy(&length, header, sizeof(uint32_t));
        std::memcpy(&packet.requestId, header + 4, sizeof(uint32_t));
        packet.type = static_cast<PacketType>(header[8]);
        packet.opcode = header[9];

        if (length < kHeaderSize - sizeof(uint32_t) || packet.type > PacketType::Cancel)
            break;
        const size_t bodySize = static_cast<size_t>(length) + sizeof(uint32_t) - kHeaderSize;
        if (bodySize > kMaxBodySize)
            break;

        packet.body.resize(bodySize);
        if (bodySize > 0 && _pipe.Read(packet.body.data(), bodySize, true) != bodySize)
            break;

        if (_onPacket)
            _onPacket(std::move(packet));
    }
    MarkClosed();
    _readerExited = true;
}

void Transport::WriteLoop()
{
    _pipe.BindWriterThread();
    while (true)
    {
        std::vector<uint8_t> packet;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _queueChanged.wait(lock, [this] { return _stopWriter || !_queue.empty(); });
            if (_queue.empty())
                break;
            packet = std::move(_queue.front());
            _queue.pop_front();
            _writing = true;
        }

        const bool ok = _pipe.Write(packet.data(), packet.size(), true) == packet.size();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _writing = false;
            _queuedBytes -= packet.size();
        }
        _queueChanged.notify_all();
        if (!ok)
            break;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.clear();
        _queuedBytes = 0;
    }
    _queueChanged.notify_all();
    MarkClosed();
    _writerExited = true;
}

void Transport::MarkClosed()
{
    _accepting = false;
    _open = false;
    _queueChanged.notify_all();
    if (!_closedNotified.exchange(true) && _onClosed)
        _onClosed();
}

void Transport::Shutdown(std::chrono::milliseconds flushDeadline)
{
    if (!_started)
        return;

    std::call_once(_shutdownOnce,
                   [this, flushDeadline]
                   {
                       _accepting = false;
                       {
                           std::unique_lock<std::mutex> lock(_mutex);
                           _queueChanged.wait_for(lock, flushDeadline, [this] { return !_open || (_queue.empty() && !_writing); });
                           _stopWriter = true;
                       }
                       _queueChanged.notify_all();

                       _open = false;
                       _pipe.Interrupt();
                       for (int attempt = 0; attempt < 500 && (!_readerExited || !_writerExited); ++attempt)
                       {
                           std::this_thread::sleep_for(std::chrono::milliseconds(10));
                           _pipe.Interrupt();
                       }
                       if (_writer.joinable())
                           _writer.join();
                       if (_reader.joinable())
                           _reader.join();
                       _pipe.Close();
                   });
}

} // namespace ipc
