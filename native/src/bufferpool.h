#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include <list>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

class BufferPool
{
public:
    BufferPool(size_t bufferSize, size_t initialPoolSize);

    std::shared_ptr<std::vector<uint8_t>> GetBuffer();
    void ReturnBuffer(std::shared_ptr<std::vector<uint8_t>> buffer);

private:
    size_t _bufferSize;
    std::list<std::shared_ptr<std::vector<uint8_t>>> _freeBuffers;
    std::mutex _poolMutex;
};

#endif // BUFFER_POOL_H
