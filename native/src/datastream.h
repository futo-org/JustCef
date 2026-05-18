#ifndef DATASTREAM_H
#define DATASTREAM_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

enum class StreamState : uint8_t
{
    Active = 0,
    Completed = 1,
    Canceled = 2,
    Error = 3
};

class DataStream
{
public:
    DataStream(uint32_t identifier, size_t bufferSize = 10 * 1024 * 1024);

    size_t TryWrite(const uint8_t* data, size_t length);

    void MarkCompleted(uint64_t totalBytes);
    void MarkCanceled();
    void MarkError();

    size_t ReadSome(uint8_t* buffer, size_t bufferSize);

    size_t Read(uint8_t* buffer, size_t bufferSize);

    StreamState State() const;
    bool Drained() const;
    uint64_t ConsumedTotal() const;
    uint64_t FinalTotal() const;
    uint64_t Capacity() const { return _capacity; }

    void RegisterReadWakeup(std::function<void()> cb);

    void RegisterSpaceWakeup(std::function<void()> cb);

    uint32_t GetIdentifier() const { return _identifier; }

private:
    uint32_t _identifier;
    std::vector<uint8_t> _buffer;
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    size_t _head = 0, _tail = 0, _size = 0, _capacity;
    StreamState _state = StreamState::Active;
    uint64_t _consumedTotal = 0;
    uint64_t _finalTotal = 0;
    std::function<void()> _readWakeup;
    std::function<void()> _spaceWakeup;

    bool isEmpty() const { return _size == 0; }
};

#endif // DATASTREAM_H
