#include "bufferpool.h"

BufferPool::BufferPool(size_t bufferSize, size_t initialPoolSize) : _bufferSize(bufferSize) 
{
    for (size_t i = 0; i < initialPoolSize; ++i) 
        _freeBuffers.emplace_back(new std::vector<uint8_t>(bufferSize));
}

std::shared_ptr<std::vector<uint8_t>> BufferPool::GetBuffer() {
    std::lock_guard<std::mutex> lock(_poolMutex);
    if (_freeBuffers.empty()) 
        return std::make_shared<std::vector<uint8_t>>(_bufferSize);
    else 
    {
        auto buffer = _freeBuffers.front();
        _freeBuffers.pop_front();
        return buffer;
    }
}

void BufferPool::ReturnBuffer(std::shared_ptr<std::vector<uint8_t>> buffer) 
{
    std::lock_guard<std::mutex> lock(_poolMutex);
    _freeBuffers.push_back(buffer);
}

