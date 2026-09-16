using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization.Metadata;

namespace FakeNative;

public sealed class ControlChannel
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        TypeInfoResolver = new DefaultJsonTypeInfoResolver()
    };

    private readonly NetworkStream _stream;
    private readonly Logger _log;
    private readonly BlockingCollection<object> _queue = new();

    private ControlChannel(TcpClient client, Logger log)
    {
        _stream = client.GetStream();
        _log = log;
        new Thread(WriteLoop) { IsBackground = true, Name = "control-writer" }.Start();
    }

    public static ControlChannel Connect(int port, Logger log, TimeSpan timeout)
    {
        var client = new TcpClient { NoDelay = true };
        var connect = client.ConnectAsync(IPAddress.Loopback, port);
        if (!connect.Wait(timeout))
        {
            client.Dispose();
            throw new TimeoutException($"control connection to 127.0.0.1:{port} timed out");
        }
        return new ControlChannel(client, log);
    }

    public void Send(JsonObject evt)
    {
        var line = evt.ToJsonString(JsonOptions);
        try
        {
            _queue.Add(line);
        }
        catch (InvalidOperationException)
        {
        }
    }

    public bool Flush(TimeSpan timeout)
    {
        var done = new ManualResetEventSlim(false);
        try
        {
            _queue.Add(done);
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        return done.Wait(timeout);
    }

    public void StartReading(Action<string> onLine, Action onClosed)
    {
        new Thread(() =>
        {
            try
            {
                using var reader = new StreamReader(_stream, new UTF8Encoding(false), false, 65536, true);
                while (true)
                {
                    var line = reader.ReadLine();
                    if (line == null)
                        break;
                    if (line.Length > 0)
                        onLine(line);
                }
            }
            catch (Exception e) when (e is IOException or ObjectDisposedException or SocketException)
            {
                _log.Write($"control read failed: {e.Message}");
            }
            onClosed();
        }) { IsBackground = true, Name = "control-reader" }.Start();
    }

    private void WriteLoop()
    {
        var failed = false;
        foreach (var item in _queue.GetConsumingEnumerable())
        {
            if (item is ManualResetEventSlim done)
            {
                done.Set();
                continue;
            }
            if (failed)
                continue;
            try
            {
                var bytes = Encoding.UTF8.GetBytes((string)item + "\n");
                _stream.Write(bytes, 0, bytes.Length);
            }
            catch (Exception e) when (e is IOException or ObjectDisposedException or SocketException)
            {
                failed = true;
                _log.Write($"control write failed: {e.Message}");
            }
        }
    }
}
