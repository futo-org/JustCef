using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace FakeNative;

public sealed class BrowserState
{
    public int Id;
    public int ParentId;
    public string? Url;
    public string? Title;
    public bool Closed;
    public int X;
    public int Y;
    public int Width = 800;
    public int Height = 600;
    public double Zoom;
}

public sealed class Behavior
{
    public string Kind = "ok";
    public string? Message;
    public Status Status = Status.Error;
    public int? PayloadSize;
    public int DelayMs;
    public JsonArray? AfterReply;
    public JsonNode? Tag;
}

public sealed class InboundCall
{
    public uint RequestId;
    public byte Opcode;
    public required ControllerRequest Request;
    public Behavior? Behavior;
    public Timer? Timer;
    public bool Held;
    public bool Answered;
}

public sealed class OutgoingCall
{
    public uint RequestId;
    public byte Opcode;
    public long Sequence;
    public JsonNode? Tag;
    public bool Canceled;
    public required string EventName;
    public required Action<OutgoingCall, ResponseHead> OnResponse;
}

public enum StreamFinish
{
    Ended,
    Errored,
    Canceled
}

public sealed class StreamState
{
    public uint Id;
    public uint RequestId;
    public JsonNode? Tag;
    public long ReportedLength;
    public long Received;
    public long Buffered;
    public long Consumed;
    public long Granted;
    public long? CancelAfter;
    public int ReadDelayMs;
    public bool Ended;
    public ulong? TotalReported;
    public bool Busy;
    public bool Done;
    public bool CreditViolation;
    public Timer? Timer;
    public readonly IncrementalHash Sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
}

public sealed class Result
{
    public Status Status = Status.Ok;
    public string? Message;
    public Action<PacketBuilder>? Payload;
    public int Capacity = 16;
    public Action? FollowUp;
    public int? NewBrowserId;

    public static Result Ok(Action<PacketBuilder>? payload = null, int capacity = 16) => new() { Payload = payload, Capacity = capacity };
    public static Result Fail(Status status, string? message) => new() { Status = status, Message = message };
}

public sealed class Host
{
    private static readonly string[] BehaviorKinds = { "ok", "hold", "error", "notFound", "status" };

    private readonly Options _options;
    private readonly EventLoop _loop;
    private readonly PacketWriterThread _writer;
    private readonly ControlChannel? _control;
    private readonly Logger _log;
    private readonly Stream _input;

    private readonly SortedDictionary<int, BrowserState> _browsers = new();
    private int _nextBrowserId = 1;
    private readonly Dictionary<byte, Behavior> _behaviors = new();
    private readonly Dictionary<uint, InboundCall> _inbound = new();
    private readonly List<JsonObject> _cancels = new();
    private readonly Dictionary<uint, OutgoingCall> _outgoing = new();
    private uint _nextRequestId;
    private long _sequence;
    private readonly Dictionary<uint, StreamState> _streams = new();
    private readonly Dictionary<uint, StreamFinish> _finishedStreams = new();
    private bool _trace;
    private volatile bool _stopReading;
    private bool _shuttingDown;
    private int _violations;

    public Host(Options options, EventLoop loop, Stream input, PacketWriterThread writer, ControlChannel? control, Logger log)
    {
        _options = options;
        _loop = loop;
        _input = input;
        _writer = writer;
        _control = control;
        _log = log;
        _writer.Failed += detail => _loop.Post(() => Emit(new JsonObject { ["event"] = "writeFailed", ["detail"] = detail }));
    }

    public void Start()
    {
        Emit(new JsonObject { ["event"] = "hello", ["pid"] = Environment.ProcessId });
        _writer.Start();
        if (!_options.NoReady)
            Send(Encode.Ready(_options.ReadyVersion));
        new PipeReaderThread(_input,
            p => _loop.Post(() => OnPacket(p)),
            msg => _loop.Post(() => OnFatal(msg)),
            () => _loop.Post(OnEof),
            () => _stopReading).Start();
        _control?.StartReading(line => _loop.Post(() => OnControlLine(line)), () => _loop.Post(OnControlClosed));
    }

    public void OnLoopError(Exception e)
    {
        Emit(new JsonObject { ["event"] = "error", ["detail"] = e.ToString() });
    }

    private void Emit(JsonObject evt, JsonNode? tag = null)
    {
        if (tag != null)
            evt["tag"] = tag.DeepClone();
        if (_log.Enabled)
            _log.Write($"event {evt.ToJsonString()}");
        _control?.Send(evt);
    }

    private void Violation(string detail)
    {
        _violations++;
        Emit(new JsonObject { ["event"] = "violation", ["detail"] = detail });
    }

    private void Send(ReadOnlyMemory<byte> packet)
    {
        if (_log.Enabled)
            _log.Write($"-> {DescribeOutgoing(packet.Span)}");
        _writer.Enqueue(packet);
    }

    private static string TypeName(byte type) => type switch
    {
        0 => "Request",
        1 => "Response",
        2 => "Notification",
        3 => "Cancel",
        _ => $"Type{type}"
    };

    private static string EnumName<T>(byte value) where T : struct, Enum
    {
        var e = (T)Enum.ToObject(typeof(T), value);
        return Enum.IsDefined(e) ? e.ToString() : $"op{value}";
    }

    private static string IncomingOpName(byte type, byte opcode) => type switch
    {
        0 or 3 => EnumName<ControllerOp>(opcode),
        1 => EnumName<ClientOp>(opcode),
        2 => EnumName<ControllerNotification>(opcode),
        _ => $"op{opcode}"
    };

    private static string OutgoingOpName(byte type, byte opcode) => type switch
    {
        0 or 3 => EnumName<ClientOp>(opcode),
        1 => EnumName<ControllerOp>(opcode),
        2 => EnumName<ClientNotification>(opcode),
        _ => $"op{opcode}"
    };

