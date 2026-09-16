namespace FakeNative;

public sealed class WindowCreateArgs
{
    public bool Resizable;
    public bool Frameless;
    public bool Fullscreen;
    public bool Centered;
    public bool Shown;
    public bool ContextMenuEnable;
    public bool DeveloperToolsEnabled;
    public bool ModifyRequests;
    public bool ModifyRequestBody;
    public bool ProxyRequests;
    public bool LogConsole;
    public bool BridgeEnabled;
    public int MinimumWidth;
    public int MinimumHeight;
    public int PreferredWidth;
    public int PreferredHeight;
    public string? Url;
    public string? Title;
    public string? IconPath;
    public string? AppId;
    public bool ViewsEnabled;
    public uint ModifyTimeoutMs;
    public byte ModifyTimeoutPolicy;
    public uint ProxyOpenTimeoutMs;
}

public sealed class ControllerRequest
{
    public ControllerOp Op;
    public int? BrowserId;
    public bool Flag;
    public string? Text;
    public int X;
    public int Y;
    public double Level;
    public byte Flags;
    public ReadOnlyMemory<byte> Data;
    public readonly List<(string? Name, string? Pattern)> Filters = new();
    public WindowCreateArgs? Create;
}

public sealed class ControllerNotificationData
{
    public ControllerNotification Op;
    public uint StreamId;
    public ReadOnlyMemory<byte> Data;
    public ulong TotalBytes;
    public string? Message;
}

public sealed record HttpHeader(string? Name, string? Value);

public sealed class BodyElement
{
    public byte Type;
    public ReadOnlyMemory<byte> Data;
    public string? Path;
}

public sealed class HttpRequestData
{
    public string? Method;
    public string? Url;
    public readonly List<HttpHeader> Headers = new();
    public readonly List<BodyElement> Elements = new();
}

public sealed class ProxyResponse
{
    public uint StatusCode;
    public string? StatusText;
    public readonly List<HttpHeader> Headers = new();
    public byte BodyType;
    public ReadOnlyMemory<byte> Data;
    public long Length;
    public uint StreamId;
}

public sealed class ResponseHead
{
    public Status Status;
    public string? Message;
    public required BodyReader Payload;
}

public static class Codec
{
    public static bool IsKnownControllerOp(byte opcode) => Enum.IsDefined(typeof(ControllerOp), opcode);

    public static bool IsBrowserTargeted(ControllerOp op) => op switch
    {
        ControllerOp.Ping or ControllerOp.Print or ControllerOp.Echo or ControllerOp.WindowCreate or ControllerOp.GetWidevineStatus or ControllerOp.Debug => false,
        _ => true
    };

    public static ControllerRequest ParseControllerRequest(byte opcode, byte[] body)
    {
        if (!IsKnownControllerOp(opcode))
            throw new ProtocolException($"unknown controller opcode {opcode}");
        var op = (ControllerOp)opcode;
        var r = new BodyReader(body);
        var req = new ControllerRequest { Op = op };
        if (IsBrowserTargeted(op))
            req.BrowserId = r.I32();
        switch (op)
        {
            case ControllerOp.Ping:
            case ControllerOp.GetWidevineStatus:
                break;
            case ControllerOp.Print:
            case ControllerOp.Echo:
            case ControllerOp.Debug:
                req.Data = r.Rest();
                break;
            case ControllerOp.WindowCreate:
                req.Create = ParseWindowCreate(r);
                break;
            case ControllerOp.WindowSetDevelopmentToolsEnabled:
            case ControllerOp.WindowClose:
            case ControllerOp.WindowSetAlwaysOnTop:
            case ControllerOp.WindowSetFullscreen:
            case ControllerOp.WindowSetProxyRequests:
            case ControllerOp.WindowSetDevelopmentToolsVisible:
                req.Flag = r.Bool();
                break;
            case ControllerOp.WindowLoadUrl:
            case ControllerOp.WindowSetTitle:
            case ControllerOp.WindowSetIcon:
            case ControllerOp.WindowAddUrlToProxy:
            case ControllerOp.WindowRemoveUrlToProxy:
            case ControllerOp.WindowAddUrlToModify:
            case ControllerOp.WindowRemoveUrlToModify:
            case ControllerOp.WindowAddDevToolsEventMethod:
            case ControllerOp.WindowRemoveDevToolsEventMethod:
            case ControllerOp.WindowAddDomainToProxy:
            case ControllerOp.WindowRemoveDomainToProxy:
                req.Text = r.Str();
                break;
            case ControllerOp.WindowSetZoom:
                req.Level = r.F64();
                break;
            case ControllerOp.WindowSetPosition:
            case ControllerOp.WindowSetSize:
                req.X = r.I32();
                req.Y = r.I32();
                break;
            case ControllerOp.WindowSetModifyRequests:
                req.Flags = r.U8();
                if (req.Flags > 3)
                    throw new ProtocolException($"modify flags {req.Flags} use undefined bits");
                break;
            case ControllerOp.PickFile:
                req.Flag = r.Bool();
                ReadFilters(r, req);
                break;
            case ControllerOp.SaveFile:
                req.Text = r.Str();
                ReadFilters(r, req);
                break;
            case ControllerOp.WindowExecuteDevToolsMethod:
                req.Text = r.Str();
                req.Flag = r.Bool();
                if (req.Flag)
                    req.Data = r.Bytes();
                break;
            case ControllerOp.WindowBridgeRpc:
                req.Text = r.Str();
                req.Data = r.Bytes();
                break;
            default:
                break;
        }
        r.End();
        return req;
    }

