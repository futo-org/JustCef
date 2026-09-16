using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.JustCefProcess;

namespace JustCef.Tests.Unit;

[Trait("Category", "Unit")]
public class VectorTests
{
    private const uint Id = 7;
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(20);

    private sealed record Vector(string Name, string Direction, byte[] Bytes);

    private static readonly Lazy<Dictionary<string, Vector>> Vectors = new(Load);

    private static readonly HashSet<string> NotProducedByController = new()
    {
        "proxy_response_stream"
    };

    private static Dictionary<string, Vector> Load()
    {
        string path = Path.Combine(AppContext.BaseDirectory, "vectors.json");
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        var result = new Dictionary<string, Vector>();
        foreach (var element in document.RootElement.EnumerateArray())
        {
            var vector = new Vector(
                element.GetProperty("name").GetString()!,
                element.GetProperty("direction").GetString()!,
                Convert.FromHexString(element.GetProperty("hex").GetString()!));
            result.Add(vector.Name, vector);
        }
        return result;
    }

    private static byte[] Bytes(string name) => Vectors.Value[name].Bytes;

    private static byte[] WithRequestId(byte[] packet, uint requestId)
    {
        byte[] copy = packet.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(copy.AsSpan(4), requestId);
        return copy;
    }

    private static byte[] Raw(RawPacket packet) => Wire.Packet(packet.RequestId, packet.Type, packet.Opcode, packet.Body);

    private static byte[] Utf8(string value) => Encoding.UTF8.GetBytes(value);