    private static string DescribeOutgoing(ReadOnlySpan<byte> packet)
    {
        if (packet.Length < Protocol.HeaderSize)
            return $"raw {packet.Length} bytes";
        var size = BinaryPrimitives.ReadUInt32LittleEndian(packet);
        var rid = BinaryPrimitives.ReadUInt32LittleEndian(packet[4..]);
        var text = $"{TypeName(packet[8])} {OutgoingOpName(packet[8], packet[9])} id={rid} body={(long)size - 6}";
        if (packet[8] == 1 && packet.Length > Protocol.HeaderSize)
            text += $" status={(Status)packet[10]}";
        return text;
    }

    private void OnPacket(Packet p)
    {
        if (_stopReading || _shuttingDown)
            return;
        if (_log.Enabled)
            _log.Write($"<- {TypeName(p.Type)} {IncomingOpName(p.Type, p.Opcode)} id={p.RequestId} body={p.Body.Length}");
        if (_trace)
        {
            var evt = new JsonObject
            {
                ["event"] = "packet",
                ["type"] = TypeName(p.Type),
                ["opcode"] = p.Opcode,
                ["requestId"] = p.RequestId,
                ["size"] = p.Body.Length
            };
            if (p.Type == 1 && p.Body.Length > 0)
                evt["status"] = p.Body[0];
            Emit(evt);
        }
        switch (p.Type)
        {
            case (byte)PacketType.Request:
                HandleRequest(p);
                break;
            case (byte)PacketType.Response:
                HandleResponse(p);
                break;
            case (byte)PacketType.Notification:
                HandleNotification(p);
                break;
            case (byte)PacketType.Cancel:
                HandleCancel(p);
                break;
            default:
                Violation($"unknown packet type {p.Type} (opcode {p.Opcode}, requestId {p.RequestId})");
                break;
        }
    }

    private void HandleRequest(Packet p)
    {
        var rid = p.RequestId;
        var op = p.Opcode;
        var name = IncomingOpName(p.Type, op);
        if (rid == 0)
        {
            Violation($"request {name} uses requestId 0");
            return;
        }
        if (_inbound.ContainsKey(rid))
            Violation($"request {name} reuses requestId {rid} while it is still in flight");
        if (!Codec.IsKnownControllerOp(op))
        {
            Send(Encode.Response(rid, op, Status.Unsupported, $"unknown opcode {op}"));
            return;
        }
        ControllerRequest req;
        try
        {
            req = Codec.ParseControllerRequest(op, p.Body);
        }
        catch (ProtocolException e)
        {
            Violation($"malformed {name} request {rid}: {e.Message}");
            Send(Encode.Response(rid, op, Status.InvalidRequest, e.Message));
            return;
        }
        _behaviors.TryGetValue(op, out var behavior);
        var call = new InboundCall { RequestId = rid, Opcode = op, Request = req, Behavior = behavior };
        if (behavior is { Kind: "hold" })
        {
            call.Held = true;
            _inbound[rid] = call;
            Emit(new JsonObject { ["event"] = "held", ["requestId"] = rid, ["opcode"] = op }, behavior.Tag);
            return;
        }
        if (behavior is { DelayMs: > 0 })
        {
            _inbound[rid] = call;
            call.Timer = _loop.PostDelayed(behavior.DelayMs, () =>
            {
                if (!call.Answered)
                    Execute(call);
            });
            return;
        }
        Execute(call);
    }

    private void Execute(InboundCall call)
    {
        var b = call.Behavior;
        var result = b?.Kind switch
        {
            "error" => Result.Fail(Status.Error, b.Message ?? "configured error"),
            "notFound" => Result.Fail(Status.NotFound, b.Message),
            "status" => Result.Fail(b.Status, b.Message),
            _ => HandleDefault(call.Request, b?.PayloadSize)
        };
        Complete(call, result);
    }

    private void Complete(InboundCall call, Result result)
    {
        if (call.Answered)
            return;
        call.Answered = true;
        if (_inbound.TryGetValue(call.RequestId, out var current) && current == call)
            _inbound.Remove(call.RequestId);
        _loop.CancelTimer(call.Timer);
        if (result.Status == Status.Ok && result.Capacity > Protocol.MaxBody - 1)
            result = Result.Fail(Status.TooLarge, null);
        if (result.Status == Status.Ok)
            Send(Encode.ResponseOk(call.RequestId, call.Opcode, result.Payload, result.Capacity));
        else
            Send(Encode.Response(call.RequestId, call.Opcode, result.Status, result.Message));
        if (call.Behavior?.AfterReply is JsonArray actions)
            RunActions(actions, call.Behavior.Tag, result.NewBrowserId);
        result.FollowUp?.Invoke();
    }

    private bool TryGetBrowser(int id, out BrowserState browser)
    {
        if (_browsers.TryGetValue(id, out var b) && !b.Closed)
        {
            browser = b;
            return true;
        }
        browser = null!;
        return false;
    }

    private BrowserState TopLevel(BrowserState browser)
    {
        var current = browser;
        for (var depth = 0; depth < 64 && current.ParentId != 0 && _browsers.TryGetValue(current.ParentId, out var parent); depth++)
            current = parent;
        return current;
    }