    private static void ReadFilters(BodyReader r, ControllerRequest req)
    {
        var count = r.Count(8, "filter");
        for (var i = 0; i < count; i++)
            req.Filters.Add((r.Str(), r.Str()));
    }

    private static WindowCreateArgs ParseWindowCreate(BodyReader r)
    {
        var a = new WindowCreateArgs
        {
            Resizable = r.Bool(),
            Frameless = r.Bool(),
            Fullscreen = r.Bool(),
            Centered = r.Bool(),
            Shown = r.Bool(),
            ContextMenuEnable = r.Bool(),
            DeveloperToolsEnabled = r.Bool(),
            ModifyRequests = r.Bool(),
            ModifyRequestBody = r.Bool(),
            ProxyRequests = r.Bool(),
            LogConsole = r.Bool(),
            BridgeEnabled = r.Bool(),
            MinimumWidth = r.I32(),
            MinimumHeight = r.I32(),
            PreferredWidth = r.I32(),
            PreferredHeight = r.I32(),
            Url = r.Str(),
            Title = r.Str(),
            IconPath = r.Str(),
            AppId = r.Str(),
            ViewsEnabled = r.Bool(),
            ModifyTimeoutMs = r.U32(),
            ModifyTimeoutPolicy = r.U8(),
            ProxyOpenTimeoutMs = r.U32()
        };
        if (a.ModifyTimeoutPolicy > 1)
            throw new ProtocolException($"modifyTimeoutPolicy {a.ModifyTimeoutPolicy} is not 0 or 1");
        return a;
    }

    public static ControllerNotificationData ParseControllerNotification(byte opcode, byte[] body)
    {
        if (!Enum.IsDefined(typeof(ControllerNotification), opcode))
            throw new ProtocolException($"unknown controller notification opcode {opcode}");
        var r = new BodyReader(body);
        var n = new ControllerNotificationData { Op = (ControllerNotification)opcode };
        switch (n.Op)
        {
            case ControllerNotification.Exit:
                break;
            case ControllerNotification.StreamData:
                n.StreamId = r.U32();
                n.Data = r.Rest();
                break;
            case ControllerNotification.StreamEnd:
                n.StreamId = r.U32();
                n.TotalBytes = r.U64();
                break;
            case ControllerNotification.StreamError:
                n.StreamId = r.U32();
                n.Message = r.Str();
                break;
        }
        r.End();
        if (n.Op != ControllerNotification.Exit && n.StreamId == 0)
            throw new ProtocolException("streamId 0 is reserved");
        return n;
    }

    public static ResponseHead ParseResponseHead(byte[] body)
    {
        var r = new BodyReader(body);
        var status = r.U8();
        if (status > (byte)Status.InvalidRequest)
            throw new ProtocolException($"unknown status {status}");
        var head = new ResponseHead { Status = (Status)status, Payload = r };
        if (head.Status != Status.Ok)
        {
            head.Message = r.Str();
            r.End();
        }
        return head;
    }

    public static ProxyResponse ParseProxyResponse(BodyReader r)
    {
        var p = new ProxyResponse
        {
            StatusCode = r.U32(),
            StatusText = r.Str()
        };
        var count = r.Count(8, "header");
        for (var i = 0; i < count; i++)
            p.Headers.Add(new HttpHeader(r.Str(), r.Str()));
        p.BodyType = r.U8();
        switch (p.BodyType)
        {
            case 0:
                break;
            case 1:
                p.Data = r.Bytes();
                p.Length = p.Data.Length;
                break;
            case 2:
                p.Length = r.I64();
                p.StreamId = r.U32();
                if (p.Length < -1)
                    throw new ProtocolException($"stream length {p.Length} is invalid");
                if (p.StreamId == 0)
                    throw new ProtocolException("streamId 0 is reserved");
                break;
            default:
                throw new ProtocolException($"unknown proxy body type {p.BodyType}");
        }
        r.End();
        return p;
    }

