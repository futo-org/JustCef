using System.Text;

namespace DotCef;

public class PacketReader
{
    private byte[] _data;
    private int _position;

    public int RemainingSize => _data.Length - _position;

    public PacketReader(byte[] data) : this(data, data.Length) { }
    public PacketReader(byte[] data, int size) 
    {
        if (size > data.Length)
            throw new ArgumentException("Size must be less than data size.");

        _data = data;
        _position = 0;
    }

    public unsafe T Read<T>() where T : unmanaged 
    {
        int sizeOfT = sizeof(T);
        if (_position + sizeOfT > _data.Length)
            throw new InvalidOperationException("Reading past the end of the data buffer.");

        T value;
        fixed (byte* ptr = &_data[_position]) 
        {
            value = *(T*)ptr;
        }
        _position += sizeOfT;
        return value;
    }

    public string ReadString(int size)
    {
        if (_position + size > _data.Length)
            throw new InvalidOperationException("Reading past the end of the data buffer.");

        string result = Encoding.UTF8.GetString(_data, _position, size);
        _position += size;
        return result;
    }

    public byte[] ReadBytes(int size)
    {
        if (_position + size > _data.Length)
            throw new InvalidOperationException("Reading past the end of the data buffer.");

        byte[] result = _data.AsSpan().Slice(_position, size).ToArray();
        _position += size;
        return result;
    }

    public string? ReadSizePrefixedString()
    {
        int size = Read<int>();
        if (size == -1)
            return null;
        return ReadString(size);
    }

    public void Skip(int size)
    {
        if (_position + size > _data.Length)
            throw new InvalidOperationException("Skipping past the end of the data buffer.");

        _position += size;
    }

    public bool HasAvailable(int size)
    {
        return _position + size <= _data.Length;
    }
}