    private Result HandleDefault(ControllerRequest req, int? payloadSize)
    {
        if (payloadSize is int size && size > Protocol.MaxBody - 64)
            return Result.Fail(Status.TooLarge, null);
        switch (req.Op)
        {
            case ControllerOp.Ping:
                return Result.Ok();
            case ControllerOp.Print:
            {
                var text = Encoding.UTF8.GetString(req.Data.Span);
                Emit(new JsonObject { ["event"] = "print", ["text"] = text });
                return Result.Ok();
            }
            case ControllerOp.Echo:
            {
                var data = payloadSize is int n ? Pattern(n) : req.Data;
                return Result.Ok(b => b.Raw(data.Span), data.Length);
            }
            case ControllerOp.WindowCreate:
                return CreateWindow(req.Create!);
            case ControllerOp.GetWidevineStatus:
                return Result.Ok(b => b.I32(0).Str(null).Bool(false).Bool(false).Bool(false));
            case ControllerOp.Debug:
                return Result.Fail(Status.Unsupported, "Debug requires --enable-ipc-debug");
        }

        var id = req.BrowserId!.Value;
        if (!TryGetBrowser(id, out var browser))
            return Result.Fail(Status.NotFound, $"browser {id} does not exist");
        var window = TopLevel(browser);
        switch (req.Op)
        {
            case ControllerOp.WindowLoadUrl:
            {
                var url = req.Text;
                browser.Url = url;
                var result = Result.Ok();
                result.FollowUp = () => SendLoadSequence(id, url);
                return result;
            }
            case ControllerOp.WindowSetZoom:
                browser.Zoom = req.Level;
                return Result.Ok();
            case ControllerOp.WindowGetZoom:
            {
                var zoom = browser.Zoom;
                return Result.Ok(b => b.F64(zoom), 8);
            }
            case ControllerOp.WindowGetPosition:
            {
                var (x, y) = (window.X, window.Y);
                return Result.Ok(b => b.I32(x).I32(y), 8);
            }
            case ControllerOp.WindowSetPosition:
                window.X = req.X;
                window.Y = req.Y;
                return Result.Ok();
            case ControllerOp.WindowGetSize:
            {
                var (w, h) = (window.Width, window.Height);
                return Result.Ok(b => b.I32(w).I32(h), 8);
            }
            case ControllerOp.WindowSetSize:
                window.Width = req.X;
                window.Height = req.Y;
                return Result.Ok();
            case ControllerOp.WindowSetTitle:
                window.Title = req.Text;
                return Result.Ok();
            case ControllerOp.WindowClose:
            {
                var result = Result.Ok();
                result.FollowUp = () => CloseBrowser(browser);
                return result;
            }
            case ControllerOp.PickFile:
                return Result.Ok(b => b.U32(0));
            case ControllerOp.PickDirectory:
            case ControllerOp.SaveFile:
                return Result.Ok(b => b.Str(""));
            case ControllerOp.WindowExecuteDevToolsMethod:
            {
                var json = payloadSize is int n ? JsonStringBytes(n) : "{}"u8.ToArray();
                return Result.Ok(b => b.Bool(true).Bytes(json), json.Length + 8);
            }
            case ControllerOp.WindowBridgeRpc:
            {
                var json = payloadSize is int n ? JsonStringBytes(n) : "null"u8.ToArray();
                return Result.Ok(b => b.Bytes(json), json.Length + 8);
            }
            default:
                return Result.Ok();
        }
    }

    private Result CreateWindow(WindowCreateArgs args)
    {
        var id = _nextBrowserId++;
        while (_browsers.ContainsKey(id))
            id = _nextBrowserId++;
        _browsers[id] = new BrowserState { Id = id, Url = args.Url, Title = args.Title };
        var url = args.Url;
        var result = Result.Ok(b => b.I32(id), 4);
        result.NewBrowserId = id;
        result.FollowUp = () =>
        {
            Send(Encode.WindowOpened(id));
            if (!string.IsNullOrEmpty(url))
                SendLoadSequence(id, url);
        };
        return result;
    }

    private void SendLoadSequence(int id, string? url)
    {
        Send(Encode.LoadingStateChanged(id, true, false, false));
        Send(Encode.FrameLoadStart(id, "main", true, url));
        Send(Encode.FrameLoadEnd(id, "main", true, url, 200));
        Send(Encode.LoadingStateChanged(id, false, false, false));
    }

    private void CloseBrowser(BrowserState browser)
    {
        if (browser.Closed)
            return;
        foreach (var child in _browsers.Values.Where(b => b.ParentId == browser.Id && !b.Closed).ToList())
            CloseBrowser(child);
        browser.Closed = true;
        Send(Encode.WindowClosed(browser.Id));
    }

    private void HandleCancel(Packet p)
    {
        var rid = p.RequestId;
        if (p.Body.Length != 0)
            Violation($"cancel for request {rid} carries {p.Body.Length} body bytes");
        if (rid == 0)
            Violation("cancel uses requestId 0");
        var matched = _inbound.TryGetValue(rid, out var call);
        if (matched && call!.Opcode != p.Opcode)
        {
            Violation($"cancel names request {rid} with opcode {p.Opcode}, but that request has opcode {call.Opcode}");
            matched = false;
        }
        _cancels.Add(new JsonObject { ["requestId"] = rid, ["opcode"] = p.Opcode, ["matched"] = matched });
        var evt = new JsonObject
        {
            ["event"] = "cancelReceived",
            ["requestId"] = rid,
            ["opcode"] = p.Opcode,
            ["matched"] = matched,
            ["held"] = matched && call!.Held
        };
        Emit(evt, matched ? call!.Behavior?.Tag : null);
        if (matched)
            Complete(call!, Result.Fail(Status.Canceled, null));
    }

    private void HandleNotification(Packet p)
    {
        var name = IncomingOpName(p.Type, p.Opcode);
        if (p.RequestId != 0)
            Violation($"notification {name} uses requestId {p.RequestId}");
        ControllerNotificationData n;
        try
        {
            n = Codec.ParseControllerNotification(p.Opcode, p.Body);
        }
        catch (ProtocolException e)
        {
            Violation($"malformed notification {name}: {e.Message}");
            return;
        }
        switch (n.Op)
        {
            case ControllerNotification.Exit:
                Shutdown("Exit notification");
                break;
            case ControllerNotification.StreamData:
                OnStreamData(n);
                break;
            case ControllerNotification.StreamEnd:
                OnStreamEnd(n);
                break;
            case ControllerNotification.StreamError:
                OnStreamError(n);
                break;
        }
    }

