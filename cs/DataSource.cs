namespace JustCef;

public interface IDataSource : IDisposable
{
    int Read(Span<byte> buffer);
    ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default);
}

public sealed class StreamDataSource : IDataSource
{
    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private bool _disposed;

    public StreamDataSource(Stream stream, bool leaveOpen = false)
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        _leaveOpen = leaveOpen;
    }

    public int Read(Span<byte> buffer)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _stream.Read(buffer);
    }

    public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _stream.ReadAsync(buffer, cancellationToken);
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        if (!_leaveOpen)
            _stream.Dispose();
    }
}

public sealed class FixedBytesDataSource : IDataSource
{
    private readonly ReadOnlyMemory<byte> _data;
    private int _position;
    private bool _disposed;

    public FixedBytesDataSource(byte[] data, bool copy = true)
        : this((ReadOnlyMemory<byte>)data, copy)
    {
    }

    public FixedBytesDataSource(ReadOnlyMemory<byte> data, bool copy = true)
    {
        _data = copy ? data.ToArray() : data;
    }

    public int Read(Span<byte> buffer)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (buffer.Length == 0)
            return 0;

        int remaining = _data.Length - _position;
        if (remaining <= 0)
            return 0;

        int bytesToRead = Math.Min(buffer.Length, remaining);
        _data.Span.Slice(_position, bytesToRead).CopyTo(buffer);
        _position += bytesToRead;

        return bytesToRead;
    }

    public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (cancellationToken.IsCancellationRequested)
            return ValueTask.FromCanceled<int>(cancellationToken);

        try
        {
            return new ValueTask<int>(Read(buffer.Span));
        }
        catch (Exception ex)
        {
            return ValueTask.FromException<int>(ex);
        }
    }

    public void Dispose()
    {
        _disposed = true;
    }
}