    public static HttpRequestData ParseModifyResponse(BodyReader r)
    {
        var req = new HttpRequestData
        {
            Method = r.Str(),
            Url = r.Str()
        };
        var headerCount = r.Count(8, "header");
        for (var i = 0; i < headerCount; i++)
            req.Headers.Add(new HttpHeader(r.Str(), r.Str()));
        var elementCount = r.Count(5, "element");
        for (var i = 0; i < elementCount; i++)
            req.Elements.Add(ReadElement(r));
        r.End();
        return req;
    }

    public static HttpRequestData ParseHttpRequest(BodyReader r)
    {
        var req = new HttpRequestData
        {
            Method = r.Str(),
            Url = r.Str()
        };
        var headerCount = r.SignedCount(8, "header");
        for (var i = 0; i < headerCount; i++)
            req.Headers.Add(new HttpHeader(r.Str(), r.Str()));
        var elementCount = r.SignedCount(5, "element");
        for (var i = 0; i < elementCount; i++)
            req.Elements.Add(ReadElement(r));
        return req;
    }

    private static BodyElement ReadElement(BodyReader r)
    {
        var e = new BodyElement { Type = r.U8() };
        switch (e.Type)
        {
            case 1:
                e.Data = r.Bytes();
                break;
            case 2:
                e.Path = r.Str();
                break;
            default:
                throw new ProtocolException($"unknown body element type {e.Type}");
        }
        return e;
    }

    public static ReadOnlyMemory<byte> ParseBridgeResponse(BodyReader r)
    {
        var json = r.Bytes();
        r.End();
        return json;
    }

    public static bool ParseViewCreatedResponse(BodyReader r)
    {
        var allow = r.Bool();
        r.End();
        return allow;
    }

    public static void ValidateClientResponse(byte opcode, BodyReader payload)
    {
        switch ((ClientOp)opcode)
        {
            case ClientOp.Ping:
            case ClientOp.Print:
                payload.End();
                break;
            case ClientOp.Echo:
                payload.Rest();
                break;
            case ClientOp.WindowProxyRequest:
                ParseProxyResponse(payload);
                break;
            case ClientOp.WindowModifyRequest:
                ParseModifyResponse(payload);
                break;
            case ClientOp.WindowBridgeRpc:
                ParseBridgeResponse(payload);
                break;
            case ClientOp.WindowViewCreated:
                ParseViewCreatedResponse(payload);
                break;
            default:
                throw new ProtocolException($"unknown client opcode {opcode}");
        }
    }
}

public static class Encode
{
    public static ReadOnlyMemory<byte> Notification(ClientNotification op, Action<PacketBuilder>? body = null, int capacity = 16)
    {
        var b = new PacketBuilder(PacketType.Notification, (byte)op, 0, capacity);
        body?.Invoke(b);
        return b.Finish();
    }

    public static ReadOnlyMemory<byte> Ready(uint version) => Notification(ClientNotification.Ready, b => b.U32(version));
    public static ReadOnlyMemory<byte> Exit() => Notification(ClientNotification.Exit);
    public static ReadOnlyMemory<byte> WindowOpened(int id) => Notification(ClientNotification.WindowOpened, b => b.I32(id));
    public static ReadOnlyMemory<byte> WindowClosed(int id) => Notification(ClientNotification.WindowClosed, b => b.I32(id));
    public static ReadOnlyMemory<byte> WindowFocused(int id) => Notification(ClientNotification.WindowFocused, b => b.I32(id));
    public static ReadOnlyMemory<byte> WindowUnfocused(int id) => Notification(ClientNotification.WindowUnfocused, b => b.I32(id));

    public static ReadOnlyMemory<byte> FullscreenChanged(int id, bool fullscreen) =>
        Notification(ClientNotification.WindowFullscreenChanged, b => b.I32(id).Bool(fullscreen));

    public static ReadOnlyMemory<byte> FrameLoadStart(int id, string? frameId, bool isMain, string? url) =>
        Notification(ClientNotification.WindowFrameLoadStart, b => b.I32(id).Str(frameId).Bool(isMain).Str(url), 64);

    public static ReadOnlyMemory<byte> FrameLoadEnd(int id, string? frameId, bool isMain, string? url, int httpStatusCode) =>
        Notification(ClientNotification.WindowFrameLoadEnd, b => b.I32(id).Str(frameId).Bool(isMain).Str(url).I32(httpStatusCode), 64);

    public static ReadOnlyMemory<byte> FrameLoadError(int id, string? frameId, bool isMain, int errorCode, string? errorText, string? url) =>
        Notification(ClientNotification.WindowFrameLoadError, b => b.I32(id).Str(frameId).Bool(isMain).I32(errorCode).Str(errorText).Str(url), 96);

