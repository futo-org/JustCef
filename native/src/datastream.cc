#include "datastream.h"

#include <algorithm>
#include <utility>

DataStream::DataStream(uint32_t identifier, size_t bufferSize) : _identifier(identifier), _buffer(bufferSize), _capacity(bufferSize)
{
}

size_t DataStream::TryWrite(const uint8_t* data, size_t length)
{
    std::function<void()> wake;
    size_t written = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != StreamState::Active || length == 0)
            return 0;

        const size_t space = _capacity - _size;
        const size_t toWrite = std::min(space, length);

        const size_t firstPart = std::min(toWrite, _capacity - _tail);
        std::copy_n(data, firstPart, _buffer.begin() + _tail);
        _tail = (_tail + firstPart) % _capacity;
        if (firstPart < toWrite)
        {
            const size_t secondPart = toWrite - firstPart;
            std::copy_n(data + firstPart, secondPart, _buffer.begin() + _tail);
            _tail = (_tail + secondPart) % _capacity;
        }
        _size += toWrite;
        written = toWrite;

        if (written > 0)
        {
            _cv.notify_all();
            if (_readWakeup)
                wake = std::exchange(_readWakeup, nullptr);
        }
    }
    if (wake)
        wake();
    return written;
}

void DataStream::MarkCompleted(uint64_t totalBytes)
{
    std::function<void()> wake, spaceWake;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != StreamState::Active)
            return;
        _state = StreamState::Completed;
        _finalTotal = totalBytes;
        _cv.notify_all();
        wake = std::exchange(_readWakeup, nullptr);
        spaceWake = std::exchange(_spaceWakeup, nullptr);
    }
    if (wake)
        wake();
    if (spaceWake)
        spaceWake();
}

void DataStream::MarkCanceled()
{
    std::function<void()> wake, spaceWake;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != StreamState::Active)
            return;
        _state = StreamState::Canceled;
        _cv.notify_all();
        wake = std::exchange(_readWakeup, nullptr);
        spaceWake = std::exchange(_spaceWakeup, nullptr);
    }
    if (wake)
        wake();
    if (spaceWake)
        spaceWake();
}

void DataStream::MarkError()
{
    std::function<void()> wake, spaceWake;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != StreamState::Active)
            return;
        _state = StreamState::Error;
        _cv.notify_all();
        wake = std::exchange(_readWakeup, nullptr);
        spaceWake = std::exchange(_spaceWakeup, nullptr);
    }
    if (wake)
        wake();
    if (spaceWake)
        spaceWake();
}

size_t DataStream::ReadSome(uint8_t* buffer, size_t bufferSize)
{
    std::function<void()> spaceWake;
    size_t toRead = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_size == 0 || bufferSize == 0)
            return 0;

        toRead = std::min(_size, bufferSize);
        const size_t firstPart = std::min(toRead, _capacity - _head);
        std::copy_n(_buffer.begin() + _head, firstPart, buffer);
        _head = (_head + firstPart) % _capacity;
        if (firstPart < toRead)
        {
            const size_t secondPart = toRead - firstPart;
            std::copy_n(_buffer.begin() + _head, secondPart, buffer + firstPart);
            _head = (_head + secondPart) % _capacity;
        }
        _size -= toRead;
        _consumedTotal += toRead;

        spaceWake = std::exchange(_spaceWakeup, nullptr);
    }
    if (spaceWake)
        spaceWake();
    return toRead;
}

size_t DataStream::Read(uint8_t* buffer, size_t bufferSize)
{
    std::function<void()> spaceWake;
    size_t toRead = 0;
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (bufferSize == 0)
            return 0;
        _cv.wait(lock, [this] { return _size != 0 || _state != StreamState::Active; });
        if (_size == 0)
            return 0;

        toRead = std::min(_size, bufferSize);
        const size_t firstPart = std::min(toRead, _capacity - _head);
        std::copy_n(_buffer.begin() + _head, firstPart, buffer);
        _head = (_head + firstPart) % _capacity;
        if (firstPart < toRead)
        {
            const size_t secondPart = toRead - firstPart;
            std::copy_n(_buffer.begin() + _head, secondPart, buffer + firstPart);
            _head = (_head + secondPart) % _capacity;
        }
        _size -= toRead;
        _consumedTotal += toRead;

        spaceWake = std::exchange(_spaceWakeup, nullptr);
    }
    if (spaceWake)
        spaceWake();
    return toRead;
}

StreamState DataStream::State() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _state;
}

bool DataStream::Drained() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _size == 0 && _state != StreamState::Active;
}

uint64_t DataStream::ConsumedTotal() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _consumedTotal;
}

uint64_t DataStream::FinalTotal() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _finalTotal;
}

void DataStream::RegisterReadWakeup(std::function<void()> cb)
{
    std::function<void()> fireNow;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!isEmpty() || _state != StreamState::Active)
            fireNow = std::move(cb);
        else
            _readWakeup = std::move(cb);
    }
    if (fireNow)
        fireNow();
}

void DataStream::RegisterSpaceWakeup(std::function<void()> cb)
{
    std::function<void()> fireNow;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_size < _capacity || _state != StreamState::Active)
            fireNow = std::move(cb);
        else
            _spaceWakeup = std::move(cb);
    }
    if (fireNow)
        fireNow();
}