    private void HandleResponse(Packet p)
    {
        var rid = p.RequestId;
        if (!_outgoing.Remove(rid, out var call))
        {
            Violation($"response {IncomingOpName(p.Type, p.Opcode)} for unknown requestId {rid}");
            return;
        }
        if (call.Opcode != p.Opcode)
            Violation($"response for request {rid} has opcode {p.Opcode}, expected {call.Opcode}");
        ResponseHead? head = null;
        try
        {
            head = Codec.ParseResponseHead(p.Body);
            call.OnResponse(call, head);
        }
        catch (ProtocolException e)
        {
            Violation($"malformed {EnumName<ClientOp>(call.Opcode)} response {rid}: {e.Message}");
            var evt = new JsonObject
            {
                ["event"] = call.EventName,
                ["requestId"] = rid,
                ["status"] = head == null ? -1 : (int)head.Status,
                ["malformed"] = e.Message
            };
            Emit(evt, call.Tag);
        }
    }

    private uint NextRequestId()
    {
        do
        {
            _nextRequestId++;
        } while (_nextRequestId == 0 || _outgoing.ContainsKey(_nextRequestId));
        return _nextRequestId;
    }

    private void StartCall(ClientOp op, string eventName, JsonNode? tag, Func<uint, ReadOnlyMemory<byte>> build, Action<OutgoingCall, ResponseHead> onResponse)
    {
        var rid = NextRequestId();
        var call = new OutgoingCall
        {
            RequestId = rid,
            Opcode = (byte)op,
            Sequence = ++_sequence,
            Tag = tag?.DeepClone(),
            EventName = eventName,
            OnResponse = onResponse
        };
        _outgoing[rid] = call;
        Send(build(rid));
    }

    private static JsonObject ResponseEvent(OutgoingCall call, ResponseHead head)
    {
        var evt = new JsonObject
        {
            ["event"] = call.EventName,
            ["requestId"] = call.RequestId,
            ["status"] = (int)head.Status,
            ["message"] = head.Message
        };
        if (call.Canceled)
            evt["cancelSent"] = true;
        return evt;
    }

    private static JsonObject HeadersJson(List<HttpHeader> headers)
    {
        var obj = new JsonObject();
        foreach (var h in headers)
        {
            var key = h.Name ?? "";
            if (obj[key] is not JsonArray values)
            {
                values = new JsonArray();
                obj[key] = values;
            }
            values.Add(h.Value);
        }
        return obj;
    }

    private static string Sha256Hex(ReadOnlySpan<byte> data) => Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();

    private static ReadOnlyMemory<byte> Pattern(int size)
    {
        var data = new byte[Math.Max(size, 0)];
        for (var i = 0; i < data.Length; i++)
            data[i] = (byte)(i % 251);
        return data;
    }

    private static byte[] JsonStringBytes(int size)
    {
        var data = new byte[Math.Max(size, 2)];
        data.AsSpan().Fill((byte)'a');
        data[0] = (byte)'"';
        data[^1] = (byte)'"';
        return data;
    }

    private void OnProxyResponse(OutgoingCall call, ResponseHead head, long? cancelAfter, int readDelayMs)
    {
        var evt = ResponseEvent(call, head);
        if (head.Status != Status.Ok)
        {
            Emit(evt, call.Tag);
            return;
        }
        var r = Codec.ParseProxyResponse(head.Payload);
        evt["statusCode"] = r.StatusCode;
        evt["statusText"] = r.StatusText;
        evt["headers"] = HeadersJson(r.Headers);
        evt["bodyType"] = r.BodyType;
        evt["length"] = r.Length;
        if (r.BodyType == 1)
            evt["sha256"] = Sha256Hex(r.Data.Span);
        if (r.BodyType == 2)
            evt["streamId"] = r.StreamId;
        Emit(evt, call.Tag);
        if (r.BodyType == 2)
            OpenStream(call, r, cancelAfter, readDelayMs);
    }

    private void OpenStream(OutgoingCall call, ProxyResponse r, long? cancelAfter, int readDelayMs)
    {
        if (_streams.ContainsKey(r.StreamId))
        {
            Violation($"proxy response opens stream {r.StreamId}, which is already open");
            return;
        }
        _finishedStreams.Remove(r.StreamId);
        var s = new StreamState
        {
            Id = r.StreamId,
            RequestId = call.RequestId,
            Tag = call.Tag,
            ReportedLength = r.Length,
            CancelAfter = cancelAfter,
            ReadDelayMs = readDelayMs
        };
        _streams[s.Id] = s;
        if (call.Canceled || cancelAfter is <= 0)
            CancelStream(s);
    }

    private void OnStreamData(ControllerNotificationData n)
    {
        var length = n.Data.Length;
        if (length > Protocol.MaxStreamChunk)
            Violation($"StreamData for stream {n.StreamId} carries {length} bytes, more than {Protocol.MaxStreamChunk}");
        if (!_streams.TryGetValue(n.StreamId, out var s))
        {
            if (_finishedStreams.TryGetValue(n.StreamId, out var finish))
            {
                if (finish == StreamFinish.Canceled)
                    return;
                Violation($"StreamData for stream {n.StreamId} after it {(finish == StreamFinish.Ended ? "ended" : "failed")}");
            }
            else
            {
                Violation($"StreamData for unknown stream {n.StreamId}");
            }
            _finishedStreams[n.StreamId] = StreamFinish.Canceled;
            Send(Encode.StreamCancel(n.StreamId));
            return;
        }
        if (s.Ended)
            Violation($"StreamData for stream {s.Id} after StreamEnd");
        s.Received += length;
        s.Buffered += length;
        s.Sha.AppendData(n.Data.Span);
        var outstanding = s.Received - s.Granted;
        if (outstanding > Protocol.InitialCredit && !s.CreditViolation)
        {
            s.CreditViolation = true;
            Violation($"stream {s.Id} exceeded its credit: {outstanding} bytes outstanding");
            Emit(new JsonObject { ["event"] = "creditViolation", ["streamId"] = s.Id, ["outstanding"] = outstanding }, s.Tag);
        }
        if (s.CancelAfter is long limit && s.Received >= limit)
        {
            CancelStream(s);
            return;
        }
        Pump(s);
    }