    public static ReadOnlyMemory<byte> DevToolsEvent(int id, string? method, ReadOnlyMemory<byte> parameters) =>
        Notification(ClientNotification.WindowDevToolsEvent, b => b.I32(id).Str(method).Bytes(parameters.Span), 64 + parameters.Length);

    public static ReadOnlyMemory<byte> LoadingStateChanged(int id, bool isLoading, bool canGoBack, bool canGoForward) =>
        Notification(ClientNotification.WindowLoadingStateChanged, b => b.I32(id).Bool(isLoading).Bool(canGoBack).Bool(canGoForward));

    public static ReadOnlyMemory<byte> StreamCredit(uint streamId, uint bytes) =>
        Notification(ClientNotification.StreamCredit, b => b.U32(streamId).U32(bytes));

    public static ReadOnlyMemory<byte> StreamCancel(uint streamId) =>
        Notification(ClientNotification.StreamCancel, b => b.U32(streamId));

    public static ReadOnlyMemory<byte> Debug(uint sequence) =>
        Notification(ClientNotification.Debug, b => b.U32(sequence));

    public static ReadOnlyMemory<byte> Response(uint requestId, byte opcode, Status status, string? message)
    {
        var b = new PacketBuilder(PacketType.Response, opcode, requestId, 16 + (message?.Length ?? 0) * 3);
        b.U8((byte)status);
        if (status != Status.Ok)
            b.Str(message);
        return b.Finish();
    }

    public static ReadOnlyMemory<byte> ResponseOk(uint requestId, byte opcode, Action<PacketBuilder>? payload = null, int capacity = 16)
    {
        var b = new PacketBuilder(PacketType.Response, opcode, requestId, capacity + 1);
        b.U8((byte)Status.Ok);
        payload?.Invoke(b);
        return b.Finish();
    }

    public static ReadOnlyMemory<byte> Cancel(uint requestId, byte opcode) =>
        new PacketBuilder(PacketType.Cancel, opcode, requestId, 0).Finish();

    public static ReadOnlyMemory<byte> Request(ClientOp op, uint requestId, Action<PacketBuilder>? body = null, int capacity = 64)
    {
        var b = new PacketBuilder(PacketType.Request, (byte)op, requestId, capacity);
        body?.Invoke(b);
        return b.Finish();
    }

    private static void WriteHttpRequest(PacketBuilder b, HttpRequestData req)
    {
        b.Str(req.Method).Str(req.Url);
        b.I32(req.Headers.Count);
        foreach (var h in req.Headers)
            b.Str(h.Name).Str(h.Value);
        b.I32(req.Elements.Count);
        foreach (var e in req.Elements)
        {
            b.U8(e.Type);
            if (e.Type == 1)
                b.Bytes(e.Data.Span);
            else
                b.Str(e.Path);
        }
    }

    private static int EstimateSize(HttpRequestData req)
    {
        var size = 64 + (req.Url?.Length ?? 0) * 3;
        foreach (var h in req.Headers)
            size += 8 + ((h.Name?.Length ?? 0) + (h.Value?.Length ?? 0)) * 3;
        foreach (var e in req.Elements)
            size += 8 + e.Data.Length + (e.Path?.Length ?? 0) * 3;
        return size;
    }

    public static ReadOnlyMemory<byte> ProxyRequest(uint requestId, int browserId, HttpRequestData req) =>
        Request(ClientOp.WindowProxyRequest, requestId, b => WriteHttpRequest(b.I32(browserId), req), EstimateSize(req));

    public static ReadOnlyMemory<byte> ModifyRequest(uint requestId, int browserId, HttpRequestData req) =>
        Request(ClientOp.WindowModifyRequest, requestId, b => WriteHttpRequest(b.I32(browserId), req), EstimateSize(req));

    public static ReadOnlyMemory<byte> BridgeRpc(uint requestId, int browserId, string? method, ReadOnlyMemory<byte> json) =>
        Request(ClientOp.WindowBridgeRpc, requestId, b => b.I32(browserId).Str(method).Bytes(json.Span), 64 + json.Length);

    public static ReadOnlyMemory<byte> ViewCreated(uint requestId, int parentId, int viewId, string? src) =>
        Request(ClientOp.WindowViewCreated, requestId, b => b.I32(parentId).I32(viewId).Str(src), 64 + (src?.Length ?? 0) * 3);

    public static ReadOnlyMemory<byte> EchoRequest(uint requestId, ReadOnlyMemory<byte> data) =>
        Request(ClientOp.Echo, requestId, b => b.Raw(data.Span), data.Length);
}
