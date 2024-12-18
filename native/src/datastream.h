#ifndef DATASTREAM_H
#define DATASTREAM_H

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstring>

class DataStream {
public:
    DataStream(uint32_t identifier, size_t bufferSize = 10 * 1024 * 1024);

    void Write(const uint8_t* data, size_t length);
    size_t Read(uint8_t* buffer, size_t bufferSize);
    void Close();

    uint32_t GetIdentifier() const { return _identifier; }

private:
    uint32_t _identifier;
    std::vector<uint8_t> _buffer;
    std::mutex _mutex;
    std::condition_variable _cvRead, _cvWrite;
    size_t _head = 0, _tail = 0, _size = 0, _capacity;
    bool _isClosed = false;

    bool isFull() const { return _size == _capacity; }
    bool isEmpty() const { return _size == 0; }
};

#endif // DATASTREAM_H

