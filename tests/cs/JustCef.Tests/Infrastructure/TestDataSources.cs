using System.Security.Cryptography;
using System.Text;

namespace JustCef.Tests.Infrastructure;

public sealed class PatternDataSource : IDataSource
{
    private readonly long _length;
    private readonly TimeSpan _readDelay;
    private long _position;
    private int _disposeCount;
    private long _readCalls;

    public PatternDataSource(long length, TimeSpan readDelay = default)
    {
        _length = length;
        _readDelay = readDelay;
    }

    public long Position => Interlocked.Read(ref _position);
    public int DisposeCount => Volatile.Read(ref _disposeCount);
    public long ReadCalls => Interlocked.Read(ref _readCalls);
    public TaskCompletionSource Disposed { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public static byte At(long index) => (byte)(index % 251);

    public static string Sha256Hex(long length)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[1024 * 1024];
        long offset = 0;
        while (offset < length)
        {
            int count = (int)Math.Min(buffer.Length, length - offset);
            for (int i = 0; i < count; i++)
                buffer[i] = At(offset + i);
            hash.AppendData(buffer, 0, count);
            offset += count;
        }
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    public int Read(Span<byte> buffer)
    {
        ObjectDisposedException.ThrowIf(DisposeCount != 0, this);
        Interlocked.Increment(ref _readCalls);
        long position = Interlocked.Read(ref _position);
        int count = (int)Math.Min(buffer.Length, _length - position);
        if (count <= 0)
            return 0;

        for (int i = 0; i < count; i++)
            buffer[i] = At(position + i);
        Interlocked.Add(ref _position, count);
        return count;
    }

    public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (_readDelay > TimeSpan.Zero)
            await Task.Delay(_readDelay, cancellationToken).ConfigureAwait(false);
        return Read(buffer.Span);
    }

    public void Dispose()
    {
        if (Interlocked.Increment(ref _disposeCount) == 1)
            Disposed.TrySetResult();
    }
}

public sealed class FailingDataSource : IDataSource
{
    private readonly long _failAfter;
    private long _position;
    public int DisposeCount;

    public FailingDataSource(long failAfter)
    {
        _failAfter = failAfter;
    }

    public int Read(Span<byte> buffer)
    {
        if (_position >= _failAfter)
            throw new IOException("source failed");

        int count = (int)Math.Min(buffer.Length, _failAfter - _position);
        buffer.Slice(0, count).Fill(7);
        _position += count;
        return count;
    }

    public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        try
        {
            return new ValueTask<int>(Read(buffer.Span));
        }
        catch (Exception e)
        {
            return ValueTask.FromException<int>(e);
        }
    }

    public void Dispose() => Interlocked.Increment(ref DisposeCount);
}

public static class Http
{
    public static IPCResponse Text(string body, int statusCode = 200) => new()
    {
        StatusCode = statusCode,
        StatusText = "OK",
        Headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase)
        {
            ["Content-Type"] = new List<string> { "text/plain" }
        },
        Body = Encoding.UTF8.GetBytes(body)
    };

    public static IPCResponse Stream(IDataSource source, long? length = null)
    {
        var headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase)
        {
            ["Content-Type"] = new List<string> { "application/octet-stream" }
        };
        if (length != null)
            headers["Content-Length"] = new List<string> { length.Value.ToString() };

        return new IPCResponse
        {
            StatusCode = 200,
            StatusText = "OK",
            Headers = headers,
            DataSource = source
        };
    }

    public static string Sha256Hex(byte[] data) => Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
}
