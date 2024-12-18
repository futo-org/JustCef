#ifndef PACKET_WRITER_H
#define PACKET_WRITER_H

#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

class PacketWriter {
public:
    explicit PacketWriter(size_t maxSize = 10 * 1024 * 1024)
        : _maxSize(maxSize) {
        _buffer.reserve(std::min(_maxSize, static_cast<size_t>(512)));
    }

    template <typename T>
    bool write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        return writeBytes(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }

    bool writeSizePrefixedString(const std::string& str) {
        int32_t length = static_cast<int32_t>(str.size());
        if (!write(length)) {
            return false;
        }
        return writeBytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    }

    bool writeString(const std::string& str) {
        return writeBytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
    }

    bool writeBytes(const uint8_t* data, size_t size) {
        if (size == 0) {
            return true;
        }

        size_t requiredCapacity = _buffer.size() + size;
        if (requiredCapacity > _maxSize) {
            return false;
        }

        if (_buffer.capacity() < requiredCapacity) {
            size_t newCapacity = std::max(_buffer.capacity() * 2, requiredCapacity);
            newCapacity = std::min(newCapacity, _maxSize);
            _buffer.reserve(newCapacity);
        }

        _buffer.insert(_buffer.end(), data, data + size);
        return true;
    }

    size_t size() const {
        return _buffer.size();
    }

    const uint8_t* data() const {
        return _buffer.data();
    }

private:
    std::vector<uint8_t> _buffer;
    size_t _maxSize;
};

#endif // PACKET_WRITER_H