    private void Pump(StreamState s)
    {
        if (s.Busy || s.Done)
            return;
        while (s.Buffered > 0)
        {
            var chunk = Math.Min(Protocol.CreditBatch, s.Buffered);
            s.Buffered -= chunk;
            s.Consumed += chunk;
            var ungranted = s.Consumed - s.Granted;
            if (!s.Ended && ungranted > 0 && (ungranted >= Protocol.CreditBatch || s.Buffered == 0))
            {
                s.Granted += ungranted;
                Send(Encode.StreamCredit(s.Id, (uint)ungranted));
            }
            if (s.ReadDelayMs > 0)
            {
                s.Busy = true;
                s.Timer = _loop.PostDelayed(s.ReadDelayMs, () =>
                {
                    s.Timer = null;
                    s.Busy = false;
                    Pump(s);
                });
                return;
            }
        }
        if (s.Ended)
            FinishStream(s, StreamFinish.Ended, null);
    }

    private void OnStreamEnd(ControllerNotificationData n)
    {
        if (!_streams.TryGetValue(n.StreamId, out var s))
        {
            ReportLateFinish("StreamEnd", n.StreamId);
            return;
        }
        if (s.Ended)
        {
            Violation($"duplicate StreamEnd for stream {s.Id}");
            return;
        }
        s.Ended = true;
        s.TotalReported = n.TotalBytes;
        if ((long)n.TotalBytes != s.Received)
            Violation($"StreamEnd for stream {s.Id} reports {n.TotalBytes} bytes, but {s.Received} arrived");
        if (s.ReportedLength >= 0 && s.ReportedLength != s.Received)
            Violation($"stream {s.Id} announced length {s.ReportedLength}, but {s.Received} bytes arrived");
        Pump(s);
    }

    private void OnStreamError(ControllerNotificationData n)
    {
        if (!_streams.TryGetValue(n.StreamId, out var s))
        {
            ReportLateFinish("StreamError", n.StreamId);
            return;
        }
        FinishStream(s, StreamFinish.Errored, n.Message);
    }

    private void ReportLateFinish(string what, uint streamId)
    {
        if (_finishedStreams.TryGetValue(streamId, out var finish))
        {
            if (finish != StreamFinish.Canceled)
                Violation($"{what} for stream {streamId}, which already finished");
            return;
        }
        Violation($"{what} for unknown stream {streamId}");
    }

    private void CancelStream(StreamState s)
    {
        Send(Encode.StreamCancel(s.Id));
        FinishStream(s, StreamFinish.Canceled, null);
    }

    private void FinishStream(StreamState s, StreamFinish finish, string? message)
    {
        if (s.Done)
            return;
        s.Done = true;
        _loop.CancelTimer(s.Timer);
        _streams.Remove(s.Id);
        _finishedStreams[s.Id] = finish;
        var evt = new JsonObject
        {
            ["event"] = "streamDone",
            ["streamId"] = s.Id,
            ["requestId"] = s.RequestId,
            ["bytes"] = s.Received,
            ["sha256"] = Convert.ToHexString(s.Sha.GetHashAndReset()).ToLowerInvariant(),
            ["result"] = finish switch
            {
                StreamFinish.Ended => "end",
                StreamFinish.Errored => "error",
                _ => "canceled"
            },
            ["message"] = message,
            ["totalReported"] = s.TotalReported is ulong total ? JsonValue.Create(total) : null,
            ["creditGranted"] = s.Granted
        };
        Emit(evt, s.Tag);
    }

    private void OnControlLine(string line)
    {
        JsonObject? cmd;
        try
        {
            cmd = JsonNode.Parse(line) as JsonObject;
        }
        catch (Exception e) when (e is System.Text.Json.JsonException or ArgumentException)
        {
            Emit(new JsonObject { ["event"] = "error", ["detail"] = $"invalid JSON: {e.Message}" });
            return;
        }
        if (cmd == null)
        {
            Emit(new JsonObject { ["event"] = "error", ["detail"] = "command is not a JSON object" });
            return;
        }
        var tag = cmd["tag"];
        string? name = null;
        try
        {
            name = Json.Str(cmd, "cmd");
            RunCommand(name, cmd, tag, null);
        }
        catch (Exception e) when (e is InvalidOperationException or FormatException or ArgumentException or OverflowException or KeyNotFoundException)
        {
            Emit(new JsonObject { ["event"] = "error", ["detail"] = $"{name}: {e.Message}" }, tag);
        }
    }

    private void OnControlClosed()
    {
        _log.Write("control channel closed");
        if (_stopReading)
            Terminate(0);
    }

    private void RunActions(JsonArray actions, JsonNode? tag, int? newBrowserId)
    {
        foreach (var node in actions)
        {
            if (node is not JsonObject action)
            {
                Emit(new JsonObject { ["event"] = "error", ["detail"] = "afterReply entry is not an object" }, tag);
                continue;
            }
            var actionTag = action["tag"] ?? tag;
            string? name = null;
            try
            {
                name = Json.Str(action, "action") ?? Json.Str(action, "cmd");
                RunCommand(name, action, actionTag, newBrowserId);
            }
            catch (Exception e) when (e is InvalidOperationException or FormatException or ArgumentException or OverflowException or KeyNotFoundException)
            {
                Emit(new JsonObject { ["event"] = "error", ["detail"] = $"{name}: {e.Message}" }, actionTag);
            }
        }
    }

