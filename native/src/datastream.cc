#include "datastream.h"
#include "include/base/cef_logging.h"

#include <algorithm>

DataStream::DataStream(uint32_t identifier, size_t bufferSize)
    : _identifier(identifier), _buffer(bufferSize), _capacity(bufferSize) {}

void DataStream::Write(const uint8_t* data, size_t length) 
{
    std::unique_lock<std::mutex> lock(_mutex);

    size_t offset = 0;
    while (offset < length) 
    {
        _cvWrite.wait(lock, [this] { return !isFull() || _isClosed; });
        if (_isClosed) break;

        size_t spaceAvailable = _capacity - _size;
        size_t writeLength = std::min(spaceAvailable, length - offset);
        size_t firstPart = std::min(writeLength, _capacity - _tail);

        std::copy_n(data + offset, firstPart, _buffer.begin() + _tail);
        offset += firstPart;
        _tail = (_tail + firstPart) % _capacity;
        _size += firstPart;

        if (firstPart < writeLength) 
        {
            size_t secondPart = writeLength - firstPart;

            std::copy_n(data + offset, secondPart, _buffer.begin() + _tail);
            offset += secondPart;
            _tail = (_tail + secondPart) % _capacity;
            _size += secondPart;
        }

        _cvRead.notify_all();
    }
}

size_t DataStream::Read(uint8_t* buffer, size_t bufferSize) 
{
    std::unique_lock<std::mutex> lock(_mutex);
    size_t bytesRead = 0;

    while (bytesRead < bufferSize) 
    {
        _cvRead.wait(lock, [this] { return !isEmpty() || _isClosed; });
        if (isEmpty() && _isClosed) break;

        size_t dataAvailable = _size;
        size_t readLength = std::min(dataAvailable, bufferSize - bytesRead);
        size_t firstPart = std::min(readLength, _capacity - _head);

        std::copy_n(_buffer.begin() + _head, firstPart, buffer + bytesRead);
        bytesRead += firstPart;
        _head = (_head + firstPart) % _capacity;
        _size -= firstPart;

        if (firstPart < readLength) 
        {
            size_t secondPart = readLength - firstPart;
            std::copy_n(_buffer.begin() + _head, secondPart, buffer + bytesRead);
            bytesRead += secondPart;
            _head = (_head + secondPart) % _capacity;
            _size -= secondPart;
        }

        _cvWrite.notify_all();

        if (!isEmpty()) break;
    }

    return bytesRead;
}

void DataStream::Close() 
{
    std::lock_guard<std::mutex> lock(_mutex);
    _isClosed = true;
    _cvRead.notify_all();
    _cvWrite.notify_all();
}