    private static IPCResponse Response(int statusCode, string statusText, byte[]? body = null, IDataSource? dataSource = null, params (string Name, string Value)[] headers)
    {
        var map = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase);
        foreach (var (name, value) in headers)
            map[name] = new List<string> { value };
        return new IPCResponse { StatusCode = statusCode, StatusText = statusText, Headers = map, Body = body, DataSource = dataSource };
    }

    private static byte[] Call(Func<JustCefProcess, Task> call)
    {
        using var peer = new PipePeer();
        _ = call(peer.Process);
        return WithRequestId(Raw(peer.Take(PacketType.Request, Timeout)), Id);
    }

    private static async Task<JustCefWindow> CreateWindowAsync(PipePeer peer, int identifier = 1,
        Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy = null,
        Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier = null,
        Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler = null,
        Func<JustCefView, Task>? viewCreatedHandler = null)
    {
        var create = peer.Process.CreateWindowAsync("", 0, 0,
            proxyRequests: requestProxy != null, requestProxy: requestProxy,
            modifyRequests: requestModifier != null, requestModifier: requestModifier, modifyRequestBody: requestModifier != null,
            bridgeEnabled: bridgeRpcHandler != null, bridgeRpcHandler: bridgeRpcHandler,
            viewsEnabled: viewCreatedHandler != null, viewCreatedHandler: viewCreatedHandler);
        var request = peer.Take(PacketType.Request, Timeout);
        peer.Send(request.RequestId, PacketType.Response, request.Opcode, Wire.Ok(w => w.Write(identifier)));
        return await create.WaitAsync(Timeout);
    }

    private static byte[] Reply(Func<PipePeer, Task> setup, string nativeRequest)
    {
        using var peer = new PipePeer();
        setup(peer).WaitAsync(Timeout).GetAwaiter().GetResult();
        peer.SendRaw(Bytes(nativeRequest));
        return Raw(peer.Take(PacketType.Response, Timeout));
    }

    private static List<RawPacket> ProxyStream(IDataSource source, int requests = 1)
    {
        using var peer = new PipePeer();
        CreateWindowAsync(peer, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(Response(200, "OK", dataSource: new FixedBytesDataSource(Utf8("x")))))
            .GetAwaiter().GetResult();
        var packets = new List<RawPacket>();
        for (int i = 0; i < requests - 1; i++)
        {
            peer.SendRaw(WithRequestId(Bytes("proxy_request"), Id + 100 + (uint)i));
            peer.Take(PacketType.Response, Timeout);
        }

        peer.Process.GetWindow(1)!.SetRequestProxy((_, _) => Task.FromResult<IPCResponse?>(Response(200, "OK", dataSource: source)));
        peer.SendRaw(Bytes("proxy_request"));
        packets.Add(peer.Take(PacketType.Response, Timeout));
        while (true)
        {
            var packet = peer.Take(Timeout);
            if (packet.Type != PacketType.Notification || BinaryPrimitives.ReadUInt32LittleEndian(packet.Body) != (uint)requests)
                continue;
            packets.Add(packet);
            if (packet.Opcode != (byte)OpcodeControllerNotification.StreamData)
                return packets;
        }
    }

    private sealed class FailingSource : IDataSource
    {
        public int Read(Span<byte> buffer) => throw new IOException("source failed");
        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) => ValueTask.FromException<int>(new IOException("source failed"));
        public void Dispose()
        {
        }
    }

    private static readonly Dictionary<string, Func<byte[]>> Builders = new()
    {
        ["controller_exit"] = () =>
        {
            using var peer = new PipePeer();
            peer.Process.Dispose();
            return Raw(peer.Take(Timeout));
        },
        ["cancel_window_create"] = () =>
        {
            using var peer = new PipePeer();
            using var cts = new CancellationTokenSource();
            _ = peer.Process.CreateWindowAsync("", 0, 0, cancellationToken: cts.Token);
            peer.Take(PacketType.Request, Timeout);
            cts.Cancel();
            return WithRequestId(Raw(peer.Take(PacketType.Cancel, Timeout)), Id);
        },
        ["ping"] = () => Call(p => p.PingAsync()),
        ["print"] = () => Call(p => p.PrintAsync("hello")),
        ["echo_request"] = () => Call(p => p.EchoAsync(Utf8("ping"))),
        ["set_title_null"] = () => Call(p => p.WindowSetTitleAsync(1, null!)),
        ["set_title_empty"] = () => Call(p => p.WindowSetTitleAsync(1, "")),
        ["set_title_utf8"] = () => Call(p => p.WindowSetTitleAsync(1, "Grüße 日本 \U00010348")),
        ["window_create"] = () => Call(p => p.CreateWindowAsync("https://example.com/", 320, 240, 1024, 768,
            fullscreen: false, contextMenuEnable: false, shown: true, developerToolsEnabled: true, resizable: true, frameless: false,
            centered: true, proxyRequests: true, logConsole: false, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(null),
            modifyRequests: false, modifyRequestBody: false, title: "JustCef", iconPath: null, appId: "com.futo.justcef",
            bridgeEnabled: true, viewsEnabled: true, modifyTimeout: TimeSpan.FromSeconds(5), modifyTimeoutPolicy: ModifyTimeoutPolicy.Cancel,
            proxyOpenTimeout: TimeSpan.FromSeconds(30))),
        ["load_url"] = () => Call(p => p.WindowLoadUrlAsync(1, "https://example.com/next")),
        ["window_get_size"] = () => Call(p => p.WindowGetSizeAsync(1)),
        ["window_set_position"] = () => Call(p => p.WindowSetPositionAsync(1, -100, 200)),
        ["window_set_zoom"] = () => Call(p => p.WindowSetZoomAsync(1, -1.5)),
        ["window_close"] = () => Call(p => p.WindowCloseAsync(1, true)),
        ["set_modify_requests"] = () => Call(p => p.WindowSetModifyRequestsAsync(1, true, true)),
        ["pick_file"] = () => Call(p => p.WindowPickFileAsync(1, true, new[] { ("Images", "*.png;*.jpg"), ("All files", "*") })),
        ["save_file"] = () => Call(p => p.WindowSaveFileAsync(1, "report.pdf", new[] { ("PDF", "*.pdf") })),
        ["execute_devtools_method"] = () => Call(p => p.WindowExecuteDevToolsMethodAsync(1, "Browser.getVersion")),
        ["execute_devtools_method_params"] = () => Call(p => p.WindowExecuteDevToolsMethodAsync(1, "Page.navigate", "{\"url\":\"about:blank\"}")),
        ["bridge_rpc_request"] = () => Call(p => p.WindowBridgeRpcAsync(1, "add", "[1,2]")),
        ["response_not_handled"] = () => Reply(peer => CreateWindowAsync(peer, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(null)), "proxy_request"),
        ["response_too_large"] = () =>
        {
            using var peer = new PipePeer();
            peer.Send(Id, PacketType.Request, (byte)OpcodeClient.Echo, new byte[Wire.MaxBodySize]);
            return Raw(peer.Take(PacketType.Response, TimeSpan.FromSeconds(60)));
        },
        ["client_bridge_rpc_ok"] = () => Reply(peer => CreateWindowAsync(peer, bridgeRpcHandler: (_, method, json) =>
        {
            Assert.Equal("getUser", method);
            Assert.Equal("{\"id\":42}", json);
            return Task.FromResult<string?>("{\"name\":\"Ada\"}");
        }), "client_bridge_rpc_request"),
        ["client_bridge_rpc_error"] = () => Reply(peer => CreateWindowAsync(peer, bridgeRpcHandler: (_, _, _) => throw new InvalidOperationException("no such user")), "client_bridge_rpc_request"),
        ["view_created_ok"] = () => Reply(peer => CreateWindowAsync(peer, viewCreatedHandler: view =>
        {
            Assert.Equal(2, view.Identifier);
            Assert.Equal(1, view.Parent.Identifier);
            return Task.CompletedTask;
        }), "view_created_request"),
        ["native_echo_ok"] = () => Reply(_ => Task.CompletedTask, "native_echo_request"),
        ["proxy_response_no_body"] = () => Reply(peer => CreateWindowAsync(peer, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(Response(204, "No Content"))), "proxy_request"),
        ["proxy_response_inline"] = () => Reply(peer => CreateWindowAsync(peer, requestProxy: (_, request) =>
        {
            Assert.Equal("POST", request.Method);
            Assert.Equal("https://example.com/api", request.Url);
            Assert.Equal(new[] { "application/json" }, request.Headers["content-type"]);
            Assert.Equal(new[] { "*/*" }, request.Headers["Accept"]);
            Assert.Equal("{\"a\":1}", Encoding.UTF8.GetString(Assert.IsType<IPCProxyBodyElementBytes>(request.Elements[0]).Data));
            Assert.Equal("/tmp/upload.bin", Assert.IsType<IPCProxyBodyElementFile>(request.Elements[1]).FileName);
            return Task.FromResult<IPCResponse?>(Response(200, "OK", Utf8("hello"), null, ("Content-Type", "text/plain")));
        }), "proxy_request"),
        ["proxy_response_stream_unknown_length"] = () => Raw(ProxyStream(new FixedBytesDataSource(Utf8("0123456789")), requests: 2)[0]),
        ["modify_response"] = () => Reply(peer => CreateWindowAsync(peer, requestModifier: (_, request) =>
        {
            Assert.Equal("GET", request.Method);
            Assert.Equal("https://example.com/", request.Url);
            Assert.Equal(new[] { "JustCef" }, request.Headers["User-Agent"]);
            Assert.Empty(request.Elements);
            request.Method = "POST";
            request.Url = "https://example.com/changed";
            request.Headers["User-Agent"] = new List<string> { "Modified" };
            request.Headers["X-Added"] = new List<string> { "1" };
            request.Elements.Add(new IPCProxyBodyElementBytes(Utf8("abc")));
            request.Elements.Add(new IPCProxyBodyElementFile("/tmp/body.txt"));
            return request;
        }), "modify_request"),
        ["stream_data"] = () => Raw(ProxyStream(new FixedBytesDataSource(Utf8("0123456789")))[1]),
        ["stream_end"] = () => Raw(ProxyStream(new FixedBytesDataSource(Utf8("0123456789")))[2]),
        ["stream_error"] = () => Raw(ProxyStream(new FailingSource())[1]),
    };

    public static IEnumerable<object[]> ControllerVectorNames() => Vectors.Value.Values
        .Where(v => v.Direction == "controllerToNative" && !NotProducedByController.Contains(v.Name))
        .Select(v => new object[] { v.Name });

    [Fact]
    public void EveryControllerVectorHasABuilder()
    {
        var missing = ControllerVectorNames().Select(n => (string)n[0]).Where(name => !Builders.ContainsKey(name)).ToList();
        Assert.Empty(missing);
    }

    [Theory]
    [MemberData(nameof(ControllerVectorNames))]
    public void EncoderMatchesVector(string name)
    {
        TestUtil.QuietLogs();
        byte[] actual = Builders[name]();
        Assert.Equal(Convert.ToHexString(Bytes(name)), Convert.ToHexString(actual));
    }

    private static async Task<T> Respond<T>(PipePeer peer, Task<T> call, string vector)
    {
        var request = peer.Take(PacketType.Request, Timeout);
        peer.SendRaw(WithRequestId(Bytes(vector), request.RequestId));
        return await call.WaitAsync(Timeout);
    }

    private static async Task Respond(PipePeer peer, Task call, string vector)
    {
        var request = peer.Take(PacketType.Request, Timeout);
        peer.SendRaw(WithRequestId(Bytes(vector), request.RequestId));
        await call.WaitAsync(Timeout);
    }

    [Fact]
    public async Task DecodesResponses()
    {
        TestUtil.QuietLogs();
        using var peer = new PipePeer();
        var p = peer.Process;

        peer.SendRaw(Bytes("ready"));
        await p.WaitForReadyAsync().WaitAsync(Timeout);

        var create = p.CreateWindowAsync("", 0, 0);
        var createRequest = peer.Take(PacketType.Request, Timeout);
        peer.SendRaw(WithRequestId(Bytes("window_create_ok"), createRequest.RequestId));
        Assert.Equal(1, (await create.WaitAsync(Timeout)).Identifier);

        Assert.Equal((800, 600), await Respond(peer, p.WindowGetSizeAsync(1), "window_get_size_ok"));
        Assert.Equal(1.25, await Respond(peer, p.WindowGetZoomAsync(1), "window_get_zoom_ok"));
        Assert.Equal(new[] { "/home/user/a.png", "/home/user/b.jpg" }, await Respond(peer, p.WindowPickFileAsync(1, true, Array.Empty<(string, string)>()), "pick_file_ok"));
        Assert.Equal("/home/user/report.pdf", await Respond(peer, p.WindowSaveFileAsync(1, "", Array.Empty<(string, string)>()), "save_file_ok"));
        var devTools = await Respond(peer, p.WindowExecuteDevToolsMethodAsync(1, "m"), "execute_devtools_method_ok");
        Assert.True(devTools.Success);
        Assert.Equal("{}", Encoding.UTF8.GetString(devTools.Data));
        var widevine = await Respond(peer, p.GetWidevineStatusAsync(), "widevine_status_ok");
        Assert.Equal(WidevineComponentState.CanUpdate, widevine.State);
        Assert.Equal("4.10.2830.0", widevine.Version);
        Assert.True(widevine.Registered);
        Assert.True(widevine.Installed);
        Assert.False(widevine.RequiresRestart);
        Assert.Equal("3", await Respond(peer, p.WindowBridgeRpcAsync(1, "add"), "bridge_rpc_ok"));
        await Respond(peer, p.EchoAsync(new byte[] { 1, 2, 3 }), "echo_ok");
        await Respond(peer, p.WindowShowAsync(1), "response_ok_empty");

        foreach (var (vector, status, message) in new (string, JustCefStatus, string?)[]
        {
            ("response_error", JustCefStatus.Error, "boom"),
            ("response_canceled", JustCefStatus.Canceled, null),
            ("response_not_found", JustCefStatus.NotFound, null),
            ("response_unsupported", JustCefStatus.Unsupported, "unknown opcode"),
            ("response_shutting_down", JustCefStatus.ShuttingDown, null),
            ("response_invalid_request", JustCefStatus.InvalidRequest, "truncated body")
        })
        {
            Task call = vector == "response_unsupported"
                ? p.CallAsync((JustCefProcess.OpcodeController)99, new PacketWriter())
                : p.WindowShowAsync(1);
            var exception = await Assert.ThrowsAsync<JustCefRemoteException>(() => Respond(peer, call, vector));
            Assert.Equal(status, exception.Status);
            if (message != null)
                Assert.Equal(message, exception.Message);
        }

        peer.SendRaw(Bytes("native_exit"));
        await p.WaitForExitAsync().WaitAsync(Timeout);
    }

    [Fact]
    public async Task DecodesNotifications()
    {
        TestUtil.QuietLogs();
        using var peer = new PipePeer();
        var window = await CreateWindowAsync(peer);
        var log = new List<string>();
        var closed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.OnFullscreenChanged += fullscreen => log.Add($"fullscreen:{fullscreen}");
        window.OnLoadingStateChanged += info => log.Add($"loading:{info.IsLoading}:{info.CanGoBack}:{info.CanGoForward}");
        window.OnFrameLoadStart += info => log.Add($"start:{info.FrameIdentifier}:{info.IsMainFrame}:{info.Url}");
        window.OnFrameLoadEnd += info => log.Add($"end:{info.FrameIdentifier}:{info.IsMainFrame}:{info.Url}:{info.HttpStatusCode}");
        window.OnFrameLoadError += info => log.Add($"error:{info.FrameIdentifier}:{info.IsMainFrame}:{info.ErrorCode}:{info.ErrorText}:{info.FailedUrl}");
        window.OnDevToolsEvent += (method, data) => log.Add($"devtools:{method}:{Encoding.UTF8.GetString(data)}");
        window.OnClose += () => closed.TrySetResult();

        foreach (var name in new[] { "window_opened", "debug_notification", "fullscreen_changed", "loading_state_changed", "frame_load_start", "frame_load_end", "frame_load_error", "devtools_event", "window_closed" })
            peer.SendRaw(Bytes(name));

        await closed.Task.WaitAsync(Timeout);
        Assert.Equal(new[]
        {
            "fullscreen:True",
            "loading:True:False:True",
            "start:main:True:https://example.com/",
            "end:main:True:https://example.com/:200",
            "error:main:True:-105:net::ERR_NAME_NOT_RESOLVED:https://nonexistent.invalid/",
            "devtools:Network.requestWillBeSent:{\"requestId\":\"1\"}"
        }, log);
        Assert.Null(peer.Process.GetBrowser(1));
    }

    [Fact]
    public async Task DecodesStreamCreditAndCancel()
    {
        TestUtil.QuietLogs();
        using var peer = new PipePeer();
        var source = new PatternDataSource(long.MaxValue / 2);
        await CreateWindowAsync(peer, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(Response(200, "OK", dataSource: source)));
        peer.SendRaw(Bytes("proxy_request"));
        peer.Take(PacketType.Response, Timeout);

        long received = 0;
        while (received < 1024 * 1024)
            received += peer.Take(Timeout).Body.Length - sizeof(uint);
        Assert.Equal(1024 * 1024, received);
        Assert.False(peer.Received.TryTake(out _, TimeSpan.FromMilliseconds(300)));

        peer.SendRaw(Bytes("stream_credit"));
        received = 0;
        while (received < 65536)
            received += peer.Take(Timeout).Body.Length - sizeof(uint);
        Assert.Equal(65536, received);

        peer.SendRaw(Bytes("stream_cancel"));
        await source.Disposed.Task.WaitAsync(Timeout);
        await TestUtil.WaitUntil(() => peer.Process.OpenStreamCount == 0, Timeout, "the stream was released");
    }

    [Fact]
    public async Task DecodesCancelForNativeRequest()
    {
        TestUtil.QuietLogs();
        using var peer = new PipePeer();
        var release = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
        await CreateWindowAsync(peer, bridgeRpcHandler: (_, _, _) => release.Task);
        peer.SendRaw(Bytes("client_bridge_rpc_request"));
        await TestUtil.WaitUntil(() => peer.Process.InflightHandlerCount == 1, Timeout, "the handler started");
        peer.SendRaw(Bytes("cancel_client_bridge_rpc"));

        var response = peer.Take(PacketType.Response, Timeout);
        Assert.Equal(Id, response.RequestId);
        Assert.Equal((byte)OpcodeClient.WindowBridgeRpc, response.Opcode);
        Assert.Equal(Wire.Failure(JustCefStatus.Canceled, null), response.Body);

        release.TrySetResult("1");
        Assert.False(peer.Received.TryTake(out _, TimeSpan.FromMilliseconds(300)));
    }
}