    private void Ack(string name, JsonNode? tag) => Emit(new JsonObject { ["event"] = "ack", ["cmd"] = name }, tag);

    private void RunCommand(string? name, JsonObject o, JsonNode? tag, int? newId)
    {
        switch (name)
        {
            case "configure":
                Configure(o, tag);
                Ack(name, tag);
                break;
            case "notify":
                Notify(o, newId);
                Ack(name, tag);
                break;
            case "loadingBurst":
                LoadingBurst(o, tag, newId);
                break;
            case "proxyRequest":
                ProxyRequestCommand(o, tag, newId);
                break;
            case "modifyRequest":
                ModifyRequestCommand(o, tag, newId);
                break;
            case "bridgeRpc":
                BridgeRpcCommand(o, tag, newId);
                break;
            case "viewCreated":
                ViewCreatedCommand(o, tag, newId);
                break;
            case "echo":
                EchoCommand(o, tag);
                break;
            case "cancelLastRequest":
                CancelLastRequest(o, tag);
                break;
            case "exit":
                Shutdown("exit command");
                break;
            case "stopReading":
                _stopReading = true;
                Ack(name, tag);
                break;
            case "spawnGrandchildAndExit":
                SpawnGrandchildAndExit(o, tag);
                break;
            case "crash":
                _log.Write("crash command");
                Environment.Exit(3);
                break;
            case "trace":
                _trace = Json.Bool(o, "on", true);
                Ack(name, tag);
                break;
            case "sendRaw":
                Send(Convert.FromHexString(Json.Str(o, "hex") ?? ""));
                Ack(name, tag);
                break;
            case "state":
                Emit(StateEvent(), tag);
                break;
            default:
                Emit(new JsonObject { ["event"] = "error", ["detail"] = $"unknown command {name ?? "(missing)"}" }, tag);
                break;
        }
    }

    private void Configure(JsonObject o, JsonNode? tag)
    {
        var op = Json.Opcode<ControllerOp>(o, "opcode");
        var kind = Json.Str(o, "behavior") ?? "ok";
        if (kind is "default" or "reset")
        {
            _behaviors.Remove(op);
            return;
        }
        if (!BehaviorKinds.Contains(kind))
            throw new ArgumentException($"unknown behavior {kind}");
        var behavior = new Behavior
        {
            Kind = kind,
            Message = Json.Str(o, "message"),
            PayloadSize = o["payloadSize"] == null ? null : Json.Int(o, "payloadSize", 0),
            DelayMs = Json.Int(o, "delayMs", 0),
            AfterReply = o["afterReply"]?.DeepClone() as JsonArray,
            Tag = tag?.DeepClone()
        };
        if (kind == "status")
        {
            var status = Json.Int(o, "status", (int)Status.Error);
            if (status is <= 0 or > (int)Status.InvalidRequest)
                throw new ArgumentException($"status {status} is not a failure status");
            behavior.Status = (Status)status;
        }
        _behaviors[op] = behavior;
    }

    private void Notify(JsonObject o, int? newId)
    {
        var op = (ClientNotification)Json.Opcode<ClientNotification>(o, "opcode");
        var id = Json.Int(o, "browserId", 1, newId);
        ReadOnlyMemory<byte> packet;
        switch (op)
        {
            case ClientNotification.Ready:
                packet = Encode.Ready((uint)Json.Long(o, "version", Protocol.Version));
                break;
            case ClientNotification.Exit:
                packet = Encode.Exit();
                break;
            case ClientNotification.WindowOpened:
                if (!_browsers.TryGetValue(id, out var existing))
                    _browsers[id] = new BrowserState { Id = id, ParentId = Json.Int(o, "parentId", 0, newId) };
                else
                    existing.Closed = false;
                packet = Encode.WindowOpened(id);
                break;
            case ClientNotification.WindowClosed:
                if (_browsers.TryGetValue(id, out var closing))
                    closing.Closed = true;
                packet = Encode.WindowClosed(id);
                break;
            case ClientNotification.WindowFocused:
                packet = Encode.WindowFocused(id);
                break;
            case ClientNotification.WindowUnfocused:
                packet = Encode.WindowUnfocused(id);
                break;
            case ClientNotification.WindowFullscreenChanged:
                packet = Encode.FullscreenChanged(id, Json.Bool(o, "fullscreen", false));
                break;
            case ClientNotification.WindowFrameLoadStart:
                packet = Encode.FrameLoadStart(id, Json.Str(o, "frameId") ?? "main", Json.Bool(o, "isMain", true), Json.Str(o, "url") ?? "");
                break;
            case ClientNotification.WindowFrameLoadEnd:
                packet = Encode.FrameLoadEnd(id, Json.Str(o, "frameId") ?? "main", Json.Bool(o, "isMain", true), Json.Str(o, "url") ?? "", Json.Int(o, "httpStatusCode", 200));
                break;
            case ClientNotification.WindowFrameLoadError:
                packet = Encode.FrameLoadError(id, Json.Str(o, "frameId") ?? "main", Json.Bool(o, "isMain", true), Json.Int(o, "errorCode", -2), Json.Str(o, "errorText") ?? "net::ERR_FAILED", Json.Str(o, "url") ?? "");
                break;
            case ClientNotification.WindowDevToolsEvent:
            {
                var parameters = o["size"] != null ? JsonStringBytes(Json.Int(o, "size", 2)) : Encoding.UTF8.GetBytes(Json.Str(o, "params") ?? "{}");
                packet = Encode.DevToolsEvent(id, Json.Str(o, "method") ?? "Test.event", parameters);
                break;
            }
            case ClientNotification.WindowLoadingStateChanged:
                packet = Encode.LoadingStateChanged(id, Json.Bool(o, "isLoading", false), Json.Bool(o, "canGoBack", false), Json.Bool(o, "canGoForward", false));
                break;
            case ClientNotification.StreamCredit:
                packet = Encode.StreamCredit((uint)Json.Long(o, "streamId", 1), (uint)Json.Long(o, "bytes", Protocol.CreditBatch));
                break;
            case ClientNotification.StreamCancel:
                packet = Encode.StreamCancel((uint)Json.Long(o, "streamId", 1));
                break;
            case ClientNotification.Debug:
                packet = Encode.Debug((uint)Json.Long(o, "sequence", 0));
                break;
            default:
                throw new ArgumentException($"notify does not support opcode {(byte)op}");
        }
        Send(packet);
    }

