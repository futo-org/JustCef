namespace JustCef;

public sealed class DataStream : IDisposable
{
    private readonly uint _identifier;
    private readonly Action<uint, bool>? _onDispose;
    private readonly byte[] _buffer;
    private readonly object _stateLock = new();

    private TaskCompletionSource? _dataAvailableSignal;
    private int _head;
    private int _tail;
    private int _size;
    private bool _writerClosed;
    private bool _disposed;

    public DataStream(int bufferSize = 10 * 1024 * 1024)
        : this(0, null, bufferSize)
    {
    }

    internal DataStream(uint identifier, Action<uint, bool>? onDispose, int bufferSize = 10 * 1024 * 1024)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(bufferSize);

        _identifier = identifier;
        _onDispose = onDispose;
        _buffer = new byte[bufferSize];
    }

    internal bool WriteData(byte[] data, int offset, int length)
    {
        ArgumentNullException.ThrowIfNull(data);
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        ArgumentOutOfRangeException.ThrowIfNegative(length);

        if (offset > data.Length - length)
            throw new ArgumentOutOfRangeException(nameof(length));

        if (length == 0)
            return true;

        bool wroteAny = false;
        while (length > 0)
        {
            lock (_stateLock)
            {
                while (_size == _buffer.Length && !_writerClosed && !_disposed)
                    Monitor.Wait(_stateLock);

                if (_writerClosed || _disposed)
                    return wroteAny;

                int bytesWritten = WriteAvailable(data.AsSpan(offset, length));
                wroteAny |= bytesWritten > 0;
                offset += bytesWritten;
                length -= bytesWritten;
            }
        }

        return true;
    }

    public void Write(byte[] data, int offset, int length)
    {
        if (!WriteData(data, offset, length))
            throw new InvalidOperationException("Data stream is closed.");
    }

    public void Close()
    {
        lock (_stateLock)
        {
            if (_writerClosed || _disposed)
                return;

            _writerClosed = true;
            SignalDataAvailableNoLock();
            Monitor.PulseAll(_stateLock);
        }
    }

    internal void CloseFromRemote()
        => Close();

    public int Read(byte[] buffer, int offset, int count)
        => Read(buffer.AsSpan(offset, count));

    public int Read(Span<byte> buffer)
    {
        if (buffer.Length == 0)
            return 0;

        int bytesRead = 0;
        while (bytesRead < buffer.Length)
        {
            lock (_stateLock)
            {
                while (_size == 0 && !_writerClosed && !_disposed)
                    Monitor.Wait(_stateLock);

                if (_size == 0 && (_writerClosed || _disposed))
                    break;

                bytesRead += ReadAvailable(buffer[bytesRead..]);
                if (_size != 0)
                    break;
            }
        }

        return bytesRead;
    }

    public Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken = default)
        => ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

    public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (buffer.Length == 0)
            return 0;

        int bytesRead = 0;
        while (bytesRead < buffer.Length)
        {
            await WaitForDataAsync(cancellationToken).ConfigureAwait(false);

            lock (_stateLock)
            {
                if (_size == 0)
                {
                    if (_writerClosed || _disposed)
                        break;

                    continue;
                }

                bytesRead += ReadAvailable(buffer.Span[bytesRead..]);
                if (_size != 0)
                    break;
            }
        }

        return bytesRead;
    }

    public void Dispose()
    {
        bool notifyRemote = false;

        lock (_stateLock)
        {
            if (_disposed)
                return;

            _disposed = true;
            _head = 0;
            _tail = 0;
            _size = 0;
            notifyRemote = !_writerClosed;
            SignalDataAvailableNoLock();
            Monitor.PulseAll(_stateLock);
        }

        _onDispose?.Invoke(_identifier, notifyRemote);
    }

    private async ValueTask WaitForDataAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            Task waitTask;
            lock (_stateLock)
            {
                if (_size > 0 || _writerClosed || _disposed)
                    return;

                _dataAvailableSignal ??= CreateSignal();
                waitTask = _dataAvailableSignal.Task;
            }

            await waitTask.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    private int WriteAvailable(ReadOnlySpan<byte> source)
    {
        int bytesToWrite = Math.Min(_buffer.Length - _size, source.Length);
        int firstPart = Math.Min(bytesToWrite, _buffer.Length - _tail);

        source[..firstPart].CopyTo(_buffer.AsSpan(_tail, firstPart));
        _tail = (_tail + firstPart) % _buffer.Length;
        _size += firstPart;

        if (firstPart < bytesToWrite)
        {
            int secondPart = bytesToWrite - firstPart;
            source.Slice(firstPart, secondPart).CopyTo(_buffer.AsSpan(_tail, secondPart));
            _tail = (_tail + secondPart) % _buffer.Length;
            _size += secondPart;
        }

        SignalDataAvailableNoLock();
        Monitor.PulseAll(_stateLock);
        return bytesToWrite;
    }

    private int ReadAvailable(Span<byte> destination)
    {
        int bytesToRead = Math.Min(_size, destination.Length);
        int firstPart = Math.Min(bytesToRead, _buffer.Length - _head);

        _buffer.AsSpan(_head, firstPart).CopyTo(destination);
        _head = (_head + firstPart) % _buffer.Length;
        _size -= firstPart;

        if (firstPart < bytesToRead)
        {
            int secondPart = bytesToRead - firstPart;
            _buffer.AsSpan(_head, secondPart).CopyTo(destination[firstPart..]);
            _head = (_head + secondPart) % _buffer.Length;
            _size -= secondPart;
        }

        Monitor.PulseAll(_stateLock);
        return bytesToRead;
    }

    private void SignalDataAvailableNoLock()
    {
        _dataAvailableSignal?.TrySetResult();
        _dataAvailableSignal = null;
    }

    private static TaskCompletionSource CreateSignal()
        => new(TaskCreationOptions.RunContinuationsAsynchronously);
}
