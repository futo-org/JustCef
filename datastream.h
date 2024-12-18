#ifndef DATASTREAM_H
#define DATASTREAM_H

#include "bufferpool.h"

#include <stddef.h>
#include <list>
#include <mutex>
#include <condition_variable>

class DataStream {
public:
    DataStream(uint32_t identifier, size_t bufferSize = 4096, size_t poolSize = 1024);

    void Write(const uint8_t* data, size_t length);
    size_t Read(uint8_t* buffer, size_t bufferSize);
    void Close();

    uint32_t GetIdentifier() { return _identifier; }
private:
    uint32_t _identifier;
    BufferPool _pool;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::list<std::shared_ptr<std::vector<uint8_t>>> _buffers;
    std::shared_ptr<std::vector<uint8_t>> _currentWriteBuffer;
    std::shared_ptr<std::vector<uint8_t>> _currentReadBuffer;
    size_t _readPos, _writePos;
    bool _isClosed = false;
};

#endif //DATASTREAM_H