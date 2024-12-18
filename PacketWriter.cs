using System.Text;

namespace DotCef;

public class PacketWriter 
{
    private byte[] _data;
    private int _position;
    private int _maxSize;

    public byte[] Data => _data;
    public int Size => _position;

    public PacketWriter(int maxSize = 10 * 1024 * 1024) 
    {
        //TODO: Rent?
        _maxSize = maxSize;
        _data = new byte[Math.Min(maxSize, 512)];
        _position = 0;
    }

    public unsafe PacketWriter Write<T>(T value) where T : unmanaged 
    {
        int sizeOfT = sizeof(T);
        EnsureCapacity(_position + sizeOfT);

        fixed (byte* ptr = &_data[_position]) 
        {
            *(T*)ptr = value;
        }

        _position += sizeOfT;
        return this;
    }

    public PacketWriter WriteSizePrefixedString(string? str) 
    {
        if (str == null)
            Write(-1);
        else
        {
            byte[] bytes = Encoding.UTF8.GetBytes(str);
            Write(bytes.Length);
            WriteBytes(bytes);
        }
        return this;
    }

    public PacketWriter WriteString(string str) 
    {
        byte[] bytes = Encoding.UTF8.GetBytes(str);
        WriteBytes(bytes);
        return this;
    }

    public PacketWriter WriteBytes(byte[] data) 
    {
        EnsureCapacity(_position + data.Length);
        Buffer.BlockCopy(data, 0, _data, _position, data.Length);
        _position += data.Length;
        return this;
    }

    public PacketWriter WriteBytes(byte[] data, int offset, int size) 
    {
        EnsureCapacity(_position + size);
        Buffer.BlockCopy(data, offset, _data, _position, size);
        _position += size;
        return this;
    }

    private void EnsureCapacity(int requiredCapacity) 
    {
        if (requiredCapacity > _data.Length) 
        {
            int newSize = Math.Max(2 * _data.Length, requiredCapacity);
            if (newSize > _maxSize) 
                throw new InvalidOperationException("Exceeding max buffer size.");
            Array.Resize(ref _data, newSize);
        }
    }
}
