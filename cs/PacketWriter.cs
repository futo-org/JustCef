using System.Buffers;
using System.Text;
using System.Threading;

namespace JustCef;

public sealed class PacketWriter : IDisposable
{
    private byte[]? _data;
    private readonly int _maxSize;
    private int _position;
    private int _disposed;

    public byte[] Data => _data ?? throw new ObjectDisposedException(nameof(PacketWriter));
    public int Size => _position;

    public PacketWriter(int maxSize = 10 * 1024 * 1024) 
    {
        _maxSize = maxSize;
        _data = ArrayPool<byte>.Shared.Rent(Math.Min(maxSize, 512));
        _position = 0;
    }

    public unsafe PacketWriter Write<T>(T value) where T : unmanaged 
    {
        int sizeOfT = sizeof(T);
        EnsureCapacity(_position + sizeOfT);
        byte[] data = _data ?? throw new ObjectDisposedException(nameof(PacketWriter));

        fixed (byte* ptr = &data[_position]) 
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
            int byteCount = Encoding.UTF8.GetByteCount(str);
            Write(byteCount);
            WriteEncodedString(str, byteCount);
        }
        return this;
    }

    public PacketWriter WriteString(string str) 
    {
        ArgumentNullException.ThrowIfNull(str);

        int byteCount = Encoding.UTF8.GetByteCount(str);
        WriteEncodedString(str, byteCount);
        return this;
    }

    public PacketWriter WriteBytes(byte[] data) 
    {
        EnsureCapacity(_position + data.Length);
        Buffer.BlockCopy(data, 0, _data ?? throw new ObjectDisposedException(nameof(PacketWriter)), _position, data.Length);
        _position += data.Length;
        return this;
    }

    public PacketWriter WriteBytes(byte[] data, int offset, int size) 
    {
        EnsureCapacity(_position + size);
        Buffer.BlockCopy(data, offset, _data ?? throw new ObjectDisposedException(nameof(PacketWriter)), _position, size);
        _position += size;
        return this;
    }

    private void EnsureCapacity(int requiredCapacity) 
    {
        byte[] data = _data ?? throw new ObjectDisposedException(nameof(PacketWriter));
        if (requiredCapacity > data.Length) 
        {
            int newSize = Math.Max(2 * data.Length, requiredCapacity);
            if (newSize > _maxSize) 
                throw new InvalidOperationException("Exceeding max buffer size.");

            byte[] newBuffer = ArrayPool<byte>.Shared.Rent(newSize);
            Buffer.BlockCopy(data, 0, newBuffer, 0, _position);
            _data = newBuffer;
            ArrayPool<byte>.Shared.Return(data);
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        byte[]? data = _data;
        _data = null;
        _position = 0;
        if (data != null)
            ArrayPool<byte>.Shared.Return(data);
    }

    private void WriteEncodedString(string str, int byteCount)
    {
        EnsureCapacity(_position + byteCount);
        byte[] data = _data ?? throw new ObjectDisposedException(nameof(PacketWriter));
        int bytesWritten = Encoding.UTF8.GetBytes(str.AsSpan(), data.AsSpan(_position, byteCount));
        _position += bytesWritten;
    }
}
