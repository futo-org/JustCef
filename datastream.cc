#include "datastream.h"

#include <string.h>

DataStream::DataStream(uint32_t identifier, size_t bufferSize, size_t poolSize) : _identifier(identifier), _pool(bufferSize, poolSize), _readPos(0), _writePos(0) {}

void DataStream::Write(const uint8_t* data, size_t length) 
{
    std::unique_lock<std::mutex> lock(_mutex);
    size_t offset = 0;
    while (offset < length) {
        if (!_currentWriteBuffer || _writePos >= _currentWriteBuffer->size()) {
            if (_currentWriteBuffer) {
                _buffers.push_back(_currentWriteBuffer); // Full buffer, push to queue
            }
            _currentWriteBuffer = _pool.GetBuffer(); // Get a new buffer
            _writePos = 0; // Reset write position
        }

        size_t spaceLeft = _currentWriteBuffer->size() - _writePos;
        size_t chunkSize = std::min(length - offset, spaceLeft);
        memcpy(&(*_currentWriteBuffer)[_writePos], data + offset, chunkSize);

        _writePos += chunkSize;
        offset += chunkSize;

        if (_writePos == _currentWriteBuffer->size()) 
        {
            _buffers.push_back(_currentWriteBuffer);
            _currentWriteBuffer.reset(); // Ensure a new buffer is fetched next iteration
        }
    }
    _cv.notify_one(); // Notify readers
}

size_t DataStream::Read(uint8_t* buffer, size_t bufferSize) 
{
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this] { return !_buffers.empty() || _isClosed; });

    if (_buffers.empty() && _isClosed)
        return 0;

    if (!_currentReadBuffer || _readPos >= _currentReadBuffer->size()) 
    {
        if (!_buffers.empty()) 
        {
            _currentReadBuffer = _buffers.front();
            _buffers.pop_front();
            _readPos = 0;
        } 
        else if (_currentWriteBuffer && _writePos > 0) 
        { 
            // Last chunk in the write buffer
            _currentReadBuffer = _currentWriteBuffer;
            _currentWriteBuffer.reset();
            _buffers.push_back(_currentReadBuffer);
            _buffers.pop_front();
            _readPos = 0;
        }
    }

    size_t dataLeft = _currentReadBuffer->size() - _readPos;
    size_t toRead = std::min(bufferSize, dataLeft);
    memcpy(buffer, &(*_currentReadBuffer)[_readPos], toRead);
    _readPos += toRead;

    if (_readPos == _currentReadBuffer->size()) 
    {
        _pool.ReturnBuffer(_currentReadBuffer);
        _currentReadBuffer.reset();
    }

    return toRead;
}

void DataStream::Close()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _isClosed = true;
    _cv.notify_all();
}