using System.Buffers.Binary;
using System.Collections.Concurrent;

namespace FakeNative;

public sealed class Logger
{
    private readonly object _lock = new();
    private readonly StreamWriter? _writer;

    public Logger(string? path)
    {
        if (string.IsNullOrEmpty(path))
            return;
        var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
        _writer = new StreamWriter(stream) { AutoFlush = true };
    }

    public bool Enabled => _writer != null;

    public void Write(string message)
    {
        if (_writer == null)
            return;
        var thread = Thread.CurrentThread.Name ?? Environment.CurrentManagedThreadId.ToString();
        lock (_lock)
        {
            try
            {
                _writer.WriteLine($"{DateTime.Now:HH:mm:ss.fff} [{Environment.ProcessId}/{thread}] {message}");
            }
            catch (IOException)
            {
            }
        }
    }
}

public sealed class EventLoop
{
    private readonly BlockingCollection<Action> _queue = new();
    private readonly HashSet<Timer> _timers = new();
    private readonly Logger _log;
    private readonly Action<Exception> _onError;

    public EventLoop(Logger log, Action<Exception> onError)
    {
        _log = log;
        _onError = onError;
    }

    public int ThreadId { get; private set; }

    public void Post(Action action)
    {
        try
        {
            _queue.Add(action);
        }
        catch (InvalidOperationException)
        {
        }
    }

    public Timer PostDelayed(int milliseconds, Action action)
    {
        Timer? timer = null;
        timer = new Timer(_ =>
        {
            lock (_timers)
                _timers.Remove(timer!);
            Post(action);
        });
        lock (_timers)
            _timers.Add(timer);
        timer.Change(Math.Max(milliseconds, 0), Timeout.Infinite);
        return timer;
    }

    public void CancelTimer(Timer? timer)
    {
        if (timer == null)
            return;
        lock (_timers)
            _timers.Remove(timer);
        timer.Dispose();
    }

    public void Run()
    {
        ThreadId = Environment.CurrentManagedThreadId;
        foreach (var action in _queue.GetConsumingEnumerable())
        {
            try
            {
                action();
            }
            catch (Exception e)
            {
                _log.Write($"loop error: {e}");
                _onError(e);
            }
        }
    }
}

public sealed class PipeReaderThread
{
    private readonly Stream _input;
    private readonly Action<Packet> _onPacket;
    private readonly Action<string> _onFatal;
    private readonly Action _onEof;
    private readonly Func<bool> _stopped;

    public PipeReaderThread(Stream input, Action<Packet> onPacket, Action<string> onFatal, Action onEof, Func<bool> stopped)
    {
        _input = input;
        _onPacket = onPacket;
        _onFatal = onFatal;
        _onEof = onEof;
        _stopped = stopped;
    }

    public void Start()
    {
        new Thread(Run) { IsBackground = true, Name = "reader" }.Start();
    }

    private int ReadFull(byte[] buffer, int count)
    {
        var total = 0;
        while (total < count)
        {
            var n = _input.Read(buffer, total, count - total);
            if (n <= 0)
                break;
            total += n;
        }
        return total;
    }

    private void Run()
    {
        var header = new byte[Protocol.HeaderSize];
        try
        {
            while (true)
            {
                var got = ReadFull(header, header.Length);
                if (got == 0)
                {
                    _onEof();
                    return;
                }
                if (got < header.Length)
                {
                    _onFatal($"pipe closed inside a packet header ({got} of {header.Length} bytes)");
                    return;
                }
                var size = BinaryPrimitives.ReadUInt32LittleEndian(header);
                if (size < Protocol.HeaderSize - 4)
                {
                    _onFatal($"packet size {size} is smaller than the header");
                    return;
                }
                var bodyLength = (long)size - (Protocol.HeaderSize - 4);
                if (bodyLength > Protocol.MaxBody)
                {
                    _onFatal($"packet body of {bodyLength} bytes exceeds the {Protocol.MaxBody} byte limit");
                    return;
                }
                var body = bodyLength == 0 ? Array.Empty<byte>() : new byte[bodyLength];
                if (ReadFull(body, body.Length) < body.Length)
                {
                    _onFatal($"pipe closed inside a packet body of {bodyLength} bytes");
                    return;
                }
                if (_stopped())
                {
                    Thread.Sleep(Timeout.Infinite);
                    return;
                }
                var requestId = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
                _onPacket(new Packet(requestId, header[8], header[9], body));
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
            _onEof();
        }
    }
}

public sealed class PacketWriterThread
{
    private readonly Stream _output;
    private readonly Logger _log;
    private readonly BlockingCollection<ReadOnlyMemory<byte>?> _queue = new();
    private readonly ManualResetEventSlim _closed = new(false);
    private volatile bool _failed;

    public PacketWriterThread(Stream output, Logger log)
    {
        _output = output;
        _log = log;
    }

    public event Action<string>? Failed;

    public void Start()
    {
        new Thread(Run) { IsBackground = true, Name = "writer" }.Start();
    }

    public void Enqueue(ReadOnlyMemory<byte> packet)
    {
        try
        {
            _queue.Add(packet);
        }
        catch (InvalidOperationException)
        {
        }
    }

    public void Close()
    {
        try
        {
            _queue.Add(null);
            _queue.CompleteAdding();
        }
        catch (InvalidOperationException)
        {
        }
    }

    public bool WaitClosed(TimeSpan timeout) => _closed.Wait(timeout);

    private void Run()
    {
        foreach (var item in _queue.GetConsumingEnumerable())
        {
            if (item == null)
            {
                try
                {
                    _output.Dispose();
                }
                catch (IOException)
                {
                }
                _closed.Set();
                return;
            }
            if (_failed)
                continue;
            try
            {
                _output.Write(item.Value.Span);
                _output.Flush();
            }
            catch (Exception e) when (e is IOException or ObjectDisposedException)
            {
                _failed = true;
                _log.Write($"write failed: {e.Message}");
                Failed?.Invoke(e.Message);
            }
        }
        _closed.Set();
    }
}
