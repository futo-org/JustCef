#ifndef PACKET_READER_H
#define PACKET_READER_H

#include <cstring>
#include <string>
#include <optional>
#include <cstdint>

class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t size)
        : _data(data), _size(size), _position(0) {}

    template <typename T>
    std::optional<T> read() {
        if (!hasAvailable(sizeof(T)))
            return std::nullopt;

        T value;
        std::memcpy(&value, _data + _position, sizeof(T));
        _position += sizeof(T);
        return value;
    }

    std::optional<std::string> readString(uint32_t size) {
        if (!hasAvailable(size))
            return std::nullopt;

        std::string str(reinterpret_cast<const char*>(_data + _position), size);
        _position += size;
        return str;
    }

    bool readBytes(uint8_t* destination, uint32_t size) {
        if (!hasAvailable(size))
            return false;

        std::memcpy(destination, _data + _position, size);
        _position += size;
        return true;
    }

    std::optional<std::string> readSizePrefixedString() {
        std::optional<int32_t> size = read<int32_t>();
        if (!size || *size < 0) {
            return std::nullopt;
        }

        if (!hasAvailable(*size))
            return std::nullopt;

        std::string str(reinterpret_cast<const char*>(_data + _position), *size);
        _position += *size;
        return str;
    }

    bool copyTo(std::function<bool(const uint8_t*, size_t)> writer, size_t size) {
        if (!hasAvailable(size))
            return false;

        if (!writer(_data + _position, size))
            return false;

        _position += size;
        return true;
    }
    
    bool skip(size_t size) {
        if (!hasAvailable(size))
            return false;
        _position += size;
        return true;
    }

    bool hasAvailable(size_t size) {
        if (_position + size > _size)
            return false;
        return true;
    }

    size_t size() const {
        return _size;
    }

    size_t remainingSize() const {
        return _size - _position;
    }
private:
    const uint8_t* _data;
    size_t _size;
    size_t _position;
};

#endif // PACKET_READER_H