    private void LoadingBurst(JsonObject o, JsonNode? tag, int? newId)
    {
        var ids = new List<int>();
        if (o["browserIds"] is JsonArray array)
        {
            foreach (var node in array)
                ids.Add(Json.IntValue(node, newId));
        }
        if (ids.Count == 0)
            throw new ArgumentException("browserIds is empty");
        var count = Json.Int(o, "count", 0);
        var perBrowser = new int[ids.Count];
        for (var i = 0; i < ids.Count; i++)
            perBrowser[i] = count / ids.Count + (i < count % ids.Count ? 1 : 0);
        var seen = new int[ids.Count];
        for (var k = 0; k < count; k++)
        {
            var i = k % ids.Count;
            var isLoading = (perBrowser[i] - 1 - seen[i]) % 2 == 1;
            seen[i]++;
            Send(Encode.LoadingStateChanged(ids[i], isLoading, false, false));
        }
        Emit(new JsonObject { ["event"] = "loadingBurstDone", ["sent"] = count }, tag);
    }

    private static HttpRequestData RequestFromJson(JsonObject o)
    {
        var req = new HttpRequestData
        {
            Method = Json.Str(o, "method") ?? "GET",
            Url = Json.Str(o, "url") ?? "https://fake.invalid/"
        };
        if (o["headers"] is JsonObject headers)
        {
            foreach (var (key, value) in headers)
            {
                if (value is JsonArray values)
                {
                    foreach (var v in values)
                        req.Headers.Add(new HttpHeader(key, v?.GetValue<string>()));
                }
                else
                {
                    req.Headers.Add(new HttpHeader(key, value?.GetValue<string>()));
                }
            }
        }
        var bodySize = Json.Int(o, "bodySize", 0);
        if (bodySize > 0)
            req.Elements.Add(new BodyElement { Type = 1, Data = Pattern(bodySize) });
        if (o["files"] is JsonArray files)
        {
            foreach (var f in files)
                req.Elements.Add(new BodyElement { Type = 2, Path = f?.GetValue<string>() });
        }
        return req;
    }

    private void ProxyRequestCommand(JsonObject o, JsonNode? tag, int? newId)
    {
        var browserId = Json.Int(o, "browserId", 1, newId);
        var req = RequestFromJson(o);
        long? cancelAfter = o["cancelAfterBytes"] == null ? null : Json.Long(o, "cancelAfterBytes", 0);
        var readDelayMs = Json.Int(o, "readDelayMs", 0);
        StartCall(ClientOp.WindowProxyRequest, "proxyResponse", tag,
            rid => Encode.ProxyRequest(rid, browserId, req),
            (call, head) => OnProxyResponse(call, head, cancelAfter, readDelayMs));
    }

    private void ModifyRequestCommand(JsonObject o, JsonNode? tag, int? newId)
    {
        var browserId = Json.Int(o, "browserId", 1, newId);
        var req = RequestFromJson(o);
        StartCall(ClientOp.WindowModifyRequest, "modifyResponse", tag,
            rid => Encode.ModifyRequest(rid, browserId, req),
            (call, head) =>
            {
                var evt = ResponseEvent(call, head);
                if (head.Status == Status.Ok)
                {
                    var m = Codec.ParseModifyResponse(head.Payload);
                    evt["method"] = m.Method;
                    evt["url"] = m.Url;
                    evt["headers"] = HeadersJson(m.Headers);
                    var elements = new JsonArray();
                    foreach (var e in m.Elements)
                    {
                        if (e.Type == 1)
                            elements.Add(new JsonObject { ["type"] = 1, ["size"] = e.Data.Length, ["sha256"] = Sha256Hex(e.Data.Span) });
                        else
                            elements.Add(new JsonObject { ["type"] = 2, ["path"] = e.Path });
                    }
                    evt["elements"] = elements;
                }
                Emit(evt, call.Tag);
            });
    }

    private void BridgeRpcCommand(JsonObject o, JsonNode? tag, int? newId)
    {
        var browserId = Json.Int(o, "browserId", 1, newId);
        var method = Json.Str(o, "method") ?? "test";
        var json = o["size"] != null ? JsonStringBytes(Json.Int(o, "size", 2)) : Encoding.UTF8.GetBytes(Json.Str(o, "json") ?? "null");
        StartCall(ClientOp.WindowBridgeRpc, "bridgeRpcResponse", tag,
            rid => Encode.BridgeRpc(rid, browserId, method, json),
            (call, head) =>
            {
                var evt = ResponseEvent(call, head);
                if (head.Status == Status.Ok)
                {
                    var result = Codec.ParseBridgeResponse(head.Payload);
                    var prefix = Encoding.UTF8.GetString(result.Span[..Math.Min(result.Length, 1024)]);
                    evt["jsonSize"] = result.Length;
                    evt["json"] = prefix.Length > 256 ? prefix[..256] : prefix;
                }
                Emit(evt, call.Tag);
            });
    }

    private void ViewCreatedCommand(JsonObject o, JsonNode? tag, int? newId)
    {
        var parentId = Json.Int(o, "parentId", 1, newId);
        var viewId = Json.Int(o, "viewId", 0, newId);
        var src = Json.Str(o, "src") ?? "";
        StartCall(ClientOp.WindowViewCreated, "viewCreatedResponse", tag,
            rid => Encode.ViewCreated(rid, parentId, viewId, src),
            (call, head) =>
            {
                var evt = ResponseEvent(call, head);
                var allow = head.Status == Status.Ok && Codec.ParseViewCreatedResponse(head.Payload);
                evt["allow"] = allow;
                if (allow)
                    _browsers[viewId] = new BrowserState { Id = viewId, ParentId = parentId, Url = src };
                Emit(evt, call.Tag);
            });
    }

    private void EchoCommand(JsonObject o, JsonNode? tag)
    {
        var size = Json.Int(o, "size", 0);
        var data = new byte[Math.Max(size, 0)];
        Random.Shared.NextBytes(data);
        StartCall(ClientOp.Echo, "echoResponse", tag,
            rid => Encode.EchoRequest(rid, data),
            (call, head) =>
            {
                var evt = ResponseEvent(call, head);
                evt["size"] = data.Length;
                if (head.Status == Status.Ok)
                {
                    var echoed = head.Payload.Rest();
                    evt["receivedSize"] = echoed.Length;
                    evt["match"] = echoed.Span.SequenceEqual(data);
                }
                else
                {
                    evt["match"] = false;
                }
                Emit(evt, call.Tag);
            });
    }

    private void CancelLastRequest(JsonObject o, JsonNode? tag)
    {
        var op = Json.Opcode<ClientOp>(o, "opcode");
        OutgoingCall? last = null;
        foreach (var call in _outgoing.Values)
        {
            if (call.Opcode == op && !call.Canceled && (last == null || call.Sequence > last.Sequence))
                last = call;
        }
        if (last == null)
        {
            Emit(new JsonObject { ["event"] = "error", ["detail"] = $"cancelLastRequest: no outstanding request with opcode {op}" }, tag);
            return;
        }
        last.Canceled = true;
        Send(Encode.Cancel(last.RequestId, op));
        Emit(new JsonObject { ["event"] = "cancelSent", ["requestId"] = last.RequestId, ["opcode"] = op }, tag);
    }

    private JsonObject StateEvent()
    {
        var windows = new JsonArray();
        foreach (var b in _browsers.Values)
        {
            windows.Add(new JsonObject
            {
                ["id"] = b.Id,
                ["parentId"] = b.ParentId,
                ["url"] = b.Url,
                ["closed"] = b.Closed
            });
        }
        var held = new JsonArray();
        foreach (var c in _inbound.Values.OrderBy(c => c.RequestId))
            held.Add(new JsonObject { ["requestId"] = c.RequestId, ["opcode"] = c.Opcode, ["held"] = c.Held });
        var cancels = new JsonArray();
        foreach (var c in _cancels)
            cancels.Add(c.DeepClone());
        var outstanding = new JsonArray();
        foreach (var c in _outgoing.Values.OrderBy(c => c.Sequence))
            outstanding.Add(new JsonObject { ["requestId"] = c.RequestId, ["opcode"] = c.Opcode, ["canceled"] = c.Canceled });
        var streams = new JsonArray();
        foreach (var s in _streams.Values)
            streams.Add(new JsonObject { ["streamId"] = s.Id, ["received"] = s.Received, ["granted"] = s.Granted });
        return new JsonObject
        {
            ["event"] = "state",
            ["windows"] = windows,
            ["pendingHeld"] = held,
            ["cancels"] = cancels,
            ["outstanding"] = outstanding,
            ["streams"] = streams,
            ["violations"] = _violations
        };
    }

    private void SpawnGrandchildAndExit(JsonObject o, JsonNode? tag)
    {
        var seconds = Json.Int(o, "seconds", 5);
        var pid = Grandchild.Spawn(seconds, _options);
        _log.Write($"spawned grandchild {pid} for {seconds} s, exiting");
        Emit(new JsonObject { ["event"] = "grandchildSpawned", ["pid"] = pid }, tag);
        _control?.Flush(TimeSpan.FromSeconds(1));
        Environment.Exit(0);
    }

    private void Shutdown(string reason)
    {
        if (_shuttingDown)
            return;
        _shuttingDown = true;
        _log.Write($"shutdown: {reason}");
        foreach (var b in _browsers.Values.Where(b => b.ParentId == 0 && !b.Closed).ToList())
            CloseBrowser(b);
        foreach (var b in _browsers.Values.Where(b => !b.Closed).ToList())
            CloseBrowser(b);
        Send(Encode.Exit());
        _writer.Close();
        var flushed = _writer.WaitClosed(TimeSpan.FromSeconds(5));
        Emit(new JsonObject { ["event"] = "exiting", ["reason"] = reason, ["code"] = 0, ["flushed"] = flushed });
        Terminate(0);
    }

    private void OnEof()
    {
        if (_shuttingDown)
            return;
        Emit(new JsonObject { ["event"] = "eof" });
        Emit(new JsonObject { ["event"] = "exiting", ["reason"] = "eof", ["code"] = 0 });
        Terminate(0);
    }

    private void OnFatal(string detail)
    {
        if (_shuttingDown)
            return;
        _shuttingDown = true;
        Violation(detail);
        _writer.Close();
        _writer.WaitClosed(TimeSpan.FromSeconds(1));
        Emit(new JsonObject { ["event"] = "exiting", ["reason"] = "fatal", ["code"] = 4 });
        Terminate(4);
    }

    private void Terminate(int code)
    {
        _control?.Flush(TimeSpan.FromSeconds(2));
        _log.Write($"exit {code}");
        Environment.Exit(code);
    }
}
